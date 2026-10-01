// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "skill_lua.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include <common/core.hpp>      // db_path
#include <common/random.hpp>    // rnd
#include <common/showmsg.hpp>

#include "../../3rdparty/lua/src/lua.h"
#include "../../3rdparty/lua/src/lauxlib.h"
#include "../../3rdparty/lua/src/lualib.h"

#include "battle.hpp"
#include "itemdb.hpp"
#include "map.hpp"
#include "mob.hpp"
#include "pc.hpp"
#include "script.hpp"
#include "skill.hpp"
#include "status.hpp"
#include "unit.hpp"
#include "skills/skill_impl.hpp"

namespace {

// ---------------------------------------------------------------------------
// The state and its limits
// ---------------------------------------------------------------------------

lua_State* L = nullptr;

// A script can allocate this much in total, and run this many thousand
// instructions per call. Both are far beyond any formula; they exist so a
// mistake (an endless loop, a table that grows forever) costs one hook, not
// the map server.
constexpr size_t MEMORY_LIMIT = 64 * 1024 * 1024;
constexpr int32 CALL_BUDGET = 1000;       // x1000 instructions, per hook call
constexpr int32 LOAD_BUDGET = 20000;      // x1000 instructions, per file
size_t memory_used = 0;
int32 budget = 0;

// The mod whose code is running, for the log.
std::string current_mod;

void* limited_alloc(void*, void* ptr, size_t osize, size_t nsize) {
	size_t old = ptr != nullptr ? osize : 0;

	if (nsize == 0) {
		std::free(ptr);
		memory_used -= old;
		return nullptr;
	}
	if (memory_used - old + nsize > MEMORY_LIMIT)
		return nullptr;  // Lua turns this into a "not enough memory" error

	void* grown = std::realloc(ptr, nsize);

	if (grown != nullptr)
		memory_used = memory_used - old + nsize;
	return grown;
}

void count_hook(lua_State* state, lua_Debug*) {
	if (--budget <= 0)
		luaL_error(state, "stopped after %d thousand instructions", CALL_BUDGET);
}

// ---------------------------------------------------------------------------
// Hooks, by skill name and then by id
// ---------------------------------------------------------------------------

enum e_hook : uint8 { HOOK_RATIO = 0, HOOK_HIT, HOOK_ELEMENT, HOOK_ON_HIT, HOOK_MAX };
const char* const hook_names[HOOK_MAX] = { "ratio", "hit", "element", "on_hit" };

struct s_hook {
	int32 ref = LUA_NOREF;
	std::string mod;
};

struct s_skill_hooks {
	std::string skill;
	s_hook hooks[HOOK_MAX];
};

std::unordered_map<std::string, s_skill_hooks> hooks_by_name;
std::unordered_map<uint16, s_skill_hooks*> hooks_by_id;

// item("Aegis_Name", { on_attack = ..., on_hit_taken = ... })
struct s_item_hooks {
	std::string item;
	s_hook on_attack;
	s_hook on_hit_taken;
};

std::unordered_map<std::string, s_item_hooks> item_hooks_by_name;
std::unordered_map<t_itemid, s_item_hooks*> item_hooks_by_id;

// While a skill cast from Lua (c:cast) runs, no Lua hook runs: an item whose
// on_attack casts a skill that hits would otherwise trigger itself forever.
bool in_lua_cast = false;

void cast_like_autospell(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv);

s_skill_hooks* hooks_for(uint16 skill_id) {
	if (L == nullptr || hooks_by_id.empty())
		return nullptr;

	auto it = hooks_by_id.find(skill_id);

	return it == hooks_by_id.end() ? nullptr : it->second;
}

int32 message_handler(lua_State* state) {
	luaL_traceback(state, state, lua_tostring(state, 1), 1);
	return 1;
}

/// Call the function under `nargs` arguments with `nresults` results. On an
/// error the hook is switched off -- a broken hook fails once, loudly, rather
/// than on every hit -- and false is returned with the stack cleaned up.
bool protected_call(s_hook& hook, const char* skill, int32 nargs, int32 nresults) {
	int32 base = lua_gettop(L) - nargs;

	lua_pushcfunction(L, message_handler);
	lua_insert(L, base);
	current_mod = hook.mod;
	budget = CALL_BUDGET;

	int32 status = lua_pcall(L, nargs, nresults, base);

	lua_remove(L, base);

	if (status == LUA_OK)
		return true;

	ShowError("Lua: %s's hook for %s failed and is now off until the server restarts:\n%s\n", hook.mod.c_str(), skill, lua_tostring(L, -1));
	lua_pop(L, 1);
	luaL_unref(L, LUA_REGISTRYINDEX, hook.ref);
	hook.ref = LUA_NOREF;
	return false;
}

// ---------------------------------------------------------------------------
// What a hook sees: c.caster, c.target, c.skill_lv, ...
// ---------------------------------------------------------------------------

const char* unit_kind(const block_list& bl) {
	switch (bl.type) {
		case BL_PC: return "pc";
		case BL_MOB: return "mob";
		case BL_HOM: return "homun";
		case BL_MER: return "merc";
		case BL_ELEM: return "elemental";
		case BL_PET: return "pet";
		case BL_NPC: return "npc";
		default: return "other";
	}
}

void set_int(const char* name, lua_Integer value) {
	lua_pushinteger(L, value);
	lua_setfield(L, -2, name);
}

sc_type status_named(lua_State* state, int32 arg) {
	const char* name = luaL_checkstring(state, arg);
	int64 value;

	if (!script_get_constant(name, &value) || value <= SC_NONE || value >= SC_MAX)
		luaL_error(state, "%s is not a status (use a name like SC_STUN)", name);
	return static_cast<sc_type>(value);
}

// unit:has_status("SC_BLESSING")
int32 lua_unit_has_status(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	sc_type type = status_named(state, 2);
	lua_getfield(state, 1, "id");
	block_list* bl = map_id2bl(static_cast<int32>(lua_tointeger(state, -1)));
	const status_change* sc = bl != nullptr ? status_get_sc(bl) : nullptr;

	lua_pushboolean(state, sc != nullptr && sc->hasSCE(type));
	return 1;
}

void push_unit(const block_list* bl) {
	if (bl == nullptr) {
		lua_pushnil(L);
		return;
	}

	const status_data* st = status_get_status_data(*bl);

	lua_createtable(L, 0, 24);
	set_int("id", bl->id);
	lua_pushstring(L, unit_kind(*bl));
	lua_setfield(L, -2, "kind");
	lua_pushstring(L, status_get_name(*bl));
	lua_setfield(L, -2, "name");
	set_int("level", status_get_lv(bl));
	set_int("str", st->str);
	set_int("agi", st->agi);
	set_int("vit", st->vit);
	set_int("int", st->int_);
	set_int("dex", st->dex);
	set_int("luk", st->luk);
	set_int("hp", st->hp);
	set_int("maxhp", st->max_hp);
	set_int("sp", st->sp);
	set_int("maxsp", st->max_sp);
	set_int("race", st->race);
	set_int("element", st->def_ele);
	set_int("size", st->size);
	lua_pushboolean(L, st->class_ == CLASS_BOSS);
	lua_setfield(L, -2, "boss");
	lua_pushboolean(L, status_isdead(*bl));
	lua_setfield(L, -2, "dead");

	if (const map_session_data* sd = BL_CAST(BL_PC, bl); sd != nullptr) {
		set_int("job", sd->status.class_);
		set_int("job_level", sd->status.job_level);
		// Item bonuses a hook might want to honour on a skill that stock
		// rAthena does not apply them to.
		set_int("classchange", sd->bonus.classchange);
	} else if (const mob_data* md = BL_CAST(BL_MOB, bl); md != nullptr) {
		set_int("mob_id", md->mob_id);
	}

	lua_pushcfunction(L, lua_unit_has_status);
	lua_setfield(L, -2, "has_status");
}

// The hit an on_hit hook is running for. Actions record into it; outside
// on_hit it is null and they refuse.
s_skill_lua_hit* current_hit = nullptr;

s_skill_lua_hit& hit_or_error(lua_State* state, const char* what) {
	if (current_hit == nullptr)
		luaL_error(state, "c:%s() only works inside on_hit", what);
	return *current_hit;
}

int32 unit_arg(lua_State* state, int32 arg, const s_skill_lua_hit& hit) {
	const char* who = luaL_optstring(state, arg, "target");

	if (strcmp(who, "target") == 0)
		return hit.target_id;
	if (strcmp(who, "caster") == 0)
		return hit.src_id;
	return luaL_error(state, "who must be \"target\" or \"caster\", not \"%s\"", who);
}

// c:drain() -- the caster's HP/SP drain item bonuses, on this hit's damage.
int32 lua_hit_drain(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "drain");
	hit.actions.push_back({ SKILL_LUA_DRAIN, hit.src_id });
	return 0;
}

// c:heal(hp, sp) -- restores the caster.
int32 lua_hit_heal(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "heal");
	s_skill_lua_action action = { SKILL_LUA_HEAL, hit.src_id };

	action.hp = std::max<lua_Integer>(0, luaL_checkinteger(state, 2));
	action.sp = std::max<lua_Integer>(0, luaL_optinteger(state, 3, 0));
	hit.actions.push_back(action);
	return 0;
}

// c:status("SC_STUN", rate, duration_ms, {val1}, {who}) -- rate out of 10000.
int32 lua_hit_status(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "status");
	s_skill_lua_action action = { SKILL_LUA_STATUS };

	action.type = status_named(state, 2);
	action.rate = static_cast<int32>(std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 0, 10000));
	action.duration = std::clamp<lua_Integer>(luaL_checkinteger(state, 4), 0, 3600000);
	action.val1 = static_cast<int32>(luaL_optinteger(state, 5, 1));
	action.unit_id = unit_arg(state, 6, hit);
	hit.actions.push_back(action);
	return 0;
}

// c:polymorph() -- what Hylozoist Card does: turn a monster target into a
// random one from the Dead Branch list. Bosses and status-immune monsters
// are never changed.
int32 lua_hit_polymorph(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "polymorph");
	hit.actions.push_back({ SKILL_LUA_POLYMORPH, hit.target_id });
	return 0;
}

// c:cast("MG_FIREBOLT", 3, {who}) -- cast a skill the way bAutoSpell does:
// at "target" (default) or "caster", with the skill's own checks and delays.
int32 lua_hit_cast(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "cast");
	uint16 skill_id = lua_type(state, 2) == LUA_TNUMBER ? (uint16)lua_tointeger(state, 2) : skill_name2id(luaL_checkstring(state, 2));

	if (skill_id == 0 || skill_get_index(skill_id) == 0)
		return luaL_error(state, "c:cast: there is no skill %s (use the AegisName, like MG_FIREBOLT)", luaL_tolstring(state, 2, nullptr));

	s_skill_lua_action action = { SKILL_LUA_CAST };

	action.type = skill_id;
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_optinteger(state, 3, 1), 1, MAX_SKILL_LEVEL));
	action.unit_id = unit_arg(state, 4, hit);
	hit.actions.push_back(action);
	return 0;
}

// c:chance(n) -- true n times in 10000, from the server's own random numbers.
int32 lua_hit_chance(lua_State* state) {
	lua_Integer n = luaL_checkinteger(state, 2);
	lua_pushboolean(state, rnd() % 10000 < n);
	return 1;
}

void push_context(uint16 skill_id, uint16 skill_lv, const block_list* src, const block_list* target, const int64* damage, bool actions = false) {
	lua_createtable(L, 0, 16);
	if (skill_id != 0)
		lua_pushstring(L, skill_get_name(skill_id));
	else
		lua_pushnil(L);  // a normal attack
	lua_setfield(L, -2, "skill");
	set_int("skill_id", skill_id);
	set_int("skill_lv", skill_lv);
	push_unit(src);
	lua_setfield(L, -2, "caster");
	push_unit(target);
	lua_setfield(L, -2, "target");
	lua_pushcfunction(L, lua_hit_chance);
	lua_setfield(L, -2, "chance");

	if (damage != nullptr)
		set_int("damage", *damage);
	if (actions) {
		lua_pushcfunction(L, lua_hit_cast);
		lua_setfield(L, -2, "cast");
		lua_pushcfunction(L, lua_hit_drain);
		lua_setfield(L, -2, "drain");
		lua_pushcfunction(L, lua_hit_heal);
		lua_setfield(L, -2, "heal");
		lua_pushcfunction(L, lua_hit_status);
		lua_setfield(L, -2, "status");
		lua_pushcfunction(L, lua_hit_polymorph);
		lua_setfield(L, -2, "polymorph");
	}
}

/// ratio, hit and element: `value` is the stock result on the way in and the
/// hook's on the way out. A hook that returns nil keeps the stock value.
void call_number_hook(e_hook which, uint16 skill_id, uint16 skill_lv, const block_list* src, const block_list* target, int64& value) {
	s_skill_hooks* hooks = hooks_for(skill_id);

	if (hooks == nullptr || hooks->hooks[which].ref == LUA_NOREF)
		return;

	s_hook& hook = hooks->hooks[which];

	lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
	push_context(skill_id, skill_lv, src, target, nullptr);
	lua_pushinteger(L, value);
	if (!protected_call(hook, hooks->skill.c_str(), 2, 1))
		return;

	if (lua_isinteger(L, -1)) {
		value = lua_tointeger(L, -1);
	} else if (lua_type(L, -1) == LUA_TNUMBER) {
		value = static_cast<int64>(lua_tonumber(L, -1));  // toward zero, as the C++ would
	} else if (!lua_isnil(L, -1)) {
		ShowError("Lua: %s's %s hook for %s returned a %s, not a number; keeping the stock value.\n", hook.mod.c_str(), hook_names[which], hooks->skill.c_str(), luaL_typename(L, -1));
	}
	lua_pop(L, 1);
}

// ---------------------------------------------------------------------------
// The wrapper around a skill's own class
// ---------------------------------------------------------------------------

class LuaSkillImpl : public SkillImpl {
	std::unique_ptr<const SkillImpl> stock_;

public:
	LuaSkillImpl(e_skill skill_id, std::unique_ptr<const SkillImpl> stock) : SkillImpl(skill_id), stock_(std::move(stock)) {}

	void castendNoDamageId(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32& flag) const override {
		stock_->castendNoDamageId(src, target, skill_lv, tick, flag);
	}
	void castendDamageId(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32& flag) const override {
		stock_->castendDamageId(src, target, skill_lv, tick, flag);
	}
	void castendPos2(block_list* src, int32 x, int32 y, uint16 skill_lv, t_tick tick, int32& flag) const override {
		stock_->castendPos2(src, x, y, skill_lv, tick, flag);
	}
	void applyAdditionalEffects(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32 attack_type, enum damage_lv dmg_lv) const override {
		stock_->applyAdditionalEffects(src, target, skill_lv, tick, attack_type, dmg_lv);
	}
	void applyCounterAdditionalEffects(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32& attack_type) const override {
		stock_->applyCounterAdditionalEffects(src, target, skill_lv, tick, attack_type);
	}
	void modifyDamageData(Damage& dmg, const block_list& src, const block_list& target, uint16 skill_lv) const override {
		stock_->modifyDamageData(dmg, src, target, skill_lv);
	}

	void calculateSkillRatio(const Damage* wd, const block_list* src, const block_list* target, uint16 skill_lv, int32& base_skillratio, int32 mflag) const override {
		stock_->calculateSkillRatio(wd, src, target, skill_lv, base_skillratio, mflag);

		int64 value = base_skillratio;

		call_number_hook(HOOK_RATIO, skill_id_, skill_lv, src, target, value);
		base_skillratio = static_cast<int32>(std::clamp<int64>(value, 0, INT32_MAX));
	}

	void modifyHitRate(int16& hit_rate, const block_list* src, const block_list* target, uint16 skill_lv) const override {
		stock_->modifyHitRate(hit_rate, src, target, skill_lv);

		int64 value = hit_rate;

		call_number_hook(HOOK_HIT, skill_id_, skill_lv, src, target, value);
		hit_rate = static_cast<int16>(std::clamp<int64>(value, INT16_MIN, INT16_MAX));
	}

	void modifyElement(const Damage& dmg, const block_list& src, const block_list& target, uint16 skill_lv, int32& element, int32 flag) const override {
		stock_->modifyElement(dmg, src, target, skill_lv, element, flag);

		int64 value = element;

		call_number_hook(HOOK_ELEMENT, skill_id_, skill_lv, &src, &target, value);
		if (value >= ELE_NEUTRAL && value < ELE_ALL)
			element = static_cast<int32>(value);
	}
};

// ---------------------------------------------------------------------------
// The functions a mod's file can call while it loads
// ---------------------------------------------------------------------------

// skill("NJ_KAENSIN", { ratio = ..., on_hit = ... })
// A later file replaces only the hooks it names, so two mods can each hook
// a different part of one skill.
int32 lua_register_skill(lua_State* state) {
	const char* name = luaL_checkstring(state, 1);
	luaL_checktype(state, 2, LUA_TTABLE);

	s_skill_hooks& entry = hooks_by_name[name];
	entry.skill = name;

	lua_pushnil(state);
	while (lua_next(state, 2) != 0) {
		const char* key = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : "";
		int32 which = -1;

		for (int32 i = 0; i < HOOK_MAX; i++)
			if (strcmp(key, hook_names[i]) == 0)
				which = i;
		if (which < 0)
			return luaL_error(state, "skill %s: unknown hook \"%s\" (ratio, hit, element or on_hit)", name, key);
		if (!lua_isfunction(state, -1))
			return luaL_error(state, "skill %s: %s must be a function", name, key);

		s_hook& hook = entry.hooks[which];

		luaL_unref(state, LUA_REGISTRYINDEX, hook.ref);
		hook.ref = luaL_ref(state, LUA_REGISTRYINDEX);  // pops the value
		hook.mod = current_mod;
	}
	return 0;
}

// item("Moon_Ribbon", { on_attack = ..., on_hit_taken = ... })
// As with skill(), a later file replaces only the hooks it names.
int32 lua_register_item(lua_State* state) {
	const char* name = luaL_checkstring(state, 1);
	luaL_checktype(state, 2, LUA_TTABLE);

	s_item_hooks& entry = item_hooks_by_name[name];
	entry.item = name;

	lua_pushnil(state);
	while (lua_next(state, 2) != 0) {
		const char* key = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : "";
		s_hook* hook = strcmp(key, "on_attack") == 0 ? &entry.on_attack : strcmp(key, "on_hit_taken") == 0 ? &entry.on_hit_taken : nullptr;

		if (hook == nullptr)
			return luaL_error(state, "item %s: unknown hook \"%s\" (on_attack or on_hit_taken)", name, key);
		if (!lua_isfunction(state, -1))
			return luaL_error(state, "item %s: %s must be a function", name, key);
		luaL_unref(state, LUA_REGISTRYINDEX, hook->ref);
		hook->ref = luaL_ref(state, LUA_REGISTRYINDEX);
		hook->mod = current_mod;
	}
	return 0;
}

// const("SC_STUN") -> the number the server uses for it. Any script constant.
int32 lua_constant(lua_State* state) {
	const char* name = luaL_checkstring(state, 1);
	int64 value;

	if (!script_get_constant(name, &value))
		return luaL_error(state, "%s is not a constant the server knows", name);
	lua_pushinteger(state, value);
	return 1;
}

// log(...) and print(...): to the map server's log, tagged with the mod.
int32 lua_log(lua_State* state) {
	std::string line;

	for (int32 i = 1, n = lua_gettop(state); i <= n; i++) {
		if (i > 1)
			line += ' ';
		line += luaL_tolstring(state, i, nullptr);
		lua_pop(state, 1);
	}
	ShowInfo("Lua: %s: %s\n", current_mod.c_str(), line.c_str());
	return 0;
}

// Pure Lua, run once before any mod: setting() reads the MOD_SETTINGS table
// that the app writes, and the base library loses what could read files.
const char* PRELUDE = R"lua(
dofile, loadfile, load, collectgarbage = nil, nil, nil, nil
function setting(mod, key, default)
  local m = MOD_SETTINGS and MOD_SETTINGS[mod]
  local v = m and m[key]
  if v == nil then return default end
  return v
end
)lua";

bool run_file(const std::string& path, const std::string& mod) {
	current_mod = mod;
	budget = LOAD_BUDGET;
	lua_pushcfunction(L, message_handler);

	// "t": text only. Precompiled chunks can crash the VM on purpose.
	int32 status = luaL_loadfilex(L, path.c_str(), "t");

	if (status == LUA_OK)
		status = lua_pcall(L, 0, 0, -2);
	if (status != LUA_OK) {
		ShowError("Lua: %s: could not load %s:\n%s\n", mod.c_str(), path.c_str(), lua_tostring(L, -1));
		lua_pop(L, 1);
	}
	lua_pop(L, 1);  // the message handler
	return status == LUA_OK;
}

/// The files to run, in order, as (mod, path). load.txt is "<mod>\t<path>"
/// per line, the path relative to db/import; the app writes it in mod order.
std::vector<std::pair<std::string, std::string>> file_list() {
	std::string dir = std::string(db_path) + "/import/lua";
	std::vector<std::pair<std::string, std::string>> files;
	std::ifstream list(dir + "/load.txt");

	if (list) {
		std::string line;

		while (std::getline(list, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			size_t tab = line.find('\t');
			if (line.empty() || line[0] == '#' || tab == std::string::npos)
				continue;

			std::string rel = line.substr(tab + 1);

			if (rel.find("..") != std::string::npos) {
				ShowWarning("Lua: skipping %s in load.txt: it leaves db/import.\n", rel.c_str());
				continue;
			}
			files.emplace_back(line.substr(0, tab), std::string(db_path) + "/import/" + rel);
		}
		return files;
	}

	std::error_code ec;

	for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
		if (entry.is_regular_file() && entry.path().extension() == ".lua")
			files.emplace_back("lua", entry.path().string());
	std::sort(files.begin(), files.end());
	return files;
}

} // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void do_init_skill_lua() {
	auto files = file_list();

	if (files.empty())
		return;  // no mod uses Lua: no state, and every hook check is one null test

	L = lua_newstate(limited_alloc, nullptr);
	if (L == nullptr) {
		ShowError("Lua: could not create a Lua state.\n");
		return;
	}

	// The sandbox: arithmetic, strings, tables and utf8. No io, os, package
	// (require) or debug, and they are not even compiled in (lua_vm.cpp).
	luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
	luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
	luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
	luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
	luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
	lua_settop(L, 0);

	lua_register(L, "skill", lua_register_skill);
	lua_register(L, "item", lua_register_item);
	lua_register(L, "const", lua_constant);
	lua_register(L, "log", lua_log);
	lua_register(L, "print", lua_log);
	lua_sethook(L, count_hook, LUA_MASKCOUNT, 1000);

	current_mod = "server";
	budget = LOAD_BUDGET;
	if (luaL_dostring(L, PRELUDE) != LUA_OK) {
		ShowError("Lua: prelude failed: %s\n", lua_tostring(L, -1));
		do_final_skill_lua();
		return;
	}

	size_t loaded = 0;

	for (const auto& file : files)
		loaded += run_file(file.second, file.first) ? 1 : 0;

	// Items are named by AegisName; the item database is loaded by now.
	for (auto& it : item_hooks_by_name) {
		std::shared_ptr<item_data> item = item_db.search_aegisname(it.first.c_str());

		if (item == nullptr) {
			ShowWarning("Lua: there is no item called %s (use the AegisName, like Red_Potion).\n", it.first.c_str());
			continue;
		}
		item_hooks_by_id[item->nameid] = &it.second;
	}

	ShowStatus("Lua: loaded %zu of %zu file(s), hooks for %zu skill(s) and %zu item(s).\n", loaded, files.size(), hooks_by_name.size(), item_hooks_by_id.size());
	skill_lua_attach();
}

void do_final_skill_lua() {
	hooks_by_id.clear();
	hooks_by_name.clear();
	item_hooks_by_id.clear();
	item_hooks_by_name.clear();
	current_hit = nullptr;
	if (L != nullptr) {
		lua_close(L);
		L = nullptr;
	}
	memory_used = 0;
}

void skill_lua_attach() {
	hooks_by_id.clear();
	if (L == nullptr || hooks_by_name.empty())
		return;

	std::unordered_map<std::string, bool> found;

	for (auto& it : skill_db) {
		std::shared_ptr<s_skill_db>& skill = it.second;
		auto hooks = hooks_by_name.find(skill->name);

		if (hooks == hooks_by_name.end())
			continue;
		found[skill->name] = true;
		hooks_by_id[skill->nameid] = &hooks->second;

		const s_skill_hooks& h = hooks->second;
		bool wants_class = h.hooks[HOOK_RATIO].ref != LUA_NOREF || h.hooks[HOOK_HIT].ref != LUA_NOREF || h.hooks[HOOK_ELEMENT].ref != LUA_NOREF;

		if (!wants_class || dynamic_cast<const LuaSkillImpl*>(skill->impl.get()) != nullptr)
			continue;
		if (skill->impl == nullptr) {
			// Its formula is still in battle.cpp's switch, which has no hook.
			ShowWarning("Lua: %s has no skill class in this server, so its ratio, hit and element hooks cannot run (on_hit still does).\n", skill->name);
			continue;
		}
		skill->impl = std::make_unique<LuaSkillImpl>(static_cast<e_skill>(skill->nameid), std::move(skill->impl));
	}

	for (const auto& it : hooks_by_name)
		if (!found.count(it.first))
			ShowWarning("Lua: there is no skill called %s (use the AegisName, like MG_FIREBOLT).\n", it.first.c_str());
}

std::unique_ptr<s_skill_lua_hit> skill_lua_on_hit(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, int64 damage, int32 attack_type) {
	s_skill_hooks* hooks = hooks_for(skill_id);

	if (hooks == nullptr || hooks->hooks[HOOK_ON_HIT].ref == LUA_NOREF || src == nullptr || target == nullptr || current_hit != nullptr || in_lua_cast)
		return nullptr;

	auto hit = std::make_unique<s_skill_lua_hit>();
	const status_data* tstatus = status_get_status_data(*target);

	hit->src_id = src->id;
	hit->target_id = target->id;
	hit->skill_id = skill_id;
	hit->damage = damage;
	hit->race = tstatus->race;
	hit->class_ = tstatus->class_;

	s_hook& hook = hooks->hooks[HOOK_ON_HIT];

	lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
	push_context(skill_id, skill_lv, src, target, &damage, true);
	current_hit = hit.get();
	protected_call(hook, hooks->skill.c_str(), 1, 0);
	current_hit = nullptr;

	if (hit->actions.empty())
		return nullptr;
	return hit;
}

void skill_lua_apply(std::unique_ptr<s_skill_lua_hit>& hit) {
	if (hit == nullptr)
		return;

	block_list* src = map_id2bl(hit->src_id);

	for (const s_skill_lua_action& action : hit->actions) {
		block_list* bl = map_id2bl(action.unit_id);

		switch (action.kind) {
			case SKILL_LUA_DRAIN:
				if (src != nullptr && src->type == BL_PC && hit->damage > 0) {
					block_list* target = map_id2bl(hit->target_id);
					if (target != nullptr)
						battle_drain(reinterpret_cast<map_session_data*>(src), target, hit->damage, 0, hit->race, hit->class_);
				}
				break;
			case SKILL_LUA_HEAL:
				if (bl != nullptr && !status_isdead(*bl))
					status_heal(bl, action.hp, action.sp, 0, 0);
				break;
			case SKILL_LUA_STATUS:
				if (bl != nullptr && !status_isdead(*bl))
					sc_start(src != nullptr ? src : bl, bl, static_cast<sc_type>(action.type), action.rate, action.val1, static_cast<t_tick>(action.duration));
				break;
			case SKILL_LUA_CAST:
				if (src != nullptr && bl != nullptr && !status_isdead(*src) && !status_isdead(*bl))
					cast_like_autospell(src, bl, static_cast<uint16>(action.type), static_cast<uint16>(action.val1));
				break;
			case SKILL_LUA_POLYMORPH:
				if (bl != nullptr && bl->type == BL_MOB && !status_isdead(*bl)) {
					mob_data* md = reinterpret_cast<mob_data*>(bl);
					const status_data* st = status_get_status_data(*bl);

					if (st->class_ != CLASS_BOSS && !status_has_mode(st, MD_STATUSIMMUNE)) {
						int32 class_ = mob_get_random_id(MOBG_BRANCH_OF_DEAD_TREE, RMF_DB_RATE, 0);
						if (class_ != 0 && mobdb_checkid(class_))
							mob_class_change(md, class_);
					}
				}
				break;
		}
	}
	hit.reset();
}

namespace {

/// Cast a skill the way bAutoSpell does (skill_additional_effect): the same
/// "can this be cast here" check, ground-skill limit, item requirements and
/// after-cast delay. No Lua hook runs while it does (in_lua_cast).
void cast_like_autospell(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv) {
	map_session_data* sd = BL_CAST(BL_PC, src);
	t_tick tick = gettick();

	if (sd != nullptr) {
		sd->state.autocast = 1;
		bool refused = skill_isNotOk(skill_id, *sd);
		sd->state.autocast = 0;
		if (refused)
			return;
	}

	e_cast_type type = skill_get_casttype(skill_id);

	if (type == CAST_GROUND && !skill_pos_maxcount_check(src, target->x, target->y, skill_id, skill_lv, BL_PC, false))
		return;
	if (skill_id == PF_SPIDERWEB)
		type = CAST_GROUND;

	in_lua_cast = true;
	if (sd != nullptr) {
		sd->state.autocast = 1;
		skill_consume_requirement(sd, skill_id, skill_lv, 1);
	}
	switch (type) {
		case CAST_GROUND:
			skill_castend_pos2(src, target->x, target->y, skill_id, skill_lv, tick, 0);
			break;
		case CAST_NODAMAGE:
			skill_castend_nodamage_id(src, target, skill_id, skill_lv, tick, 0);
			break;
		case CAST_DAMAGE:
			skill_castend_damage_id(src, target, skill_id, skill_lv, tick, 0);
			break;
	}
	if (sd != nullptr)
		sd->state.autocast = 0;
	in_lua_cast = false;

	if (unit_data* ud = unit_bl2ud(src); ud != nullptr) {
		int32 delay = skill_delayfix(src, skill_id, skill_lv);

		if (DIFF_TICK(ud->canact_tick, tick + delay) < 0)
			ud->canact_tick = i64max(tick + delay, ud->canact_tick);
	}
}

/// Run one item hook kind for every hooked item `wearer` has equipped, cards
/// in equipped items included. `other` is whoever is on the other end of the
/// hit; to the hook, the wearer is c.caster and `other` is c.target.
void run_item_hooks(s_hook s_item_hooks::*which, block_list* wearer, block_list* other, uint16 skill_id, uint16 skill_lv, int32 attack_type) {
	if (L == nullptr || item_hooks_by_id.empty() || in_lua_cast || current_hit != nullptr || wearer == nullptr || other == nullptr || status_isdead(*wearer))
		return;

	map_session_data* sd = BL_CAST(BL_PC, wearer);

	if (sd == nullptr)
		return;

	// Gathered first: a hook's actions can change what is equipped.
	std::vector<std::pair<t_itemid, s_item_hooks*>> hooked;

	for (int32 i = 0; i < EQI_MAX; i++) {
		int16 index = sd->equip_index[i];

		if (index < 0)
			continue;

		bool seen = false;  // a two-handed weapon fills two slots

		for (int32 j = 0; j < i; j++)
			seen = seen || sd->equip_index[j] == index;
		if (seen)
			continue;

		const item& equipped = sd->inventory.u.items_inventory[index];
		auto check = [&](t_itemid id) {
			if (auto it = item_hooks_by_id.find(id); it != item_hooks_by_id.end() && (it->second->*which).ref != LUA_NOREF)
				hooked.emplace_back(id, it->second);
		};

		check(equipped.nameid);
		if (!itemdb_isspecial(equipped.card[0]))
			for (int32 c = 0; c < MAX_SLOTS; c++)
				if (equipped.card[c] != 0)
					check(equipped.card[c]);
	}

	for (const auto& [id, hooks] : hooked) {
		s_hook& hook = hooks->*which;

		if (hook.ref == LUA_NOREF)
			continue;  // switched off by an error earlier in this loop

		auto hit = std::make_unique<s_skill_lua_hit>();
		const status_data* other_status = status_get_status_data(*other);

		hit->src_id = wearer->id;
		hit->target_id = other->id;
		hit->skill_id = skill_id;
		hit->damage = 0;
		hit->race = other_status->race;
		hit->class_ = other_status->class_;

		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, skill_lv, wearer, other, nullptr, true);
		lua_pushstring(L, hooks->item.c_str());
		lua_setfield(L, -2, "item");
		set_int("item_id", id);
		lua_pushstring(L, attack_type & BF_MAGIC ? "magic" : attack_type & BF_MISC ? "misc" : "weapon");
		lua_setfield(L, -2, "attack");
		lua_pushboolean(L, (attack_type & BF_LONG) != 0);
		lua_setfield(L, -2, "ranged");

		current_hit = hit.get();
		protected_call(hook, hooks->item.c_str(), 1, 0);
		current_hit = nullptr;
		skill_lua_apply(hit);
	}
}

} // namespace

void skill_lua_item_attack(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, int32 attack_type) {
	run_item_hooks(&s_item_hooks::on_attack, src, target, skill_id, skill_lv, attack_type);
}

void skill_lua_item_hit_taken(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, int32 attack_type) {
	run_item_hooks(&s_item_hooks::on_hit_taken, target, src, skill_id, skill_lv, attack_type);
}
