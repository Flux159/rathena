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
#include <unordered_set>

#include <common/core.hpp>      // db_path
#include <common/random.hpp>    // rnd
#include <common/showmsg.hpp>
#include <common/utils.hpp>     // MakeDWord

#include "../../3rdparty/lua/src/lua.h"
#include "../../3rdparty/lua/src/lauxlib.h"
#include "../../3rdparty/lua/src/lualib.h"

#include "battle.hpp"
#include "clif.hpp"
#include "elemental.hpp"
#include "homunculus.hpp"
#include "itemdb.hpp"
#include "log.hpp"
#include "map.hpp"
#include "mercenary.hpp"
#include "mob.hpp"
#include "mod_store.hpp"
#include "party.hpp"
#include "path.hpp"
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

// The memory limit holds while a mod's code runs (protected_call, run_file),
// where an error costs that hook. The server's own allocations for a hook --
// the tables it builds and reads back -- happen outside, where an error has
// nothing to catch it and would end the map server, so they are not refused.
int32 protected_depth = 0;

// The mod whose code is running, for the log.
std::string current_mod;

// True while the mods' files run. skill(), item() and status() register only
// then, so a hook cannot change the lists of hooks it is being called from.
bool loading = false;

void* limited_alloc(void*, void* ptr, size_t osize, size_t nsize) {
	size_t old = ptr != nullptr ? osize : 0;

	if (nsize == 0) {
		std::free(ptr);
		memory_used -= old;
		return nullptr;
	}
	if (protected_depth > 0 && memory_used - old + nsize > MEMORY_LIMIT)
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

enum e_hook : uint8 {
	HOOK_RATIO = 0, HOOK_HIT, HOOK_ELEMENT, HOOK_ON_HIT, HOOK_ON_STEAL, HOOK_LEVEL, HOOK_JOB_LEVEL, HOOK_HEAL,
	// About a use of this skill
	HOOK_ON_CAST, HOOK_CAN_CAST, HOOK_COST, HOOK_BEFORE_CAST, HOOK_HITS, HOOK_CRITICAL, HOOK_RANGE,
	// For every player who has learned this skill (see LEARNER_HOOKS)
	HOOK_BONUS, HOOK_ON_ATTACK, HOOK_ON_HIT_TAKEN, HOOK_DAMAGE_DEALT, HOOK_DAMAGE_TAKEN, HOOK_ON_KILL, HOOK_ON_DEATH, HOOK_ON_SKILL, HOOK_TICK,
	HOOK_STATS, HOOK_CRAFT, HOOK_ON_CRAFT, HOOK_ON_USE,
	HOOK_MAX
};
const char* const hook_names[HOOK_MAX] = {
	"ratio", "hit", "element", "on_hit", "on_steal", "level", "job_level", "heal",
	"on_cast", "can_cast", "cost", "before_cast", "hits", "critical", "range",
	"bonus", "on_attack", "on_hit_taken", "damage_dealt", "damage_taken", "on_kill", "on_death", "on_skill", "tick",
	"stats", "craft", "on_craft", "on_use",
};

/// The hooks that fire for a player who has learned the skill, whatever that
/// player is doing: a passive's effect.
constexpr e_hook LEARNER_HOOKS[] = { HOOK_BONUS, HOOK_ON_ATTACK, HOOK_ON_HIT_TAKEN, HOOK_DAMAGE_DEALT, HOOK_DAMAGE_TAKEN, HOOK_ON_KILL, HOOK_ON_DEATH, HOOK_ON_SKILL, HOOK_TICK,
	HOOK_STATS, HOOK_CRAFT, HOOK_ON_CRAFT, HOOK_ON_USE };

enum e_item_hook : uint8 { ITEM_HOOK_ON_ATTACK = 0, ITEM_HOOK_ON_HIT_TAKEN, ITEM_HOOK_ON_USE, ITEM_HOOK_MAX };
const char* const item_hook_names[ITEM_HOOK_MAX] = { "on_attack", "on_hit_taken", "on_use" };

constexpr int32 PRIORITY_MIN = 0;
constexpr int32 PRIORITY_MAX = 10;
constexpr int32 PRIORITY_DEFAULT = 5;

/// One mod's registration for one hook on one skill. Several of these may
/// live on the same (skill, hook) when more than one mod wants in: they run
/// in ascending `priority` order, ties broken by `load_order` (the order
/// their files were run, which is the mod's alphabetical place).
struct s_hook {
	int32 ref = LUA_NOREF;
	std::string mod;
	int32 priority = PRIORITY_DEFAULT;
	uint32 load_order = 0;
};

struct s_skill_hooks {
	std::string skill;
	std::vector<s_hook> hooks[HOOK_MAX];
	bool custom = false;   ///< a mod's own skill: give it the generic class if it has none
	bool replace = false;  ///< use the generic class even over the server's own
	bool stock_checks = true;  ///< false: skip the server's skill-specific use checks
};

/// Same shape as s_skill_hooks, keyed by item AegisName; mod registrations
/// are kept here until startup resolves each name to its nameid.
struct s_item_hooks {
	std::string aegis;
	std::vector<s_hook> hooks[ITEM_HOOK_MAX];
};

/// Monotonic counter so a late-registering hook keeps its place behind
/// earlier ones at the same priority.
uint32 next_load_order = 0;

std::unordered_map<std::string, s_skill_hooks> hooks_by_name;
std::unordered_map<uint16, s_skill_hooks*> hooks_by_id;

/// For each learner hook, the skills that have one; filled by skill_lua_attach.
std::vector<uint16> learner_skills[HOOK_MAX];

s_skill_hooks* hooks_for(uint16 skill_id) {
	if (L == nullptr || hooks_by_id.empty())
		return nullptr;

	auto it = hooks_by_id.find(skill_id);

	return it == hooks_by_id.end() ? nullptr : it->second;
}

} // namespace

bool skill_lua_stock_checks(uint16 skill_id) {
	const s_skill_hooks* h = hooks_for(skill_id);

	return h == nullptr || h->stock_checks;
}

namespace {

std::unordered_map<std::string, s_item_hooks> item_hooks_by_name;
std::unordered_map<t_itemid, s_item_hooks*> item_hooks_by_id;

// While a skill cast from Lua (c:cast, c:attack) runs, no hook that can act
// on what it does runs: not the attack hooks, nor the skill's before_cast,
// on_cast, on_skill, heal or on_steal. An item whose on_attack casts a skill
// that hits would otherwise trigger itself forever. Its damage still goes
// through ratio, damage_dealt and the like.
bool in_lua_cast = false;

s_item_hooks* item_hooks_for(t_itemid nameid) {
	if (L == nullptr || item_hooks_by_id.empty() || nameid == 0)
		return nullptr;

	auto it = item_hooks_by_id.find(nameid);

	return it == item_hooks_by_id.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Statuses a mod adds: status("TC_TOXIN", { ... })
//
// They are kept here, not as server status changes: the server's statuses
// are an enum compiled into it, and their effects are code. A mod's status is
// a name, stacks, a duration and hooks. It lives in memory, so it ends with a
// restart, a logout or the unit leaving; with persist = true, a player's is
// kept in the mod store over a logout or a restart.
// ---------------------------------------------------------------------------

enum e_status_hook : uint8 {
	SHOOK_ON_START = 0, SHOOK_ON_END, SHOOK_BONUS, SHOOK_STATS, SHOOK_DAMAGE_DEALT, SHOOK_DAMAGE_TAKEN, SHOOK_ON_ATTACK, SHOOK_ON_HIT_TAKEN, SHOOK_TICK,
	SHOOK_ON_KILL, SHOOK_ON_DEATH, SHOOK_ON_SKILL, SHOOK_COST, SHOOK_CAN_CAST, SHOOK_CRITICAL, SHOOK_CRAFT, SHOOK_ON_CRAFT, SHOOK_ON_USE,
	SHOOK_RANGE,
	SHOOK_CHANCE,  ///< a server status only: the chance it lands
	SHOOK_MAX
};
const char* const status_hook_names[SHOOK_MAX] = {
	"on_start", "on_end", "bonus", "stats", "damage_dealt", "damage_taken", "on_attack", "on_hit_taken", "tick",
	"on_kill", "on_death", "on_skill", "cost", "can_cast", "critical", "craft", "on_craft", "on_use",
	"range",
	"chance",
};

/// The status hooks a server status (status("SC_...")) and the wildcard
/// status("*") take; the others belong to a mod's own statuses.
bool stock_status_hook(int32 which) {
	return which == SHOOK_ON_START || which == SHOOK_ON_END || which == SHOOK_CHANCE;
}

/// A status a mod defined, or a stock status (`sc` set) whose start and end a
/// mod hooks.
struct s_lua_status {
	std::string name;
	std::string mod;               ///< the mod that defined it (the last one, if several did)
	int32 index = -1;              ///< in status_list; what actions refer to
	sc_type sc = SC_NONE;          ///< a stock status: on_start and on_end only
	int32 icon = EFST_BLANK;       ///< what a player sees while it lasts
	int32 duration = 0;            ///< ms; 0 lasts until it is removed
	int32 max_stacks = 1;
	int32 max_stacks_boss = -1;    ///< -1: max_stacks
	int32 max_stacks_player = -1;  ///< -1: max_stacks
	bool per_source = false;       ///< one instance for each unit that applied it
	bool refresh = true;           ///< a new application restarts the duration
	bool end_on_death = true;
	int32 decay_every = 0;         ///< ms between losing stacks; 0: never
	int32 decay_stacks = 1;
	bool persist = false;          ///< a player keeps it over a logout (mod store)
	std::vector<s_hook> hooks[SHOOK_MAX];
};

std::unordered_map<std::string, s_lua_status> statuses_by_name;
std::vector<s_lua_status*> status_list;                      ///< by index
std::unordered_map<int32, s_lua_status*> stock_status_hooks; ///< by sc_type
s_lua_status* any_status_hooks = nullptr;                    ///< status("*"): every server status

/// A mod status on a unit.
struct s_lua_status_on {
	int32 index;      ///< s_lua_status::index
	int32 source_id;  ///< who applied it: the latest one, unless per_source
	int32 stacks;
	t_tick expires;   ///< 0: never
	t_tick next_decay;
};

/// The mod statuses on each unit, by unit id.
std::unordered_map<int32, std::vector<s_lua_status_on>> statuses_on;

/// A mod's own status by name; nullptr for a stock status or an unknown name.
s_lua_status* custom_status(const char* name) {
	auto it = statuses_by_name.find(name);

	if (it == statuses_by_name.end() || it->second.sc != SC_NONE)
		return nullptr;
	return &it->second;
}

/// How many stacks of `index` a unit carries; from `source_id` only, unless 0.
int32 status_stacks_on(int32 unit_id, int32 index, int32 source_id) {
	auto it = statuses_on.find(unit_id);

	if (it == statuses_on.end())
		return 0;

	int32 total = 0;

	for (const s_lua_status_on& on : it->second)
		if (on.index == index && (source_id == 0 || on.source_id == source_id))
			total += on.stacks;
	return total;
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

	// A hook can run inside another's call (a range hook, for the other's
	// unit:skill_range): the other goes on as its own mod, with its budget.
	std::string outer_mod = current_mod;
	int32 outer_budget = budget;

	current_mod = hook.mod;
	budget = CALL_BUDGET;

	protected_depth++;

	int32 status = lua_pcall(L, nargs, nresults, base);

	protected_depth--;
	lua_remove(L, base);
	current_mod = outer_mod;
	budget = outer_budget;

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

void set_bool(const char* name, bool value) {
	lua_pushboolean(L, value);
	lua_setfield(L, -2, name);
}

void set_string(const char* name, const char* value) {
	lua_pushstring(L, value);
	lua_setfield(L, -2, name);
}

sc_type status_named(lua_State* state, int32 arg) {
	const char* name = luaL_checkstring(state, arg);
	int64 value;

	if (!script_get_constant(name, &value) || value <= SC_NONE || value >= SC_MAX)
		luaL_error(state, "%s is not a status (use a name like SC_STUN)", name);
	return static_cast<sc_type>(value);
}

/// The id of the unit table at `arg` (a c.caster, a c.target, a party
/// member...), or 0.
int32 unit_id_at(lua_State* state, int32 arg) {
	if (!lua_istable(state, arg))
		return 0;
	lua_getfield(state, arg, "id");
	int32 id = static_cast<int32>(lua_tointeger(state, -1));
	lua_pop(state, 1);
	return id;
}

// unit:has_status("SC_BLESSING") or unit:has_status("TC_TOXIN"), a mod's status
int32 lua_unit_has_status(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	int32 id = unit_id_at(state, 1);

	if (const s_lua_status* def = custom_status(luaL_checkstring(state, 2)); def != nullptr) {
		lua_pushboolean(state, status_stacks_on(id, def->index, 0) > 0);
		return 1;
	}

	sc_type type = status_named(state, 2);
	block_list* bl = map_id2bl(id);
	const status_change* sc = bl != nullptr ? status_get_sc(bl) : nullptr;

	lua_pushboolean(state, sc != nullptr && sc->hasSCE(type));
	return 1;
}

// unit:status_stacks("TC_TOXIN", {source}) -- stacks of a mod's status, all
// of them or only those `source` (a unit table) applied.
int32 lua_unit_status_stacks(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	const char* name = luaL_checkstring(state, 2);
	const s_lua_status* def = custom_status(name);

	if (def == nullptr)
		return luaL_error(state, "%s is not a status a mod has defined", name);
	lua_pushinteger(state, status_stacks_on(unit_id_at(state, 1), def->index, unit_id_at(state, 3)));
	return 1;
}

// unit:skill_lv("AS_KATAR") -- the level the unit has of a skill; 0 for a
// skill it does not have, and for anything that is not a player.
int32 lua_unit_skill_lv(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	uint16 skill_id = lua_type(state, 2) == LUA_TNUMBER ? static_cast<uint16>(lua_tointeger(state, 2)) : skill_name2id(luaL_checkstring(state, 2));

	if (skill_id == 0)
		return luaL_error(state, "there is no skill %s (use the AegisName, like AS_KATAR)", luaL_tolstring(state, 2, nullptr));

	const map_session_data* sd = map_id2sd(unit_id_at(state, 1));

	lua_pushinteger(state, sd != nullptr ? pc_checkskill(sd, skill_id) : 0);
	return 1;
}

void push_unit(const block_list* bl);
int32 name_index(const char* key, const char* const* names, int32 count);
std::string name_list(const char* const* names, int32 count);

// unit:party(range) -- the unit's party members on its map within `range`
// cells, alive, without the unit itself. Empty for anything not in a party.
int32 lua_unit_party(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	int32 range = static_cast<int32>(luaL_optinteger(state, 2, AREA_SIZE));
	const map_session_data* sd = map_id2sd(unit_id_at(state, 1));
	const party_data* p = sd != nullptr && sd->status.party_id != 0 ? party_search(sd->status.party_id) : nullptr;
	int32 n = 0;

	lua_newtable(state);
	if (p == nullptr)
		return 1;
	for (const party_member_data& member : p->data) {
		const map_session_data* other = member.sd;

		if (other == nullptr || other == sd || other->m != sd->m || pc_isdead(other) || !check_distance_bl(sd, other, range))
			continue;
		push_unit(other);
		lua_rawseti(state, -2, ++n);
	}
	return 1;
}

int32 unit_in_area_sub(block_list* bl, va_list ap) {
	const block_list* unit_bl = va_arg(ap, const block_list*);
	uint16 skill_id = static_cast<uint16>(va_arg(ap, int32));
	int32 owner_id = va_arg(ap, int32);
	const skill_unit* su = reinterpret_cast<const skill_unit*>(bl);

	if (!su->alive || su->group == nullptr || su->group->skill_id != skill_id)
		return 0;
	if (owner_id != 0 && su->group->src_id != owner_id)
		return 0;
	return check_distance_bl(su, unit_bl, su->range) ? 1 : 0;
}

// unit:in_skill_area("GC_POISONSMOKE", {owner}) -- whether the unit stands in
// that ground skill's area, anyone's or only `owner`'s.
int32 lua_unit_in_skill_area(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	uint16 skill_id = skill_name2id(luaL_checkstring(state, 2));
	const block_list* bl = map_id2bl(unit_id_at(state, 1));

	if (skill_id == 0)
		return luaL_error(state, "there is no skill %s", lua_tostring(state, 2));
	lua_pushboolean(state, bl != nullptr && map_foreachinallrange(unit_in_area_sub, bl, 3, BL_SKILL, bl, static_cast<int32>(skill_id), unit_id_at(state, 3)) > 0);
	return 1;
}

/// A skill by AegisName or id at `arg`; 0 for neither.
uint16 skill_at(lua_State* state, int32 arg) {
	if (lua_type(state, arg) == LUA_TNUMBER) {
		uint16 skill_id = static_cast<uint16>(lua_tointeger(state, arg));
		return skill_get_index(skill_id) != 0 ? skill_id : 0;
	}
	return lua_type(state, arg) == LUA_TSTRING ? skill_name2id(lua_tostring(state, arg)) : 0;
}

/// A skill at `arg`, or an error naming `what`.
uint16 check_skill(lua_State* state, int32 arg, const char* what) {
	uint16 skill_id = skill_at(state, arg);

	if (skill_id == 0)
		luaL_error(state, "%s: there is no skill %s (use the AegisName, like MG_FIREBOLT)", what, luaL_tolstring(state, arg, nullptr));
	return skill_id;
}

/// An item by AegisName or id at `arg`, or an error naming `what`.
std::shared_ptr<item_data> check_item(lua_State* state, int32 arg, const char* what) {
	std::shared_ptr<item_data> id = lua_type(state, arg) == LUA_TNUMBER ? item_db.find(static_cast<t_itemid>(lua_tointeger(state, arg))) : item_db.search_aegisname(luaL_checkstring(state, arg));

	if (id == nullptr)
		luaL_error(state, "%s: there is no item %s (use the AegisName, like POISON_BOTTLE)", what, luaL_tolstring(state, arg, nullptr));
	return id;
}

/// The player whose unit table is at `arg`, or nullptr.
map_session_data* player_at(lua_State* state, int32 arg) {
	return map_id2sd(unit_id_at(state, arg));
}

/// Who forged or brewed an item: a character id, or 0.
uint32 item_creator(const item& it) {
	if (it.card[0] == CARD0_FORGE || it.card[0] == CARD0_CREATE)
		return MakeDWord(it.card[2], it.card[3]);
	return 0;
}

/// An item as a table: id, name, amount, refine, creator, broken, cards,
/// weight, weapon_level, and its random options.
void push_item(const item& it) {
	std::shared_ptr<item_data> data = item_db.find(it.nameid);

	lua_createtable(L, 0, 12);
	set_int("id", it.nameid);
	set_string("name", data != nullptr ? data->name.c_str() : "");
	set_int("amount", it.amount);
	set_int("refine", it.refine);
	set_int("creator", item_creator(it));
	set_bool("broken", it.attribute != 0);
	set_int("weight", data != nullptr ? data->weight : 0);
	set_int("weapon_level", data != nullptr && data->type == IT_WEAPON ? data->weapon_level : 0);
	lua_createtable(L, MAX_SLOTS, 0);
	if (!itemdb_isspecial(it.card[0]))
		for (int32 c = 0; c < MAX_SLOTS; c++) {
			lua_pushinteger(L, it.card[c]);
			lua_rawseti(L, -2, c + 1);
		}
	lua_setfield(L, -2, "cards");
	lua_createtable(L, MAX_ITEM_RDM_OPT, 0);
	for (int32 o = 0, n = 0; o < MAX_ITEM_RDM_OPT; o++) {
		if (it.option[o].id == 0)
			continue;
		lua_createtable(L, 0, 3);
		set_int("id", it.option[o].id);
		set_int("value", it.option[o].value);
		set_int("param", it.option[o].param);
		lua_rawseti(L, -2, ++n);
	}
	lua_setfield(L, -2, "options");
}

// unit:status_info("SC_POISONREACT") -- nil, or what a running status holds:
// val1..val4 and remaining (ms; -1 for no end). For a mod's status: stacks
// and remaining.
int32 lua_unit_status_info(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	int32 id = unit_id_at(state, 1);

	if (const s_lua_status* def = custom_status(luaL_checkstring(state, 2)); def != nullptr) {
		auto it = statuses_on.find(id);
		int32 stacks = 0;
		t_tick left = 0;
		bool forever = false;

		if (it != statuses_on.end())
			for (const s_lua_status_on& on : it->second) {
				if (on.index != def->index)
					continue;
				stacks += on.stacks;
				if (on.expires == 0)
					forever = true;
				else
					left = std::max<t_tick>(left, DIFF_TICK(on.expires, gettick()));
			}
		if (stacks == 0) {
			lua_pushnil(state);
			return 1;
		}
		lua_createtable(state, 0, 2);
		lua_pushinteger(state, stacks);
		lua_setfield(state, -2, "stacks");
		lua_pushinteger(state, forever ? -1 : left);
		lua_setfield(state, -2, "remaining");
		return 1;
	}

	sc_type type = status_named(state, 2);
	block_list* bl = map_id2bl(id);
	status_change* sc = bl != nullptr ? status_get_sc(bl) : nullptr;
	status_change_entry* sce = sc != nullptr ? sc->getSCE(type) : nullptr;

	if (sce == nullptr) {
		lua_pushnil(state);
		return 1;
	}

	const TimerData* timer = sce->timer != INVALID_TIMER ? get_timer(sce->timer) : nullptr;

	lua_createtable(state, 0, 5);
	lua_pushinteger(state, sce->val1);
	lua_setfield(state, -2, "val1");
	lua_pushinteger(state, sce->val2);
	lua_setfield(state, -2, "val2");
	lua_pushinteger(state, sce->val3);
	lua_setfield(state, -2, "val3");
	lua_pushinteger(state, sce->val4);
	lua_setfield(state, -2, "val4");
	lua_pushinteger(state, timer != nullptr ? std::max<t_tick>(0, DIFF_TICK(timer->tick, gettick())) : -1);
	lua_setfield(state, -2, "remaining");
	return 1;
}

/// The script constant names of the server's statuses, by sc_type, looked
/// up the first time each is asked for.
std::unordered_map<int32, std::string> status_names;

const char* status_name(int32 type) {
	if (type <= SC_NONE || type >= SC_MAX)
		return "";

	auto it = status_names.find(type);

	if (it == status_names.end()) {
		const char* name = script_get_constant_str("SC_", type);

		it = status_names.emplace(type, name != nullptr ? name : "").first;
	}
	return it->second.c_str();
}

/// What a hook calls a debuff: one of the common ailments (stone, freeze,
/// stun, sleep, poison, curse, silence, confusion, blind, bleeding), which
/// status.yml leaves unflagged, or a status it flags Debuff.
bool is_debuff(int32 type, const s_status_change_db* scdb) {
	return (type >= SC_COMMON_MIN && type <= SC_COMMON_MAX) || (scdb != nullptr && scdb->flag[SCF_DEBUFF]);
}

// unit:statuses({ debuff = true }) -- what the unit carries: an array of
// { name, remaining, debuff, mod }; the server's statuses, then the mods'.
int32 lua_unit_statuses(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	int32 id = unit_id_at(state, 1);
	bool only_debuffs = false;

	if (lua_istable(state, 2)) {
		lua_getfield(state, 2, "debuff");
		only_debuffs = lua_toboolean(state, -1) != 0;
		lua_pop(state, 1);
	}

	block_list* bl = map_id2bl(id);
	status_change* sc = bl != nullptr ? status_get_sc(bl) : nullptr;
	int32 n = 0;

	lua_newtable(state);
	if (sc != nullptr && !sc->empty()) {
		for (int32 type = SC_NONE + 1; type < SC_MAX; type++) {
			status_change_entry* sce = sc->getSCE(static_cast<sc_type>(type));
			std::shared_ptr<s_status_change_db> scdb = sce != nullptr ? status_db.find(static_cast<sc_type>(type)) : nullptr;

			if (scdb == nullptr)
				continue;

			bool debuff = is_debuff(type, scdb.get());

			if (only_debuffs && !debuff)
				continue;

			const TimerData* timer = sce->timer != INVALID_TIMER ? get_timer(sce->timer) : nullptr;

			lua_createtable(state, 0, 4);
			lua_pushstring(state, status_name(type));
			lua_setfield(state, -2, "name");
			lua_pushinteger(state, timer != nullptr ? std::max<t_tick>(0, DIFF_TICK(timer->tick, gettick())) : -1);
			lua_setfield(state, -2, "remaining");
			lua_pushboolean(state, debuff);
			lua_setfield(state, -2, "debuff");
			lua_pushboolean(state, false);
			lua_setfield(state, -2, "mod");
			lua_rawseti(state, -2, ++n);
		}
	}
	if (auto it = statuses_on.find(id); it != statuses_on.end() && !only_debuffs) {
		for (const s_lua_status_on& on : it->second) {
			lua_createtable(state, 0, 5);
			lua_pushstring(state, status_list[on.index]->name.c_str());
			lua_setfield(state, -2, "name");
			lua_pushinteger(state, on.expires == 0 ? -1 : std::max<t_tick>(0, DIFF_TICK(on.expires, gettick())));
			lua_setfield(state, -2, "remaining");
			lua_pushinteger(state, on.stacks);
			lua_setfield(state, -2, "stacks");
			lua_pushboolean(state, false);
			lua_setfield(state, -2, "debuff");
			lua_pushboolean(state, true);
			lua_setfield(state, -2, "mod");
			lua_rawseti(state, -2, ++n);
		}
	}
	return 1;
}

/// Sets a ground skill's x and y on the table at the top: the middle of the
/// cells it was placed on, which is where it was cast for a square or a cross.
void set_group_middle(lua_State* state, const s_skill_unit_group& group) {
	if (group.unit_count <= 0)
		return;

	int16 x0 = group.unit[0].x, x1 = x0, y0 = group.unit[0].y, y1 = y0;

	for (int32 i = 1; i < group.unit_count; i++) {
		x0 = std::min(x0, group.unit[i].x);
		x1 = std::max(x1, group.unit[i].x);
		y0 = std::min(y0, group.unit[i].y);
		y1 = std::max(y1, group.unit[i].y);
	}
	lua_pushinteger(state, (x0 + x1) / 2);
	lua_setfield(state, -2, "x");
	lua_pushinteger(state, (y0 + y1) / 2);
	lua_setfield(state, -2, "y");
}

// unit:ground_units({skill}) -- the ground skills the unit has placed: an
// array of { group, skill, level, map, x, y, remaining, cells = { {x, y}, ... } },
// x and y the middle of its cells.
int32 lua_unit_ground_units(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	uint16 only = lua_isnoneornil(state, 2) ? 0 : check_skill(state, 2, "unit:ground_units");
	block_list* bl = map_id2bl(unit_id_at(state, 1));
	unit_data* ud = bl != nullptr ? unit_bl2ud(bl) : nullptr;
	int32 n = 0;

	lua_newtable(state);
	if (ud == nullptr)
		return 1;
	for (const std::shared_ptr<s_skill_unit_group>& group : ud->skillunits) {
		if (group == nullptr || group->unit == nullptr || (only != 0 && group->skill_id != only))
			continue;

		int32 cells = 0;

		lua_createtable(state, 0, 9);
		lua_pushinteger(state, group->group_id);
		lua_setfield(state, -2, "group");
		lua_pushstring(state, skill_get_name(group->skill_id));
		lua_setfield(state, -2, "skill");
		lua_pushinteger(state, group->skill_lv);
		lua_setfield(state, -2, "level");
		lua_pushstring(state, map_getmapdata(group->map)->name);
		lua_setfield(state, -2, "map");
		lua_pushinteger(state, std::max<t_tick>(0, group->limit - DIFF_TICK(gettick(), group->tick)));
		lua_setfield(state, -2, "remaining");
		set_group_middle(state, *group);
		lua_newtable(state);
		for (int32 i = 0; i < group->unit_count; i++) {
			const skill_unit& su = group->unit[i];

			if (!su.alive)
				continue;
			lua_createtable(state, 0, 2);
			lua_pushinteger(state, su.x);
			lua_setfield(state, -2, "x");
			lua_pushinteger(state, su.y);
			lua_setfield(state, -2, "y");
			lua_rawseti(state, -2, ++cells);
		}
		lua_setfield(state, -2, "cells");
		lua_rawseti(state, -2, ++n);
	}
	return 1;
}

const char* const equip_slot_names[] = { "weapon", "shield", "armor", "shoes", "garment", "head_top", "head_mid", "head_low", "accessory_1", "accessory_2", "ammo" };
const equip_index equip_slots[] = { EQI_HAND_R, EQI_HAND_L, EQI_ARMOR, EQI_SHOES, EQI_GARMENT, EQI_HEAD_TOP, EQI_HEAD_MID, EQI_HEAD_LOW, EQI_ACC_L, EQI_ACC_R, EQI_AMMO };

// unit:equipment("weapon") -- the item in that slot (see push_item), or nil.
int32 lua_unit_equipment(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	int32 slot = name_index(luaL_checkstring(state, 2), equip_slot_names, static_cast<int32>(sizeof(equip_slot_names) / sizeof(equip_slot_names[0])));

	if (slot < 0)
		return luaL_error(state, "unit:equipment: the slot is one of weapon, shield, armor, shoes, garment, head_top, head_mid, head_low, accessory_1, accessory_2, ammo");

	map_session_data* sd = player_at(state, 1);
	int16 idx = sd != nullptr ? sd->equip_index[equip_slots[slot]] : -1;

	if (idx < 0 || sd->inventory.u.items_inventory[idx].nameid == 0) {
		lua_pushnil(state);
		return 1;
	}
	push_item(sd->inventory.u.items_inventory[idx]);
	return 1;
}

// unit:count_item("RED_POTION") -- how many the player carries.
int32 lua_unit_count_item(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	std::shared_ptr<item_data> id = check_item(state, 2, "unit:count_item");
	map_session_data* sd = player_at(state, 1);
	int32 total = 0;

	if (sd != nullptr)
		for (int32 i = 0; i < MAX_INVENTORY; i++)
			if (sd->inventory.u.items_inventory[i].nameid == id->nameid)
				total += sd->inventory.u.items_inventory[i].amount;
	lua_pushinteger(state, total);
	return 1;
}

// unit:find_item("ACID_BOTTLE") -- the first stack of it in the bag (the one
// a skill would use up), or nil.
int32 lua_unit_find_item(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	std::shared_ptr<item_data> id = check_item(state, 2, "unit:find_item");
	map_session_data* sd = player_at(state, 1);
	int16 idx = sd != nullptr ? pc_search_inventory(sd, id->nameid) : -1;

	if (idx < 0) {
		lua_pushnil(state);
		return 1;
	}
	push_item(sd->inventory.u.items_inventory[idx]);
	return 1;
}

// unit:mob_skills() -- a monster's skills: an array of { skill, level }.
int32 lua_unit_mob_skills(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	const mob_data* md = map_id2md(unit_id_at(state, 1));
	int32 n = 0;
	std::vector<std::pair<uint16, uint16>> seen;

	lua_newtable(state);
	if (md == nullptr)
		return 1;
	for (const std::shared_ptr<s_mob_skill>& ms : md->db->skill) {
		if (ms == nullptr || skill_get_index(ms->skill_id) == 0)
			continue;
		if (std::find(seen.begin(), seen.end(), std::make_pair(ms->skill_id, ms->skill_lv)) != seen.end())
			continue;
		seen.emplace_back(ms->skill_id, ms->skill_lv);
		lua_createtable(state, 0, 2);
		lua_pushstring(state, skill_get_name(ms->skill_id));
		lua_setfield(state, -2, "skill");
		lua_pushinteger(state, ms->skill_lv);
		lua_setfield(state, -2, "level");
		lua_rawseti(state, -2, ++n);
	}
	return 1;
}

// unit:devotees() -- the units a Crusader protects with Devotion.
int32 lua_unit_devotees(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	map_session_data* sd = player_at(state, 1);
	int32 n = 0;

	lua_newtable(state);
	if (sd == nullptr)
		return 1;
	for (int32 i = 0; i < MAX_DEVOTION; i++) {
		if (block_list* d = sd->devotion[i] != 0 ? map_id2bl(sd->devotion[i]) : nullptr; d != nullptr) {
			push_unit(d);
			lua_rawseti(state, -2, ++n);
		}
	}
	return 1;
}

// unit:homunculus() -- a player's homunculus while it is out, or nil.
int32 lua_unit_homunculus(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	map_session_data* sd = player_at(state, 1);

	push_unit(sd != nullptr && hom_is_active(sd->hd) ? sd->hd : nullptr);
	return 1;
}

// unit:master() -- who a summon, homunculus, mercenary, elemental or pet
// belongs to, or nil.
int32 lua_unit_master(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	block_list* bl = map_id2bl(unit_id_at(state, 1));
	block_list* master = bl != nullptr ? battle_get_master(bl) : nullptr;

	push_unit(master != nullptr && master != bl ? master : nullptr);
	return 1;
}

// unit:cooldown_left("GC_CROSSIMPACT") -- the milliseconds left of the unit's
// cooldown of that skill: a player's, a homunculus's or a mercenary's; 0 when
// it has none.
int32 lua_unit_cooldown_left(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	uint16 skill_id = check_skill(state, 2, "unit:cooldown_left");
	block_list* bl = map_id2bl(unit_id_at(state, 1));
	int32 tid = INVALID_TIMER;

	auto find = [&](const auto& scd) {
		if (auto it = scd.find(skill_id); it != scd.end())
			tid = it->second;
	};

	if (map_session_data* sd = bl != nullptr ? BL_CAST(BL_PC, bl) : nullptr; sd != nullptr)
		find(sd->scd);
	else if (homun_data* hd = bl != nullptr ? BL_CAST(BL_HOM, bl) : nullptr; hd != nullptr)
		find(hd->scd);
	else if (s_mercenary_data* mc = bl != nullptr ? BL_CAST(BL_MER, bl) : nullptr; mc != nullptr)
		find(mc->scd);

	const TimerData* timer = tid != INVALID_TIMER ? get_timer(tid) : nullptr;

	lua_pushinteger(state, timer != nullptr ? std::max<t_tick>(0, DIFF_TICK(timer->tick, gettick())) : 0);
	return 1;
}

// unit:skill_range("SN_SHARPSHOOTING", {lv}) -- how far the unit reaches with
// that skill at that level (1 by default), as the server checks a use: the
// skill's Range, what the unit adds to it (Vulture's Eye) and mods' range
// hooks. nil once the unit is gone.
int32 lua_unit_skill_range(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	uint16 skill_id = check_skill(state, 2, "unit:skill_range");
	uint16 lv = static_cast<uint16>(std::clamp<lua_Integer>(luaL_optinteger(state, 3, 1), 1, MAX_SKILL_LEVEL));
	block_list* bl = map_id2bl(unit_id_at(state, 1));

	if (bl == nullptr)
		lua_pushnil(state);
	else
		lua_pushinteger(state, skill_get_range2(bl, skill_id, lv, true));
	return 1;
}

/// The metatable every unit table shares: its methods.
int32 unit_meta_ref = LUA_NOREF;

void register_unit_methods() {
	static const luaL_Reg methods[] = {
		{ "has_status", lua_unit_has_status },       { "status_stacks", lua_unit_status_stacks },
		{ "skill_lv", lua_unit_skill_lv },           { "party", lua_unit_party },
		{ "in_skill_area", lua_unit_in_skill_area }, { "status_info", lua_unit_status_info },
		{ "statuses", lua_unit_statuses },           { "ground_units", lua_unit_ground_units },
		{ "equipment", lua_unit_equipment },         { "count_item", lua_unit_count_item },
		{ "find_item", lua_unit_find_item },         { "mob_skills", lua_unit_mob_skills },
		{ "devotees", lua_unit_devotees },           { "homunculus", lua_unit_homunculus },
		{ "master", lua_unit_master },               { "cooldown_left", lua_unit_cooldown_left },
		{ "skill_range", lua_unit_skill_range },     { nullptr, nullptr },
	};

	lua_createtable(L, 0, 1);
	lua_createtable(L, 0, static_cast<int32>(sizeof(methods) / sizeof(methods[0])));
	luaL_setfuncs(L, methods, 0);
	lua_setfield(L, -2, "__index");
	unit_meta_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

const char* riding_name(const status_change* sc) {
	if (sc == nullptr)
		return "";
	if (sc->option & OPTION_MADOGEAR)
		return "mado";
	if (sc->option & OPTION_DRAGON)
		return "dragon";
	if (sc->option & OPTION_WUGRIDER)
		return "warg";
	if (sc->option & OPTION_RIDING)
		return "peco";
	return "";
}

void push_unit(const block_list* bl) {
	if (bl == nullptr) {
		lua_pushnil(L);
		return;
	}

	const status_data* st = status_get_status_data(*bl);
	const status_change* sc = status_get_sc(bl);
	const unit_data* ud = unit_bl2ud(const_cast<block_list*>(bl));
	const block_list* master = battle_get_master(bl);

	lua_createtable(L, 0, 64);
	set_int("id", bl->id);
	lua_pushstring(L, unit_kind(*bl));
	lua_setfield(L, -2, "kind");
	lua_pushstring(L, status_get_name(*bl));
	lua_setfield(L, -2, "name");
	set_int("level", status_get_lv(bl));
	set_int("x", bl->x);
	set_int("y", bl->y);
	set_int("dir", unit_getdir(bl));
	if (bl->m >= 0) {
		lua_pushstring(L, map_getmapdata(bl->m)->name);
		lua_setfield(L, -2, "map");
	}
	set_int("def", st->def);
	set_int("mdef", st->mdef);
	set_int("hit", st->hit);
	set_int("flee", st->flee);
	set_int("flee2", st->flee2);
	set_int("cri", st->cri);
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
	set_int("element_level", st->ele_lv);
	set_int("size", st->size);
	set_int("amotion", st->amotion);
	set_int("aspd", (2000 - st->amotion) / 10);
	set_int("speed", st->speed);
	lua_pushboolean(L, st->class_ == CLASS_BOSS);
	lua_setfield(L, -2, "boss");
	lua_pushboolean(L, status_isdead(*bl));
	lua_setfield(L, -2, "dead");
	set_bool("hidden", sc != nullptr && (sc->option & (OPTION_HIDE | OPTION_CLOAK | OPTION_CHASEWALK)) != 0);
	set_string("casting", ud != nullptr && ud->skilltimer != INVALID_TIMER && ud->skill_id != 0 ? skill_get_name(ud->skill_id) : "");
	set_int("master_id", master != nullptr && master != bl ? master->id : 0);

	if (const map_session_data* sd = BL_CAST(BL_PC, bl); sd != nullptr) {
		set_int("job", sd->status.class_);
		set_int("job_level", sd->status.job_level);
		set_int("party_id", sd->status.party_id);
		set_int("guild_id", sd->status.guild_id);
		set_int("char_id", sd->status.char_id);
		set_int("account_id", sd->status.account_id);
		// Item bonuses a hook might want to honour on a skill that stock
		// rAthena does not apply them to.
		set_int("classchange", sd->bonus.classchange);
		// Equipped items by slot, 0 when nothing is in that slot. Scripts
		// can gate on gear ("c.caster.weapon_id == const('VORPAL_BLADE')")
		// without a separate item() registration.
		auto slot_id = [&](equip_index e) -> t_itemid {
			int16 idx = sd->equip_index[e];
			return idx >= 0 ? sd->inventory.u.items_inventory[idx].nameid : 0;
		};
		set_int("weapon_id", slot_id(EQI_HAND_R));
		set_int("shield_id", slot_id(EQI_HAND_L));
		set_int("armor_id", slot_id(EQI_ARMOR));
		set_int("shoes_id", slot_id(EQI_SHOES));
		set_int("robe_id", slot_id(EQI_GARMENT));
		set_int("helm_top_id", slot_id(EQI_HEAD_TOP));
		set_int("helm_mid_id", slot_id(EQI_HEAD_MID));
		set_int("helm_bottom_id", slot_id(EQI_HEAD_LOW));
		set_int("accessory_1_id", slot_id(EQI_ACC_L));
		set_int("accessory_2_id", slot_id(EQI_ACC_R));
		set_int("ammo_id", slot_id(EQI_AMMO));
		set_int("weapon_type", sd->weapontype1);
		set_int("left_weapon_type", sd->weapontype2);
		set_int("spheres", sd->spiritball);
		set_int("spheres_spent", sd->spiritball_old);
		set_int("weight", sd->weight);
		set_int("max_weight", sd->max_weight);
		set_int("cart_weight", sd->cart_weight);
		set_int("cart_max_weight", sd->cart_weight_max);
		set_int("zeny", sd->status.zeny);
		set_string("riding", riding_name(sc));
		set_bool("falcon", pc_isfalcon(sd) != 0);
		set_bool("warg", pc_iswug(sd) != 0);
		set_bool("cart", !!pc_iscarton(sd));
		set_int("homunculus_id", hom_is_active(sd->hd) ? sd->hd->id : 0);

		// What Plagiarism and Reproduce hold.
		auto copied = [&](const char* skill_var, const char* lv_var, const char* name, const char* lv_name) {
			int64 skill_id = pc_readglobalreg(sd, add_str(skill_var));

			set_string(name, skill_id > 0 && skill_get_index(static_cast<uint16>(skill_id)) != 0 ? skill_get_name(static_cast<uint16>(skill_id)) : "");
			set_int(lv_name, skill_id > 0 ? pc_readglobalreg(sd, add_str(lv_var)) : 0);
		};

		copied(SKILL_VAR_PLAGIARISM, SKILL_VAR_PLAGIARISM_LV, "copied_skill", "copied_skill_lv");
		copied(SKILL_VAR_REPRODUCE, SKILL_VAR_REPRODUCE_LV, "reproduced_skill", "reproduced_skill_lv");
	} else if (const mob_data* md = BL_CAST(BL_MOB, bl); md != nullptr) {
		set_int("mob_id", md->mob_id);
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, unit_meta_ref);
	lua_setmetatable(L, -2);
}

// ---------------------------------------------------------------------------
// Queries any hook may make: now(), skill_info(), units_around(), units_in_line()
// ---------------------------------------------------------------------------

// now() -- the server's clock in milliseconds; only differences mean anything.
int32 lua_now(lua_State* state) {
	lua_pushinteger(state, gettick());
	return 1;
}

const char* skill_type_name(uint16 skill_id) {
	switch (skill_get_type(skill_id)) {
		case BF_WEAPON: return "weapon";
		case BF_MAGIC: return "magic";
		case BF_MISC: return "misc";
		default: return "none";
	}
}

const char* skill_target_name(uint16 skill_id) {
	int32 inf = skill_get_inf(skill_id);

	if (inf & INF_ATTACK_SKILL) return "attack";
	if (inf & INF_GROUND_SKILL) return "ground";
	if (inf & INF_SELF_SKILL) return "self";
	if (inf & INF_SUPPORT_SKILL) return "support";
	return "passive";
}

// skill_info("MG_FIREBOLT", 5) -- what the skill database says about a skill
// at a level (1 by default).
int32 lua_skill_info(lua_State* state) {
	uint16 skill_id = check_skill(state, 1, "skill_info");
	uint16 lv = static_cast<uint16>(std::clamp<lua_Integer>(luaL_optinteger(state, 2, 1), 1, MAX_SKILL_LEVEL));
	std::shared_ptr<s_skill_db> skill = skill_db.find(skill_id);

	lua_createtable(state, 0, 21);
	lua_pushinteger(state, skill_id);
	lua_setfield(state, -2, "id");
	lua_pushstring(state, skill_get_name(skill_id));
	lua_setfield(state, -2, "name");
	lua_pushstring(state, skill_get_desc(skill_id));
	lua_setfield(state, -2, "description");
	lua_pushinteger(state, skill_get_max(skill_id));
	lua_setfield(state, -2, "max_level");
	lua_pushstring(state, skill_type_name(skill_id));
	lua_setfield(state, -2, "type");
	lua_pushstring(state, skill_target_name(skill_id));
	lua_setfield(state, -2, "target");
	lua_pushinteger(state, skill_get_ele(skill_id, lv));
	lua_setfield(state, -2, "element");
	lua_pushinteger(state, skill_get_range(skill_id, lv));
	lua_setfield(state, -2, "range");
	lua_pushinteger(state, skill_get_splash(skill_id, lv));
	lua_setfield(state, -2, "splash");
	lua_pushinteger(state, skill_get_num(skill_id, lv));
	lua_setfield(state, -2, "hits");
	lua_pushinteger(state, skill_get_sp(skill_id, lv));
	lua_setfield(state, -2, "sp");
	lua_pushinteger(state, skill_get_hp(skill_id, lv));
	lua_setfield(state, -2, "hp");
	lua_pushinteger(state, skill_get_zeny(skill_id, lv));
	lua_setfield(state, -2, "zeny");
	lua_pushinteger(state, skill_get_cast(skill_id, lv));
	lua_setfield(state, -2, "cast");
	lua_pushinteger(state, skill_get_delay(skill_id, lv));
	lua_setfield(state, -2, "delay");
	lua_pushinteger(state, skill_get_cooldown(skill_id, lv));
	lua_setfield(state, -2, "cooldown");
	lua_pushinteger(state, skill_get_time(skill_id, lv));
	lua_setfield(state, -2, "duration");
	lua_pushinteger(state, skill_get_time2(skill_id, lv));
	lua_setfield(state, -2, "duration2");
	lua_pushboolean(state, skill_get_unit_id(skill_id) != 0);
	lua_setfield(state, -2, "ground_unit");
	lua_pushboolean(state, skill != nullptr && skill->copyable.option != 0);
	lua_setfield(state, -2, "copyable");
	return 1;
}

/// Where a query looks from, at `arg`: a unit table (where it stands) or a
/// cell, { map = "prontera", x = 150, y = 150 }.
void read_where(lua_State* state, int32 arg, int16& m, int16& x, int16& y, const char* what) {
	luaL_checktype(state, arg, LUA_TTABLE);
	m = -1;
	if (int32 id = unit_id_at(state, arg); id != 0) {
		if (block_list* bl = map_id2bl(id); bl != nullptr) {
			m = bl->m;
			x = bl->x;
			y = bl->y;
		}
		return;
	}
	lua_getfield(state, arg, "map");
	lua_getfield(state, arg, "x");
	lua_getfield(state, arg, "y");
	if (lua_type(state, -3) != LUA_TSTRING || !lua_isinteger(state, -2) || !lua_isinteger(state, -1))
		luaL_error(state, "%s: a place is a unit, or a cell like { map = \"prontera\", x = 150, y = 150 }", what);
	m = map_mapname2mapid(lua_tostring(state, -3));
	x = static_cast<int16>(lua_tointeger(state, -2));
	y = static_cast<int16>(lua_tointeger(state, -1));
	lua_pop(state, 3);
}

/// Which units a query keeps (filter table at `arg`, optional):
/// enemy_of, party_of (units), kind ("pc", "mob", "homun", "merc",
/// "elemental", or a list of them), exclude (a unit or a list), limit,
/// dead and hidden (whether to keep those; false by default).
struct s_unit_filter {
	block_list* enemy_of = nullptr;
	int32 party_id = -1;      ///< -1: any; 0 matches nothing
	int32 types = BL_CHAR;
	std::vector<int32> exclude;
	size_t limit = 50;
	bool dead = false;
	bool hidden = false;

	bool keeps(block_list* bl) const {
		if (std::find(exclude.begin(), exclude.end(), bl->id) != exclude.end())
			return false;
		if (!dead && status_isdead(*bl))
			return false;

		const status_change* sc = status_get_sc(bl);

		if (sc != nullptr && (sc->option & OPTION_INVISIBLE))
			return false;  // a GM's @hide
		if (!hidden && sc != nullptr && (sc->option & (OPTION_HIDE | OPTION_CLOAK | OPTION_CHASEWALK)))
			return false;
		if (enemy_of != nullptr && (bl == enemy_of || battle_check_target(enemy_of, bl, BCT_ENEMY) <= 0))
			return false;
		if (party_id >= 0 && (party_id == 0 || status_get_party_id(bl) != party_id))
			return false;
		return true;
	}
};

int32 unit_type_named(lua_State* state, const char* kind) {
	static const char* const kinds[] = { "pc", "mob", "homun", "merc", "elemental" };
	static const int32 types[] = { BL_PC, BL_MOB, BL_HOM, BL_MER, BL_ELEM };
	int32 i = name_index(kind, kinds, 5);

	if (i < 0)
		luaL_error(state, "a unit kind is pc, mob, homun, merc or elemental, not %s", kind);
	return types[i];
}

s_unit_filter read_filter(lua_State* state, int32 arg) {
	s_unit_filter f;

	if (lua_isnoneornil(state, arg))
		return f;
	luaL_checktype(state, arg, LUA_TTABLE);

	lua_getfield(state, arg, "enemy_of");
	if (int32 id = unit_id_at(state, -1); id != 0)
		f.enemy_of = map_id2bl(id);
	lua_pop(state, 1);

	lua_getfield(state, arg, "party_of");
	if (int32 id = unit_id_at(state, -1); id != 0) {
		block_list* bl = map_id2bl(id);
		f.party_id = bl != nullptr ? status_get_party_id(bl) : 0;
	}
	lua_pop(state, 1);

	lua_getfield(state, arg, "kind");
	if (lua_type(state, -1) == LUA_TSTRING) {
		f.types = unit_type_named(state, lua_tostring(state, -1));
	} else if (lua_istable(state, -1)) {
		f.types = 0;
		for (int32 i = 1; lua_rawgeti(state, -1, i) == LUA_TSTRING; i++) {
			f.types |= unit_type_named(state, lua_tostring(state, -1));
			lua_pop(state, 1);
		}
		lua_pop(state, 1);
	}
	lua_pop(state, 1);

	lua_getfield(state, arg, "exclude");
	if (int32 id = unit_id_at(state, -1); id != 0) {
		f.exclude.push_back(id);
	} else if (lua_istable(state, -1)) {
		for (int32 i = 1; lua_rawgeti(state, -1, i) == LUA_TTABLE; i++) {
			f.exclude.push_back(unit_id_at(state, -1));
			lua_pop(state, 1);
		}
		lua_pop(state, 1);
	}
	lua_pop(state, 1);

	lua_getfield(state, arg, "limit");
	f.limit = static_cast<size_t>(std::clamp<lua_Integer>(luaL_optinteger(state, -1, 50), 0, 200));
	lua_pop(state, 1);
	lua_getfield(state, arg, "dead");
	f.dead = lua_toboolean(state, -1) != 0;
	lua_pop(state, 1);
	lua_getfield(state, arg, "hidden");
	f.hidden = lua_toboolean(state, -1) != 0;
	lua_pop(state, 1);
	return f;
}

int32 collect_unit(block_list* bl, va_list ap) {
	va_arg(ap, std::vector<block_list*>*)->push_back(bl);
	return 0;
}

/// The units `f` keeps among `found`, nearest to (x, y) first, as an array.
void push_units(std::vector<block_list*>& found, const s_unit_filter& f, int16 x, int16 y) {
	std::vector<block_list*> kept;

	for (block_list* bl : found)
		if (f.keeps(bl))
			kept.push_back(bl);
	std::stable_sort(kept.begin(), kept.end(), [&](const block_list* a, const block_list* b) {
		return distance_xy(x, y, a->x, a->y) < distance_xy(x, y, b->x, b->y);
	});
	if (kept.size() > f.limit)
		kept.resize(f.limit);
	lua_createtable(L, static_cast<int32>(kept.size()), 0);
	for (size_t i = 0; i < kept.size(); i++) {
		push_unit(kept[i]);
		lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
	}
}

// units_around(where, radius, filter) -- the units in the square of
// (2 * radius + 1) cells on a side around a unit or a cell, as skills'
// areas are; nearest first.
int32 lua_units_around(lua_State* state) {
	int16 m, x, y;

	read_where(state, 1, m, x, y, "units_around");

	int16 r = static_cast<int16>(std::clamp<lua_Integer>(luaL_checkinteger(state, 2), 0, 30));
	s_unit_filter f = read_filter(state, 3);
	std::vector<block_list*> found;

	if (m >= 0)
		map_foreachinallarea(collect_unit, m, x - r, y - r, x + r, y + r, f.types, &found);
	push_units(found, f, x, y);
	return 1;
}

// units_in_line(from, to, width, filter) -- the units within `width` cells
// of the line from one place to another (filter.length: cells to go on
// past `to`, as Sharp Shooting does); nearest to `from` first.
int32 lua_units_in_line(lua_State* state) {
	int16 m0, x0, y0, m1, x1, y1;

	read_where(state, 1, m0, x0, y0, "units_in_line");
	read_where(state, 2, m1, x1, y1, "units_in_line");

	int16 width = static_cast<int16>(std::clamp<lua_Integer>(luaL_optinteger(state, 3, 0), 0, 5));
	s_unit_filter f = read_filter(state, 4);
	int32 length = distance_xy(x0, y0, x1, y1);
	std::vector<block_list*> found;

	if (lua_istable(state, 4)) {
		lua_getfield(state, 4, "length");
		length = static_cast<int32>(std::clamp<lua_Integer>(luaL_optinteger(state, -1, length), 0, 30));
		lua_pop(state, 1);
	}
	if (m0 >= 0 && m0 == m1 && (x0 != x1 || y0 != y1))
		map_foreachinpath(collect_unit, m0, x0, y0, x1, y1, width, length, f.types, &found);
	push_units(found, f, x0, y0);
	return 1;
}

int32 collect_skill_unit(block_list* bl, va_list ap) {
	va_arg(ap, std::vector<skill_unit*>*)->push_back(reinterpret_cast<skill_unit*>(bl));
	return 0;
}

// ground_units_around(where, radius, {filter}) -- the ground skills anyone
// has placed with a cell in that square: an array of { group, skill, level,
// owner_id, x, y, cells, remaining }, x and y the middle of its cells and
// cells counting those in the square.
// filter.enemy_of: only those whose owner is that unit's enemy.
int32 lua_ground_units_around(lua_State* state) {
	int16 m, x, y;

	read_where(state, 1, m, x, y, "ground_units_around");

	int16 r = static_cast<int16>(std::clamp<lua_Integer>(luaL_checkinteger(state, 2), 0, 30));
	block_list* enemy_of = nullptr;

	if (lua_istable(state, 3)) {
		lua_getfield(state, 3, "enemy_of");
		if (int32 id = unit_id_at(state, -1); id != 0)
			enemy_of = map_id2bl(id);
		lua_pop(state, 1);
	}

	std::vector<skill_unit*> cells;

	if (m >= 0)
		map_foreachinallarea(collect_skill_unit, m, x - r, y - r, x + r, y + r, BL_SKILL, &cells);

	// One entry for each group, in the order its first cell was found.
	std::vector<std::pair<std::shared_ptr<s_skill_unit_group>, int32>> groups;

	for (skill_unit* su : cells) {
		if (!su->alive || su->group == nullptr)
			continue;

		auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == su->group; });

		if (it != groups.end()) {
			it->second++;
			continue;
		}
		if (enemy_of != nullptr) {
			block_list* owner = map_id2bl(su->group->src_id);

			if (owner == nullptr || owner == enemy_of || battle_check_target(enemy_of, owner, BCT_ENEMY) <= 0)
				continue;
		}
		groups.emplace_back(su->group, 1);
	}

	int32 n = 0;

	lua_createtable(state, static_cast<int32>(groups.size()), 0);
	for (const auto& [group, count] : groups) {
		lua_createtable(state, 0, 8);
		lua_pushinteger(state, group->group_id);
		lua_setfield(state, -2, "group");
		lua_pushstring(state, skill_get_name(group->skill_id));
		lua_setfield(state, -2, "skill");
		lua_pushinteger(state, group->skill_lv);
		lua_setfield(state, -2, "level");
		lua_pushinteger(state, group->src_id);
		lua_setfield(state, -2, "owner_id");
		set_group_middle(state, *group);
		lua_pushinteger(state, count);
		lua_setfield(state, -2, "cells");
		lua_pushinteger(state, std::max<t_tick>(0, group->limit - DIFF_TICK(gettick(), group->tick)));
		lua_setfield(state, -2, "remaining");
		lua_rawseti(state, -2, ++n);
	}
	return 1;
}

// battle_config("max_parameter") -- a value of the server's battle config
// (conf/battle/*.conf, with a mod's conf/ changes), read as the script
// command getbattleflag reads it: 0 for a name the server does not have.
int32 lua_battle_config(lua_State* state) {
	lua_pushinteger(state, battle_get_value(luaL_checkstring(state, 1)));
	return 1;
}

// element_rate(attack_element, unit) or element_rate(attack_element,
// defense_element, {defense_level}) -- the % of a blow of that element the
// server's element table (db/attr_fix.yml) lets through against that
// defender: 200 doubles it, 0 is immune, below 0 heals. The table alone:
// what statuses add in a fight (Volcano, an endow) is not in it.
int32 lua_element_rate(lua_State* state) {
	lua_Integer atk = luaL_checkinteger(state, 1), def_ele, def_lv;

	if (lua_istable(state, 2)) {
		block_list* bl = map_id2bl(unit_id_at(state, 2));
		status_data* st = bl != nullptr ? status_get_status_data(*bl) : nullptr;

		if (st == nullptr) {
			lua_pushnil(state);
			return 1;
		}
		def_ele = st->def_ele;
		def_lv = st->ele_lv;
	} else {
		def_ele = luaL_checkinteger(state, 2);
		def_lv = luaL_optinteger(state, 3, 1);
	}
	if (!CHK_ELEMENT(atk) || !CHK_ELEMENT(def_ele) || !CHK_ELEMENT_LEVEL(def_lv))
		return luaL_error(state, "element_rate: elements are const(\"ELE_FIRE\") and the like, defense levels 1 to %d", MAX_ELE_LEVEL);
	lua_pushinteger(state, elemental_attribute_db.getAttribute(static_cast<uint16>(def_lv), static_cast<uint16>(atk), static_cast<uint16>(def_ele)));
	return 1;
}

// size_rate(weapon_type, size) -- the % of a weapon's damage the server's size
// table (db/size_fix.yml) lets through against that size: a weapon type such
// as const("W_KNUCKLE") (as unit.weapon_type), and a size as unit.size (0
// small, 1 medium, 2 large). 100 for a weapon type the table leaves out.
int32 lua_size_rate(lua_State* state) {
	lua_Integer type = luaL_checkinteger(state, 1), size = luaL_checkinteger(state, 2);

	if (size < SZ_SMALL || size > SZ_BIG)
		return luaL_error(state, "size_rate: a size is 0 (small), 1 (medium) or 2 (large), as unit.size");

	std::shared_ptr<s_sizefix_db> fix = size_fix_db.find(static_cast<int32>(type));

	lua_pushinteger(state, fix == nullptr ? 100 : size == SZ_SMALL ? fix->small : size == SZ_MEDIUM ? fix->medium : fix->large);
	return 1;
}

// def_rate(def) -- the % of a weapon blow that this much hard DEF (unit.def)
// lets through, by the formula the server uses in this era: renewal's
// (4000 + DEF) / (4000 + 10 x DEF), a number with a fraction; pre-renewal's
// 100 - DEF, never below 0. Soft DEF is taken off after it, as a number.
int32 lua_def_rate(lua_State* state) {
	lua_Integer def = luaL_checkinteger(state, 1);

#ifdef RENEWAL
	if (def == -400)  // as battle.cpp: -400 would divide by 0
		def = -399;
	lua_pushnumber(state, 100.0 * static_cast<double>(4000 + def) / static_cast<double>(4000 + 10 * def));
#else
	lua_pushinteger(state, 100 - std::min<lua_Integer>(def, 100));
#endif
	return 1;
}

// The hit a hook that can act is running for. Actions record into it;
// outside such a hook (in ratio, cost, bonus, ...) it is null and they refuse.
s_skill_lua_hit* current_hit = nullptr;

/// The unit whose skill, item or status the running hook belongs to (the
/// learner, the wearer, the unit with the status): who applies the statuses
/// it puts on, unless it names someone else.
int32 current_owner = 0;

s_skill_lua_hit& hit_or_error(lua_State* state, const char* what) {
	if (current_hit == nullptr)
		luaL_error(state, "c:%s() only works in a hook that can act: on_hit, on_attack, on_hit_taken, before_cast, on_cast, on_skill, on_kill, on_death, heal, on_steal, on_craft, on_use, tick, and a status's on_start and on_end", what);
	return *current_hit;
}

/// Who an action happens to: "target" (the default `fallback`), "caster", or
/// a unit table such as a party member from unit:party().
int32 unit_arg(lua_State* state, int32 arg, const s_skill_lua_hit& hit, const char* fallback = "target") {
	if (lua_istable(state, arg)) {
		if (int32 id = unit_id_at(state, arg); id != 0)
			return id;
		return luaL_error(state, "that table is not a unit");
	}

	const char* who = luaL_optstring(state, arg, fallback);

	if (strcmp(who, "target") == 0)
		return hit.target_id;
	if (strcmp(who, "caster") == 0)
		return hit.src_id;
	return luaL_error(state, "who must be \"target\", \"caster\" or a unit, not \"%s\"", who);
}

// c:drain() -- the attacker's HP/SP drain item bonuses, on this hit's
// damage. The direction is fixed (attacker drains defender) because the
// underlying server call is: calling it in on_hit_taken is harmless but
// redundant, since the stock weapon-attack path already drains for every
// weapon hit that lands.
int32 lua_hit_drain(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "drain");
	hit.actions.push_back({ SKILL_LUA_DRAIN, hit.src_id });
	return 0;
}

/// A boolean field of the options table at `arg`, if there is one.
bool opt_bool(lua_State* state, int32 arg, const char* key) {
	if (!lua_istable(state, arg))
		return false;
	lua_getfield(state, arg, key);
	bool value = lua_toboolean(state, -1) != 0;
	lua_pop(state, 1);
	return value;
}

/// A whole number field of the options table at `arg`, or `fallback`.
lua_Integer opt_int(lua_State* state, int32 arg, const char* key, lua_Integer fallback) {
	if (!lua_istable(state, arg))
		return fallback;
	lua_getfield(state, arg, key);
	lua_Integer value = luaL_optinteger(state, -1, fallback);
	lua_pop(state, 1);
	return value;
}

/// The unit an options table at `arg` names under `key` ("caster", "target"
/// or a unit table), or 0.
int32 opt_unit(lua_State* state, int32 arg, const char* key, const s_skill_lua_hit& hit) {
	if (!lua_istable(state, arg))
		return 0;
	lua_getfield(state, arg, key);
	int32 id = lua_isnil(state, -1) ? 0 : unit_arg(state, lua_gettop(state), hit);
	lua_pop(state, 1);
	return id;
}

/// The cell an options table at `arg` names as x and y, if both are there.
void opt_cell(lua_State* state, int32 arg, s_skill_lua_action& action) {
	lua_Integer x = opt_int(state, arg, "x", -1);
	lua_Integer y = opt_int(state, arg, "y", -1);

	if (x >= 0 && y >= 0) {
		action.x = static_cast<int16>(std::min<lua_Integer>(x, INT16_MAX));
		action.y = static_cast<int16>(std::min<lua_Integer>(y, INT16_MAX));
	}
}

// c:heal(hp, sp, {who}, {opts}) -- restore a unit. `who` is "caster" (default,
// the attacker), "target" (the defender) or a unit. An on_hit_taken hook that
// wants to restore the wearer picks "target", since the wearer is the
// defender. opts.force heals through Berserk, which stops other healing.
int32 lua_hit_heal(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "heal");
	s_skill_lua_action action = { SKILL_LUA_HEAL };

	action.hp = std::max<lua_Integer>(0, luaL_checkinteger(state, 2));
	action.sp = std::max<lua_Integer>(0, luaL_optinteger(state, 3, 0));
	action.unit_id = unit_arg(state, 4, hit, "caster");
	action.flag = opt_bool(state, 5, "force") ? 1 : 0;
	hit.actions.push_back(action);
	return 0;
}

/// A duration in ms at `arg`: -1 for "until it is ended", else 0..max.
int64 read_duration(lua_State* state, int32 arg, int64 max) {
	lua_Integer ms = luaL_checkinteger(state, arg);

	return ms < 0 ? INFINITE_TICK : std::min<lua_Integer>(ms, max);
}

// c:status("SC_STUN", rate, duration_ms, {vals}, {who}) -- start one of the
// server's statuses on "target" (default), "caster" or a unit. rate is out of
// 10000, before the target's resistance; duration -1 lasts until it is ended.
// vals is val1, or a table { val1 = ..., val2 = ..., val3 = ..., val4 = ... }
// for the statuses that read more than one (SC_COMBO, SC_SKILLRATE_UP, ...).
int32 lua_hit_status(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "status");
	s_skill_lua_action action = { SKILL_LUA_STATUS };

	action.type = status_named(state, 2);
	action.rate = static_cast<int32>(std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 0, 10000));
	action.duration = read_duration(state, 4, 86400000);
	if (lua_istable(state, 5)) {
		action.val1 = static_cast<int32>(opt_int(state, 5, "val1", 0));
		action.val2 = static_cast<int32>(opt_int(state, 5, "val2", 0));
		action.val3 = static_cast<int32>(opt_int(state, 5, "val3", 0));
		action.val4 = static_cast<int32>(opt_int(state, 5, "val4", 0));
	} else {
		action.val1 = static_cast<int32>(luaL_optinteger(state, 5, 1));
	}
	action.unit_id = unit_arg(state, 6, hit);
	hit.actions.push_back(action);
	return 0;
}

// c:end_status("SC_HIDING", {who}) -- end one of the server's statuses, or
// every stack of a mod's, on "target" (default), "caster" or a unit.
int32 lua_hit_end_status(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "end_status");

	if (const s_lua_status* def = custom_status(luaL_checkstring(state, 2)); def != nullptr) {
		s_skill_lua_action action = { SKILL_LUA_REMOVE_STATUS };

		action.type = def->index;
		action.val1 = -1;
		action.unit_id = unit_arg(state, 3, hit);
		hit.actions.push_back(action);
		return 0;
	}

	s_skill_lua_action action = { SKILL_LUA_END_STATUS };

	action.type = status_named(state, 2);
	action.unit_id = unit_arg(state, 3, hit);
	hit.actions.push_back(action);
	return 0;
}

// c:status_val("SC_POISONREACT", 2, 6, {who}) -- change val1..val4 of one of
// the server's statuses while it runs (what each holds is the server's own:
// see status_change_start in status.cpp). Its effect on stats is worked out
// again.
int32 lua_hit_status_val(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "status_val");
	s_skill_lua_action action = { SKILL_LUA_STATUS_VAL };

	action.type = status_named(state, 2);
	action.rate = static_cast<int32>(luaL_checkinteger(state, 3));
	if (action.rate < 1 || action.rate > 4)
		return luaL_error(state, "c:status_val: the value is 1, 2, 3 or 4 (val1..val4)");
	action.val1 = static_cast<int32>(luaL_checkinteger(state, 4));
	action.unit_id = unit_arg(state, 5, hit);
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

/// opts.power of a cast, an attack or a placed unit: % of the damage, 1..1000.
int32 opt_power(lua_State* state, int32 arg) {
	return static_cast<int32>(std::clamp<lua_Integer>(opt_int(state, arg, "power", 100), 1, 1000));
}

// c:chance(n) -- true n times in 10000, from the server's own random numbers.
// c:cast("MG_FIREBOLT", 3, {who}, {opts}) -- cast a skill the way bAutoSpell
// does: at "target" (default), "caster" or a unit, with the skill's own checks
// and after-cast delay, no cast time, and its HP, spheres and zeny paid (no SP,
// no items).
// opts.by: who casts it (default the hook's caster); opts.x, opts.y: the cell
// a ground skill goes to (default where `who` stands); opts.power: % of the
// damage it deals, also later from the ground units it places as it is cast
// (Storm Gust's ticks).
int32 lua_hit_cast(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "cast");
	uint16 skill_id = check_skill(state, 2, "c:cast");
	s_skill_lua_action action = { SKILL_LUA_CAST };

	action.type = skill_id;
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_optinteger(state, 3, 1), 1, MAX_SKILL_LEVEL));
	action.unit_id = unit_arg(state, 4, hit);
	action.by_id = opt_unit(state, 5, "by", hit);
	action.power = opt_power(state, 5);
	opt_cell(state, 5, action);
	hit.actions.push_back(action);
	return 0;
}

// c:attack("SN_SHARPSHOOTING", 5, {who}, {opts}) -- one hit of a skill on
// "target" (default) or a unit, nothing paid, no cast or delay: a splash, a
// bounce, an echo. Only an enemy of the attacker is hit. opts.by: who deals
// it (default the hook's caster); opts.power: % of the damage.
int32 lua_hit_attack(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "attack");
	uint16 skill_id = check_skill(state, 2, "c:attack");

	if (skill_get_type(skill_id) == BF_NONE)
		return luaL_error(state, "c:attack: %s deals no damage (its skill_db Type is None)", skill_get_name(skill_id));

	s_skill_lua_action action = { SKILL_LUA_ATTACK };

	action.type = skill_id;
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_optinteger(state, 3, 1), 1, MAX_SKILL_LEVEL));
	action.unit_id = unit_arg(state, 4, hit);
	action.by_id = opt_unit(state, 5, "by", hit);
	action.power = opt_power(state, 5);
	hit.actions.push_back(action);
	return 0;
}

// c:spheres(n, {who}, {opts}) -- a player gains n spirit spheres (n < 0: loses
// them). opts.max: the most to have (default what the server allows: 5, more
// under Raising Dragon); opts.duration: how long new ones last (ms).
int32 lua_hit_spheres(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "spheres");
	s_skill_lua_action action = { SKILL_LUA_SPHERES };

	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_checkinteger(state, 2), -MAX_SPIRITBALL, MAX_SPIRITBALL));
	action.unit_id = unit_arg(state, 3, hit, "caster");
	action.flag = static_cast<int32>(std::clamp<lua_Integer>(opt_int(state, 4, "max", 0), 0, MAX_SPIRITBALL));
	action.duration = std::clamp<lua_Integer>(opt_int(state, 4, "duration", 600000), 1000, 86400000);
	hit.actions.push_back(action);
	return 0;
}

// c:slide(who, x, y) -- move a unit to a cell at once, as Snap does. Nothing
// happens when the cell cannot be walked to.
int32 lua_hit_slide(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "slide");
	s_skill_lua_action action = { SKILL_LUA_SLIDE };

	action.unit_id = unit_arg(state, 2, hit);
	action.x = static_cast<int16>(luaL_checkinteger(state, 3));
	action.y = static_cast<int16>(luaL_checkinteger(state, 4));
	hit.actions.push_back(action);
	return 0;
}

// c:knockback(who, cells, {from}) -- push a unit away from `from` (a unit or a
// cell; the hook's caster by default), as a skill's knockback does.
int32 lua_hit_knockback(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "knockback");
	s_skill_lua_action action = { SKILL_LUA_KNOCKBACK };

	action.unit_id = unit_arg(state, 2, hit);
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 1, 20));
	if (lua_istable(state, 4)) {
		if (int32 id = unit_id_at(state, 4); id != 0) {
			action.by_id = id;
		} else {
			opt_cell(state, 4, action);
			if (action.x < 0)
				return luaL_error(state, "c:knockback: from is a unit or a cell like { x = 150, y = 150 }");
		}
	}
	hit.actions.push_back(action);
	return 0;
}

// c:interrupt(who) -- cancel the cast the unit is in the middle of.
int32 lua_hit_interrupt(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "interrupt");

	hit.actions.push_back({ SKILL_LUA_INTERRUPT, unit_arg(state, 2, hit) });
	return 0;
}

// c:summon("PORING", {opts}) -- monsters that fight for the hook's owner (or
// opts.by), as the summon script command makes them. opts: amount (1),
// duration (ms, 60000), x and y (where the owner stands), and neutral: on
// nobody's side, as a monster of the map's own is (a plant to harvest).
int32 lua_hit_summon(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "summon");
	std::shared_ptr<s_mob_db> mob = lua_type(state, 2) == LUA_TNUMBER ? mob_db.find(static_cast<uint32>(lua_tointeger(state, 2))) : mobdb_search_aegisname(luaL_checkstring(state, 2));

	if (mob == nullptr)
		return luaL_error(state, "c:summon: there is no monster %s (use the AegisName, like PORING)", luaL_tolstring(state, 2, nullptr));

	s_skill_lua_action action = { SKILL_LUA_SUMMON };

	action.type = static_cast<int32>(mob->id);
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(opt_int(state, 3, "amount", 1), 1, 20));
	action.duration = std::clamp<lua_Integer>(opt_int(state, 3, "duration", 60000), 1000, 3600000);
	action.by_id = opt_unit(state, 3, "by", hit);
	if (action.by_id == 0)
		action.by_id = current_owner != 0 ? current_owner : hit.src_id;
	action.unit_id = action.by_id;
	action.flag = opt_bool(state, 3, "neutral") ? 1 : 0;
	opt_cell(state, 3, action);
	hit.actions.push_back(action);
	return 0;
}

// c:set_target(who, target) -- a monster turns on `target`, as Provoke makes
// it.
int32 lua_hit_set_target(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "set_target");
	s_skill_lua_action action = { SKILL_LUA_SET_TARGET };

	action.unit_id = unit_arg(state, 2, hit);
	action.by_id = unit_arg(state, 3, hit, "caster");
	hit.actions.push_back(action);
	return 0;
}

// c:take_item("MAGIC_GEAR_FUEL", {amount}, {who}) -- take items from a player,
// the caster by default: all of them, or none when the player has fewer.
int32 lua_hit_take_item(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "take_item");
	s_skill_lua_action action = { SKILL_LUA_TAKE_ITEM };

	action.type = static_cast<int32>(check_item(state, 2, "c:take_item")->nameid);
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_optinteger(state, 3, 1), 1, MAX_AMOUNT));
	action.unit_id = unit_arg(state, 4, hit, "caster");
	hit.actions.push_back(action);
	return 0;
}

// c:copy_skill("WZ_STORMGUST", 5, {who}, {slot}) -- put a skill into a
// player's Plagiarism slot ("plagiarism", the default) or Reproduce slot
// ("reproduce"), replacing what was there, as the server's own copy does.
int32 lua_hit_copy_skill(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "copy_skill");
	s_skill_lua_action action = { SKILL_LUA_COPY_SKILL };
	const char* slot = luaL_optstring(state, 5, "plagiarism");

	action.type = check_skill(state, 2, "c:copy_skill");
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 1, MAX_SKILL_LEVEL));
	action.unit_id = unit_arg(state, 4, hit, "caster");
	if (strcmp(slot, "reproduce") == 0)
		action.flag = 2;
	else if (strcmp(slot, "plagiarism") == 0)
		action.flag = 1;
	else
		return luaL_error(state, "c:copy_skill: the slot is \"plagiarism\" or \"reproduce\", not \"%s\"", slot);
	hit.actions.push_back(action);
	return 0;
}

// c:itemskill("WZ_METEOR", 5, {who}) -- let a player use a skill once, as a
// scroll does: the client asks where. The caster by default.
int32 lua_hit_itemskill(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "itemskill");
	s_skill_lua_action action = { SKILL_LUA_ITEMSKILL };

	action.type = check_skill(state, 2, "c:itemskill");
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 1, MAX_SKILL_LEVEL));
	action.unit_id = unit_arg(state, 4, hit, "caster");
	hit.actions.push_back(action);
	return 0;
}

// c:spend(hp, sp, {who}) -- HP and SP paid without a hit, as a skill's cost
// is; never the last HP. The caster by default.
int32 lua_hit_spend(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "spend");
	s_skill_lua_action action = { SKILL_LUA_SPEND };

	action.hp = std::clamp<lua_Integer>(luaL_checkinteger(state, 2), 0, INT32_MAX);
	action.sp = std::clamp<lua_Integer>(luaL_optinteger(state, 3, 0), 0, INT32_MAX);
	action.unit_id = unit_arg(state, 4, hit, "caster");
	hit.actions.push_back(action);
	return 0;
}

// c:place_unit("MG_FIREWALL", 10, x, y, {opts}) -- place a ground skill at a
// cell, owned by the hook's owner (or opts.by), outside the skill's own limit
// on how many may stand. opts.duration: how long it stays (ms; default the
// skill's); opts.power: % of the damage its hits deal.
int32 lua_hit_place_unit(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "place_unit");
	uint16 skill_id = check_skill(state, 2, "c:place_unit");

	if (skill_get_unit_id(skill_id) == 0)
		return luaL_error(state, "c:place_unit: %s places nothing on the ground (it has no Unit in skill_db)", skill_get_name(skill_id));

	s_skill_lua_action action = { SKILL_LUA_PLACE_UNIT };

	action.type = skill_id;
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 1, MAX_SKILL_LEVEL));
	action.x = static_cast<int16>(luaL_checkinteger(state, 4));
	action.y = static_cast<int16>(luaL_checkinteger(state, 5));
	action.duration = std::clamp<lua_Integer>(opt_int(state, 6, "duration", 0), 0, 3600000);
	action.power = opt_power(state, 6);
	action.by_id = opt_unit(state, 6, "by", hit);
	if (action.by_id == 0)
		action.by_id = current_owner != 0 ? current_owner : hit.src_id;
	action.unit_id = action.by_id;
	hit.actions.push_back(action);
	return 0;
}

// c:end_unit(group) -- remove a ground skill: a group from unit:ground_units().
int32 lua_hit_end_unit(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "end_unit");
	s_skill_lua_action action = { SKILL_LUA_END_UNIT };

	action.type = static_cast<int32>(luaL_checkinteger(state, 2));
	hit.actions.push_back(action);
	return 0;
}

// c:expire(who, ms) -- a summoned monster (a plant, a sphere mine, a summon)
// leaves `ms` from now instead of when it would have.
int32 lua_hit_expire(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "expire");
	s_skill_lua_action action = { SKILL_LUA_EXPIRE };

	action.unit_id = unit_arg(state, 2, hit);
	action.duration = std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 0, 3600000);
	hit.actions.push_back(action);
	return 0;
}

// c:produce_menu(item_level, {who}) -- open the crafting window for the
// recipes of that ItemLevel in produce_db, as the produce script command
// does. The caster by default.
int32 lua_hit_produce_menu(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "produce_menu");
	s_skill_lua_action action = { SKILL_LUA_PRODUCE_MENU };

	action.val1 = static_cast<int32>(luaL_checkinteger(state, 2));
	action.unit_id = unit_arg(state, 3, hit, "caster");
	hit.actions.push_back(action);
	return 0;
}

// c:teleport(who) -- to a random cell of the map, as Teleport does; not on
// a map that forbids teleporting.
int32 lua_hit_teleport(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "teleport");

	hit.actions.push_back({ SKILL_LUA_TELEPORT, unit_arg(state, 2, hit) });
	return 0;
}

const char* const mount_names[SKILL_LUA_MOUNT_MAX] = { "peco", "dragon", "warg", "mado", "falcon", "cart" };

// c:mount("mado", on, {who}) -- put a mount, falcon or cart on a player (on
// true) or take it off: "peco", "dragon", "warg" (riding it), "mado",
// "falcon", "cart". The caster by default. Whether the class shows it is
// the client's affair.
int32 lua_hit_mount(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "mount");
	int32 kind = name_index(luaL_checkstring(state, 2), mount_names, SKILL_LUA_MOUNT_MAX);

	if (kind < 0)
		return luaL_error(state, "c:mount: what is %s", name_list(mount_names, SKILL_LUA_MOUNT_MAX).c_str());

	s_skill_lua_action action = { SKILL_LUA_MOUNT };

	action.type = kind;
	action.flag = lua_toboolean(state, 3) ? 1 : 0;
	action.unit_id = unit_arg(state, 4, hit, "caster");
	hit.actions.push_back(action);
	return 0;
}

int32 lua_hit_chance(lua_State* state) {
	lua_Integer n = luaL_checkinteger(state, 2);
	lua_pushboolean(state, rnd() % 10000 < n);
	return 1;
}

// c:apply_status("TC_TOXIN", { stacks = 1, duration = 5000, source = unit },
// {who}) -- put a mod's status on "target" (default), "caster" or a unit.
// The status is applied by the hook's owner, or by `source`.
int32 lua_hit_apply_status(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "apply_status");
	const char* name = luaL_checkstring(state, 2);
	const s_lua_status* def = custom_status(name);

	if (def == nullptr)
		return luaL_error(state, "%s is not a status a mod has defined (status(\"%s\", { ... }))", name, name);

	s_skill_lua_action action = { SKILL_LUA_APPLY_STATUS };

	action.type = def->index;
	action.val1 = 1;
	action.source_id = current_owner != 0 ? current_owner : hit.src_id;
	if (lua_istable(state, 3)) {
		lua_getfield(state, 3, "stacks");
		action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_optinteger(state, -1, 1), 1, 10000));
		lua_getfield(state, 3, "duration");
		action.duration = std::clamp<lua_Integer>(luaL_optinteger(state, -1, 0), 0, 86400000);
		lua_getfield(state, 3, "source");
		if (int32 source = unit_id_at(state, -1); source != 0)
			action.source_id = source;
		lua_pop(state, 3);
	} else if (!lua_isnoneornil(state, 3)) {
		return luaL_error(state, "c:apply_status: the second argument is a table like { stacks = 1, duration = 5000 }");
	}
	action.unit_id = unit_arg(state, 4, hit);
	hit.actions.push_back(action);
	return 0;
}

// c:remove_status("TC_TOXIN", {who}, {stacks}, {source}) -- take stacks (all
// by default) of a mod's status off "target" (default), "caster" or a unit.
// `source`, a unit or true for the hook's owner: only the stacks it applied.
int32 lua_hit_remove_status(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "remove_status");
	const char* name = luaL_checkstring(state, 2);
	const s_lua_status* def = custom_status(name);

	if (def == nullptr)
		return luaL_error(state, "%s is not a status a mod has defined", name);

	s_skill_lua_action action = { SKILL_LUA_REMOVE_STATUS };

	action.type = def->index;
	action.unit_id = unit_arg(state, 3, hit);
	action.val1 = lua_isnoneornil(state, 4) ? -1 : static_cast<int32>(std::max<lua_Integer>(0, luaL_checkinteger(state, 4)));
	if (lua_istable(state, 5))
		action.source_id = unit_id_at(state, 5);
	else if (lua_toboolean(state, 5))
		action.source_id = current_owner != 0 ? current_owner : hit.src_id;
	hit.actions.push_back(action);
	return 0;
}

// c:cut_delay(ms, {who}) -- the caster (default) can act again that much sooner.
int32 lua_hit_cut_delay(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "cut_delay");
	s_skill_lua_action action = { SKILL_LUA_CUT_DELAY };

	action.duration = std::clamp<lua_Integer>(luaL_checkinteger(state, 2), 0, 60000);
	action.unit_id = unit_arg(state, 3, hit, "caster");
	hit.actions.push_back(action);
	return 0;
}

// c:give_item("POISON_BOTTLE", {amount}, {who}, {opts}) -- an item for a
// player, the caster by default. Nothing for anything else. opts.floor drops
// it at the player's feet instead of into the bag.
int32 lua_hit_give_item(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "give_item");
	s_skill_lua_action action = { SKILL_LUA_GIVE_ITEM };

	action.type = static_cast<int32>(check_item(state, 2, "c:give_item")->nameid);
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_optinteger(state, 3, 1), 1, MAX_AMOUNT));
	action.unit_id = unit_arg(state, 4, hit, "caster");
	action.flag = opt_bool(state, 5, "floor") ? 1 : 0;
	hit.actions.push_back(action);
	return 0;
}

// c:cut_cooldown("GC_CROSSIMPACT", ms, {who}) -- that skill's cooldown, for
// the caster (default), ends that much sooner. A player's, a homunculus's
// or a mercenary's.
int32 lua_hit_cut_cooldown(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "cut_cooldown");
	uint16 skill_id = check_skill(state, 2, "c:cut_cooldown");
	s_skill_lua_action action = { SKILL_LUA_CUT_COOLDOWN };

	action.type = skill_id;
	action.duration = std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 0, 86400000);
	action.unit_id = unit_arg(state, 4, hit, "caster");
	hit.actions.push_back(action);
	return 0;
}

// c:cooldown("GC_CROSSIMPACT", ms, {who}) -- start that skill's cooldown for
// the caster (default), as a use of it would: `ms` from now, replacing one
// that is running; 0 ends it. The player's hotkey bar shows it, and it is
// saved over a logout like any cooldown. A player's, a homunculus's or a
// mercenary's.
int32 lua_hit_cooldown(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "cooldown");
	s_skill_lua_action action = { SKILL_LUA_COOLDOWN };

	action.type = check_skill(state, 2, "c:cooldown");
	action.duration = std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 0, 86400000);
	action.unit_id = unit_arg(state, 4, hit, "caster");
	hit.actions.push_back(action);
	return 0;
}

// c:hurt(hp, {who}) -- the target (default) loses that much HP, dealt by
// the hook's caster: no defence, element or card applies. For damage over
// time, from a status's tick. (Not c:damage: c.damage is the blow's damage.)
int32 lua_hit_hurt(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "hurt");
	s_skill_lua_action action = { SKILL_LUA_DAMAGE };

	action.hp = std::clamp<lua_Integer>(luaL_checkinteger(state, 2), 0, INT32_MAX);
	action.unit_id = unit_arg(state, 3, hit);
	hit.actions.push_back(action);
	return 0;
}

/// Set while an on_death hook runs: where c:survive() records its answer.
bool* current_survive = nullptr;

// c:survive() -- in on_death: the dying player keeps 1 HP instead.
int32 lua_hit_survive(lua_State* state) {
	if (current_survive == nullptr)
		return luaL_error(state, "c:survive() only works inside on_death");
	*current_survive = true;
	return 0;
}


/// The player whose bonus hooks are running, for c:bonus().
map_session_data* bonus_sd = nullptr;

/// One value of a bonus: a number, a script constant ("RC_Boss", "Ele_Fire")
/// or a skill name ("AS_SONICBLOW").
int32 bonus_value(lua_State* state, int32 arg) {
	if (lua_type(state, arg) == LUA_TNUMBER)
		return static_cast<int32>(lua_tointeger(state, arg));

	const char* name = luaL_checkstring(state, arg);
	int64 constant;

	if (script_get_constant(name, &constant))
		return static_cast<int32>(constant);
	if (uint16 skill_id = skill_name2id(name); skill_id != 0)
		return skill_id;
	return luaL_error(state, "%s is neither a constant the server knows nor a skill name", name);
}

/// c:bonus("bStr", 5) and c:bonus2..5: an item bonus for the player, by name.
int32 lua_bonus_n(lua_State* state, int32 values) {
	if (bonus_sd == nullptr)
		return luaL_error(state, "c:bonus() only works inside a bonus hook");

	const char* name = luaL_checkstring(state, 2);
	int64 type;

	if (!script_get_constant(name, &type) || strncmp(name, "b", 1) != 0)
		return luaL_error(state, "%s is not a bonus (see doc/item_bonus.txt, e.g. bStr, bSkillAtk)", name);

	int32 v[5] = {};

	for (int32 i = 0; i < values; i++)
		v[i] = bonus_value(state, 3 + i);

	switch (values) {
		case 1: pc_bonus(bonus_sd, static_cast<int32>(type), v[0]); break;
		case 2: pc_bonus2(bonus_sd, static_cast<int32>(type), v[0], v[1]); break;
		case 3: pc_bonus3(bonus_sd, static_cast<int32>(type), v[0], v[1], v[2]); break;
		case 4: pc_bonus4(bonus_sd, static_cast<int32>(type), v[0], v[1], v[2], v[3]); break;
		case 5: pc_bonus5(bonus_sd, static_cast<int32>(type), v[0], v[1], v[2], v[3], v[4]); break;
	}
	return 0;
}

int32 lua_bonus1(lua_State* state) { return lua_bonus_n(state, 1); }
int32 lua_bonus2(lua_State* state) { return lua_bonus_n(state, 2); }
int32 lua_bonus3(lua_State* state) { return lua_bonus_n(state, 3); }
int32 lua_bonus4(lua_State* state) { return lua_bonus_n(state, 4); }
int32 lua_bonus5(lua_State* state) { return lua_bonus_n(state, 5); }

// c:grant_skill("RG_TUNNELDRIVE", 1) -- in a bonus hook: the player has that
// skill at that level while the bonus holds, the way a card grants one.
int32 lua_grant_skill(lua_State* state) {
	if (bonus_sd == nullptr)
		return luaL_error(state, "c:grant_skill() only works inside a bonus hook");

	uint16 skill_id = skill_name2id(luaL_checkstring(state, 2));

	if (skill_id == 0)
		return luaL_error(state, "there is no skill %s", lua_tostring(state, 2));
	pc_skill(bonus_sd, skill_id, static_cast<int32>(std::clamp<lua_Integer>(luaL_optinteger(state, 3, 1), 1, MAX_SKILL_LEVEL)), ADDSKILL_TEMP);
	return 0;
}

/// The metatable every context `c` shares: c:chance(), the actions, the
/// bonus functions and c:survive(). Each refuses, with the reason, where it
/// does not apply: an action outside a hook that can act, c:bonus outside a
/// bonus hook, c:survive outside on_death.
int32 context_meta_ref = LUA_NOREF;

void register_context_methods() {
	static const luaL_Reg methods[] = {
		{ "chance", lua_hit_chance },
		{ "drain", lua_hit_drain },               { "heal", lua_hit_heal },
		{ "status", lua_hit_status },             { "end_status", lua_hit_end_status },
		{ "status_val", lua_hit_status_val },     { "polymorph", lua_hit_polymorph },
		{ "cast", lua_hit_cast },                 { "attack", lua_hit_attack },
		{ "apply_status", lua_hit_apply_status }, { "remove_status", lua_hit_remove_status },
		{ "cut_delay", lua_hit_cut_delay },       { "cut_cooldown", lua_hit_cut_cooldown },
		{ "cooldown", lua_hit_cooldown },
		{ "give_item", lua_hit_give_item },       { "take_item", lua_hit_take_item },
		{ "hurt", lua_hit_hurt },                 { "spend", lua_hit_spend },
		{ "spheres", lua_hit_spheres },           { "slide", lua_hit_slide },
		{ "knockback", lua_hit_knockback },       { "interrupt", lua_hit_interrupt },
		{ "summon", lua_hit_summon },             { "set_target", lua_hit_set_target },
		{ "copy_skill", lua_hit_copy_skill },     { "itemskill", lua_hit_itemskill },
		{ "place_unit", lua_hit_place_unit },     { "end_unit", lua_hit_end_unit },
		{ "expire", lua_hit_expire },             { "produce_menu", lua_hit_produce_menu },
		{ "mount", lua_hit_mount },               { "teleport", lua_hit_teleport },
		{ "survive", lua_hit_survive },
		{ "bonus", lua_bonus1 },                  { "bonus2", lua_bonus2 },
		{ "bonus3", lua_bonus3 },                 { "bonus4", lua_bonus4 },
		{ "bonus5", lua_bonus5 },                 { "grant_skill", lua_grant_skill },
		{ nullptr, nullptr },
	};

	lua_createtable(L, 0, 1);
	lua_createtable(L, 0, static_cast<int32>(sizeof(methods) / sizeof(methods[0])));
	luaL_setfuncs(L, methods, 0);
	lua_setfield(L, -2, "__index");
	context_meta_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

/// Extra fields a hook about an attack (on_hit, on_attack, on_hit_taken,
/// damage_dealt, damage_taken) wants on `c` but a chain hook (ratio, hit,
/// element) does not need. Null for a hook that is not about an attack.
struct s_damage_ctx {
	int64 damage;
	int32 dmg_lv;          ///< ATK_* (ATK_DEF means it connected)
	int32 element;         ///< ELE_*
	int32 attack_type;     ///< BF_WEAPON / BF_MAGIC / BF_MISC mask, with BF_SHORT / BF_LONG
	bool critical;
	uint16 skill_id = 0;   ///< the attack's skill; 0 for a normal attack
	uint16 skill_lv = 0;
	int32 hits = -1;       ///< damage hooks: how many hits it shows (-1: not known here)
	int64 damage_left = -1;  ///< damage hooks: the left hand's part (-1: not known here)
	int32 targets = 0;     ///< damage hooks: how many targets the damage is divided between (0: it is not)
};

const char* attack_type_name(int32 attack_type) {
	if (attack_type & BF_MAGIC) return "magic";
	if (attack_type & BF_MISC) return "misc";
	return "weapon";
}

/// The context table `c` every hook gets. c.skill, c.skill_id and c.skill_lv
/// are the skill the hook belongs to (in a learner hook, the level learned);
/// an item's or a status's hook sees the attack's skill there instead. The
/// actions work while a hit collects what the hook asks for.
void push_context(uint16 skill_id, uint16 skill_lv, const block_list* src, const block_list* target, const s_damage_ctx* dmg) {
	lua_createtable(L, 0, 32);
	set_string("skill", skill_id != 0 ? skill_get_name(skill_id) : "");
	set_int("skill_id", skill_id);
	set_int("skill_lv", skill_lv);
	push_unit(src);
	lua_setfield(L, -2, "caster");
	push_unit(target);
	lua_setfield(L, -2, "target");
	lua_rawgeti(L, LUA_REGISTRYINDEX, context_meta_ref);
	lua_setmetatable(L, -2);

	// The map it happens on: the caster's, or the target's.
	if (const block_list* where = src != nullptr ? src : target; where != nullptr && where->m >= 0) {
		set_bool("pvp", map_getmapflag(where->m, MF_PVP) != 0);
		set_bool("gvg", map_flag_gvg2(where->m));
		set_bool("bg", map_getmapflag(where->m, MF_BATTLEGROUND) != 0);
	}

	if (dmg != nullptr) {
		set_int("damage", dmg->damage);
		set_int("element", dmg->element);
		set_bool("connected", dmg->dmg_lv >= ATK_DEF && dmg->damage > 0);
		set_bool("critical", dmg->critical);
		set_bool("missed", dmg->dmg_lv == ATK_LUCKY || dmg->dmg_lv == ATK_FLEE || dmg->dmg_lv == ATK_MISS);
		set_bool("perfect_dodge", dmg->dmg_lv == ATK_LUCKY);
		set_bool("ranged", (dmg->attack_type & BF_LONG) != 0);
		set_string("weapon_type", attack_type_name(dmg->attack_type));
		set_string("attack_skill", dmg->skill_id != 0 ? skill_get_name(dmg->skill_id) : "");
		set_int("attack_skill_id", dmg->skill_id);
		set_int("attack_skill_lv", dmg->skill_lv);
		set_bool("blocked", dmg->dmg_lv == ATK_BLOCK);
		// A skill the server cast on its own: an Auto Spell, an autocast item.
		const map_session_data* ssd = src != nullptr ? BL_CAST(BL_PC, src) : nullptr;
		set_bool("autocast", ssd != nullptr && ssd->state.autocast);
		if (dmg->hits >= 0)
			set_int("hits", dmg->hits);
		if (dmg->damage_left >= 0)
			set_int("damage_left", dmg->damage_left);
		if (dmg->targets > 0)
			set_int("targets", dmg->targets);
	}
}

/// c.status_name, c.stacks, c.remaining, c.unit and c.source: what a hook of
/// a mod's status sees about the status, onto the context at the top. (Not
/// c.status: c:status() is an action.)
void add_status_fields(const s_lua_status& def, const s_lua_status_on& on, const block_list& unit) {
	set_string("status_name", def.name.c_str());
	set_int("stacks", on.stacks);
	set_int("remaining", on.expires == 0 ? -1 : std::max<t_tick>(0, DIFF_TICK(on.expires, gettick())));
	push_unit(&unit);
	lua_setfield(L, -2, "unit");
	push_unit(on.source_id != 0 ? map_id2bl(on.source_id) : nullptr);
	lua_setfield(L, -2, "source");
}

/// What a hook returned, at the top of the stack: a number replaces `value`,
/// nil keeps it, anything else is reported and kept.
void read_number(const s_hook& hook, const char* what, const char* owner, int64& value) {
	if (lua_isinteger(L, -1)) {
		value = lua_tointeger(L, -1);
	} else if (lua_type(L, -1) == LUA_TNUMBER) {
		value = static_cast<int64>(lua_tonumber(L, -1));  // toward zero, as the C++ would
	} else if (!lua_isnil(L, -1)) {
		ShowError("Lua: %s's %s hook for %s returned a %s, not a number; keeping the running value.\n", hook.mod.c_str(), what, owner, luaL_typename(L, -1));
	}
}

/// Call a hook whose function and context are pushed with `value` as its
/// second argument; what it returns replaces `value` (see read_number).
void call_number(s_hook& hook, const char* what, const char* owner, int64& value) {
	lua_pushinteger(L, value);
	if (!protected_call(hook, owner, 2, 1))
		return;
	read_number(hook, what, owner, value);
	lua_pop(L, 1);
}

/// ratio, hit and element: `value` is the stock result on the way in. Every
/// registered hook runs in priority order; each sees what the previous one
/// returned as `stock`, and returning nil keeps the running value. A hook
/// that fails is switched off and the chain carries on with the rest.
void call_number_hook(e_hook which, uint16 skill_id, uint16 skill_lv, const block_list* src, const block_list* target, int64& value, const int64* base = nullptr) {
	s_skill_hooks* hooks = hooks_for(skill_id);

	if (hooks == nullptr || hooks->hooks[which].empty())
		return;

	for (s_hook& hook : hooks->hooks[which]) {
		if (hook.ref == LUA_NOREF)
			continue;  // this one failed earlier and is off until restart

		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, skill_lv, src, target, nullptr);
		if (base != nullptr)
			set_int("base", *base);  // heal: the amount before the skill's own bonus
		call_number(hook, hook_names[which], hooks->skill.c_str(), value);
	}
}

/// Each skill `sd` has learned that has a `which` hook, with its level.
template <typename F>
void for_each_learned(e_hook which, const map_session_data& sd, F&& fn) {
	for (uint16 skill_id : learner_skills[which]) {
		int32 lv = pc_checkskill(&sd, skill_id);
		s_skill_hooks* hooks = lv > 0 ? hooks_for(skill_id) : nullptr;

		if (hooks == nullptr)
			continue;
		for (s_hook& hook : hooks->hooks[which])
			if (hook.ref != LUA_NOREF)
				fn(skill_id, static_cast<uint16>(lv), hook, hooks->skill.c_str());
	}
}

/// Each mod status on `bl` that has a `which` hook.
template <typename F>
void for_each_status(e_status_hook which, const block_list& bl, F&& fn) {
	auto it = statuses_on.find(bl.id);

	if (it == statuses_on.end())
		return;

	// A copy: what the hooks ask for may change the statuses on the unit.
	const std::vector<s_lua_status_on> on_unit = it->second;

	for (const s_lua_status_on& on : on_unit) {
		s_lua_status& def = *status_list[on.index];

		for (s_hook& hook : def.hooks[which])
			if (hook.ref != LUA_NOREF)
				fn(def, on, hook);
	}
}

/// Whether a mod status on the unit has a `which` hook.
bool has_status_hook(int32 unit_id, e_status_hook which) {
	auto it = statuses_on.find(unit_id);

	if (it == statuses_on.end())
		return false;
	for (const s_lua_status_on& on : it->second)
		if (!status_list[on.index]->hooks[which].empty())
			return true;
	return false;
}

/// A hit to collect what hooks ask for, from `src` to `target` (either may
/// be null).
std::unique_ptr<s_skill_lua_hit> new_hit(const block_list* src, const block_list* target, uint16 skill_id) {
	auto hit = std::make_unique<s_skill_lua_hit>();

	hit->src_id = src != nullptr ? src->id : 0;
	hit->target_id = target != nullptr ? target->id : 0;
	hit->skill_id = skill_id;
	if (target != nullptr) {
		const status_data* st = status_get_status_data(*target);

		hit->race = st->race;
		hit->class_ = st->class_;
	}
	return hit;
}

/// While it lives, the actions hooks ask for go to `hit`.
class acting_for {
	s_skill_lua_hit* previous_;

public:
	explicit acting_for(s_skill_lua_hit* hit) : previous_(current_hit) { current_hit = hit; }
	~acting_for() { current_hit = previous_; }
	acting_for(const acting_for&) = delete;
	acting_for& operator=(const acting_for&) = delete;
};

/// Call a hook whose function and context are pushed, for `owner`: the unit
/// whose skill, item or status it is.
bool call_owned(s_hook& hook, const char* name, const block_list* owner, int32 nargs = 1, int32 nresults = 0) {
	int32 previous = current_owner;

	current_owner = owner != nullptr ? owner->id : 0;

	bool ok = protected_call(hook, name, nargs, nresults);

	current_owner = previous;
	return ok;
}

/// level and job_level: the level a skill's formula reads for `bl`. Runs by
/// skill id, so it reaches skills that have no class. There is no target and
/// no skill level here: a hook sees c.caster only.
int32 call_level_hook(e_hook which, uint16 skill_id, const block_list* bl, int32 stock) {
	int64 value = stock;

	call_number_hook(which, skill_id, 0, bl, nullptr, value);

	if (value == stock)
		return stock;  // no hook, or none changed it: the stock level, untouched
	return static_cast<int32>(std::clamp<int64>(value, 0, MAX_LEVEL));
}

// ---------------------------------------------------------------------------
// The wrapper around a skill's own class
// ---------------------------------------------------------------------------

/// before_cast: a use of the skill is about to take effect (once per use:
/// not for each target an area or a party buff goes on to). What the hooks
/// ask for happens before the skill's own effect. A hook that returns false
/// stops that effect: the use is paid for and nothing else happens. Returns
/// whether the effect goes ahead.
bool call_before_cast(uint16 skill_id, uint16 skill_lv, block_list* src, block_list* target, int32 x, int32 y, int32 flag) {
	s_skill_hooks* hooks = hooks_for(skill_id);

	if (hooks == nullptr || hooks->hooks[HOOK_BEFORE_CAST].empty() || (flag & 1) || src == nullptr || current_hit != nullptr || in_lua_cast)
		return true;

	std::unique_ptr<s_skill_lua_hit> hit = new_hit(src, target, skill_id);
	bool go_ahead = true;

	{
		acting_for acting(hit.get());

		for (s_hook& hook : hooks->hooks[HOOK_BEFORE_CAST]) {
			if (hook.ref == LUA_NOREF)
				continue;
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, skill_lv, src, target, nullptr);
			set_int("x", x);
			set_int("y", y);
			if (call_owned(hook, hooks->skill.c_str(), src, 1, 1)) {
				if (lua_isboolean(L, -1) && !lua_toboolean(L, -1))
					go_ahead = false;
				lua_pop(L, 1);
			}
		}
	}
	skill_lua_apply(hit);
	return go_ahead;
}

/// The skill effects running now, by caster and skill. Some stock classes
/// call a castend again for the same use (Windmill's damage, Counter Slash,
/// an acid zone's splash): before_cast runs only on the way in.
std::vector<std::pair<int32, uint16>> running_effects;

class running_effect {
	bool nested_;

public:
	running_effect(const block_list* src, uint16 skill_id) {
		std::pair<int32, uint16> key(src != nullptr ? src->id : 0, skill_id);

		nested_ = std::find(running_effects.begin(), running_effects.end(), key) != running_effects.end();
		running_effects.push_back(key);
	}
	~running_effect() { running_effects.pop_back(); }
	running_effect(const running_effect&) = delete;
	running_effect& operator=(const running_effect&) = delete;

	bool nested() const { return nested_; }
};

class LuaSkillImpl : public SkillImpl {
	std::unique_ptr<const SkillImpl> stock_;

public:
	LuaSkillImpl(e_skill skill_id, std::unique_ptr<const SkillImpl> stock) : SkillImpl(skill_id), stock_(std::move(stock)) {}

	void castendNoDamageId(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32& flag) const override {
		running_effect effect(src, skill_id_);

		if (effect.nested() || call_before_cast(skill_id_, skill_lv, src, target, target != nullptr ? target->x : 0, target != nullptr ? target->y : 0, flag))
			stock_->castendNoDamageId(src, target, skill_lv, tick, flag);
	}
	void castendDamageId(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32& flag) const override {
		running_effect effect(src, skill_id_);

		if (effect.nested() || call_before_cast(skill_id_, skill_lv, src, target, target != nullptr ? target->x : 0, target != nullptr ? target->y : 0, flag))
			stock_->castendDamageId(src, target, skill_lv, tick, flag);
	}
	void castendPos2(block_list* src, int32 x, int32 y, uint16 skill_lv, t_tick tick, int32& flag) const override {
		running_effect effect(src, skill_id_);

		if (effect.nested() || call_before_cast(skill_id_, skill_lv, src, nullptr, x, y, flag))
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

		// hits: how many hits the skill deals. A negative count is the
		// server's way to show them as one; the sign is kept.
		int64 value = dmg.div_ < 0 ? -dmg.div_ : dmg.div_;
		const int64 stock = value;

		call_number_hook(HOOK_HITS, skill_id_, skill_lv, &src, &target, value);
		if (value != stock) {
			int16 hits = static_cast<int16>(std::clamp<int64>(value, 1, 100));

			dmg.div_ = dmg.div_ < 0 ? -hits : hits;
		}
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

/// The class a mod's own skill gets (custom = true), or a skill it reworks
/// (replace = true). It does what the skill's skill_db entry says:
///  - an attack hits its target, and everything around it with a SplashArea;
///  - a Self or Support skill starts its Status on the target; a Self skill
///    that deals damage and has a SplashArea hits everything around the caster;
///  - a Ground skill places its Unit (whose Interval hits, as Dummyskill
///    does), or hits its area once when it has none.
/// The damage itself is the skill's Type and the mod's ratio hook. Anything
/// else is the mod's on_cast.
class LuaGenericSkillImpl : public SkillImplRecursiveDamageSplash {
public:
	explicit LuaGenericSkillImpl(e_skill skill_id) : SkillImplRecursiveDamageSplash(skill_id) {}

	void castendDamageId(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32& flag) const override {
		if ((flag & 1) || skill_get_splash(skill_id_, skill_lv) > 0)
			SkillImplRecursiveDamageSplash::castendDamageId(src, target, skill_lv, tick, flag);
		else
			skill_attack(skill_get_type(skill_id_), src, src, target, skill_id_, skill_lv, tick, flag);
	}

	void castendNoDamageId(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32& flag) const override {
		if (src == target && skill_get_splash(skill_id_, skill_lv) > 0 && skill_get_type(skill_id_) != BF_NONE && !skill_get_nk(skill_id_, NK_NODAMAGE)) {
			// Around the caster, the way an attack splashes around its target.
			skill_area_temp[0] = 0;
			skill_area_temp[1] = src->id;
			skill_area_temp[2] = 0;
			splashSearch(src, src, skill_lv, tick, flag);
		}

		sc_type type = skill_get_sc(skill_id_);
		bool started = type == SC_NONE || sc_start(src, target, type, 100, skill_lv, skill_get_time(skill_id_, skill_lv));

		clif_skill_nodamage(src, *target, skill_id_, skill_lv, started);
	}

	void castendPos2(block_list* src, int32 x, int32 y, uint16 skill_lv, t_tick tick, int32& flag) const override {
		if (skill_get_unit_id(skill_id_) != 0) {
			skill_unitsetting(src, skill_id_, skill_lv, static_cast<int16>(x), static_cast<int16>(y), 0);
			return;
		}
		skill_area_temp[0] = 0;
		skill_area_temp[1] = 0;
		SkillImplRecursiveDamageSplash::castendPos2(src, x, y, skill_lv, tick, flag);
	}
};

// ---------------------------------------------------------------------------
// The functions a mod's file can call while it loads
// ---------------------------------------------------------------------------

/// The optional `priority` of a registration (table at 2): 0..10, default 5.
/// It applies to every hook in the call.
int32 read_priority(lua_State* state, const char* kind, const char* name) {
	int32 priority = PRIORITY_DEFAULT;

	lua_getfield(state, 2, "priority");
	if (!lua_isnil(state, -1)) {
		if (!lua_isinteger(state, -1))
			return luaL_error(state, "%s %s: priority must be a whole number between %d and %d", kind, name, PRIORITY_MIN, PRIORITY_MAX);
		lua_Integer p = lua_tointeger(state, -1);
		if (p < PRIORITY_MIN || p > PRIORITY_MAX)
			return luaL_error(state, "%s %s: priority %d is outside the %d..%d range", kind, name, static_cast<int32>(p), PRIORITY_MIN, PRIORITY_MAX);
		priority = static_cast<int32>(p);
	}
	lua_pop(state, 1);
	return priority;
}

/// Add the function at the top (popped) to a chain of hooks. A mod that
/// registers the same hook twice replaces its own previous entry rather than
/// stacking against itself; the chain stays in priority order, ties broken
/// by load order.
void add_hook(lua_State* state, std::vector<s_hook>& chain, int32 priority) {
	s_hook hook;

	hook.mod = current_mod;
	hook.priority = priority;
	hook.load_order = next_load_order++;
	hook.ref = luaL_ref(state, LUA_REGISTRYINDEX);  // pops the value

	auto existing = std::find_if(chain.begin(), chain.end(), [&](const s_hook& h) { return h.mod == current_mod; });

	if (existing != chain.end()) {
		luaL_unref(state, LUA_REGISTRYINDEX, existing->ref);
		*existing = hook;
	} else {
		chain.push_back(hook);
	}
	std::stable_sort(chain.begin(), chain.end(), [](const s_hook& a, const s_hook& b) {
		if (a.priority != b.priority)
			return a.priority < b.priority;
		return a.load_order < b.load_order;
	});
}

/// The index of `key` among `names`, or -1.
int32 name_index(const char* key, const char* const* names, int32 count) {
	for (int32 i = 0; i < count; i++)
		if (strcmp(key, names[i]) == 0)
			return i;
	return -1;
}

/// "a, b, c": `names` for an error message.
std::string name_list(const char* const* names, int32 count) {
	std::string list;

	for (int32 i = 0; i < count; i++)
		list += (i > 0 ? ", " : "") + std::string(names[i]);
	return list;
}

// skill("NJ_KAENSIN", { priority = 3, ratio = ..., on_hit = ... })
// Every mod that registers for the same (skill, hook) is kept: at call time
// they run in ascending priority order, ties broken by load order.
// custom = true and replace = true give the skill the generic class
// (LuaGenericSkillImpl), see skill_lua_attach.
int32 lua_register_skill(lua_State* state) {
	const char* name = luaL_checkstring(state, 1);
	luaL_checktype(state, 2, LUA_TTABLE);

	if (!loading)
		return luaL_error(state, "skill() only works while the mod's files load, not from a hook");

	s_skill_hooks& entry = hooks_by_name[name];
	entry.skill = name;

	int32 priority = read_priority(state, "skill", name);

	lua_pushnil(state);
	while (lua_next(state, 2) != 0) {
		const char* key = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : "";

		if (strcmp(key, "priority") == 0) {
			lua_pop(state, 1);
			continue;
		}
		if (strcmp(key, "custom") == 0 || strcmp(key, "replace") == 0 || strcmp(key, "stock_checks") == 0) {
			if (!lua_isboolean(state, -1))
				return luaL_error(state, "skill %s: %s must be true or false", name, key);
			(key[0] == 'c' ? entry.custom : key[0] == 'r' ? entry.replace : entry.stock_checks) = lua_toboolean(state, -1);
			lua_pop(state, 1);
			continue;
		}

		int32 which = name_index(key, hook_names, HOOK_MAX);

		if (which < 0)
			return luaL_error(state, "skill %s: unknown key \"%s\" (a hook: %s; or priority, custom, replace, stock_checks)", name, key, name_list(hook_names, HOOK_MAX).c_str());
		if (!lua_isfunction(state, -1))
			return luaL_error(state, "skill %s: %s must be a function", name, key);
		add_hook(state, entry.hooks[which], priority);
	}
	return 0;
}

// item("VORPAL_BLADE", { priority = 3, on_attack = ..., on_hit_taken = ... })
// Same chaining and priority rules as skill(), keyed by the item's
// AegisName. The nameid is resolved at skill_lua_attach time, so a typo
// shows up in the log as "no item called X" rather than a silent no-op.
int32 lua_register_item(lua_State* state) {
	const char* name = luaL_checkstring(state, 1);
	luaL_checktype(state, 2, LUA_TTABLE);

	if (!loading)
		return luaL_error(state, "item() only works while the mod's files load, not from a hook");

	s_item_hooks& entry = item_hooks_by_name[name];
	entry.aegis = name;

	int32 priority = read_priority(state, "item", name);

	lua_pushnil(state);
	while (lua_next(state, 2) != 0) {
		const char* key = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : "";

		if (strcmp(key, "priority") == 0) {
			lua_pop(state, 1);
			continue;
		}

		int32 which = name_index(key, item_hook_names, ITEM_HOOK_MAX);

		if (which < 0)
			return luaL_error(state, "item %s: unknown key \"%s\" (a hook: %s; or priority)", name, key, name_list(item_hook_names, ITEM_HOOK_MAX).c_str());
		if (!lua_isfunction(state, -1))
			return luaL_error(state, "item %s: %s must be a function", name, key);
		add_hook(state, entry.hooks[which], priority);
	}
	return 0;
}

/// A whole number field of a status() table, at the top, kept in [min, max].
int32 status_int(lua_State* state, const char* status, const char* key, int32 min, int32 max) {
	if (!lua_isinteger(state, -1))
		return luaL_error(state, "status %s: %s must be a whole number", status, key);
	return static_cast<int32>(std::clamp<lua_Integer>(lua_tointeger(state, -1), min, max));
}

bool status_bool(lua_State* state, const char* status, const char* key) {
	if (!lua_isboolean(state, -1))
		return luaL_error(state, "status %s: %s must be true or false", status, key) != 0;
	return lua_toboolean(state, -1) != 0;
}

// status("TC_TOXIN", { icon = "EFST_TOXIN", duration = 10000, max_stacks = 10,
//   max_stacks_boss = 3, max_stacks_player = 3, per_source = true,
//   refresh = true, end_on_death = true, decay = { every = 3000, stacks = 1 },
//   persist = false, damage_taken = function(c, damage) ... end, ... })
// A status of the mod's own: a name, stacks, a duration and hooks.
//
// status("SC_HIDING", { on_start = ..., on_end = ... })
// The server's own status, by its name: hooks on its start and end.
//
// Several mods may hook the same status: hooks chain as for skill(); the
// fields are the last mod's.
int32 lua_register_status(lua_State* state) {
	const char* name = luaL_checkstring(state, 1);
	luaL_checktype(state, 2, LUA_TTABLE);

	if (!loading)
		return luaL_error(state, "status() only works while the mod's files load, not from a hook");

	int64 constant = 0;
	bool wildcard = strcmp(name, "*") == 0;
	bool stock = wildcard || (strncmp(name, "SC_", 3) == 0 && script_get_constant(name, &constant) && constant > SC_NONE && constant < SC_MAX);
	s_lua_status& def = statuses_by_name[name];

	if (def.name.empty()) {
		def.name = name;
		if (wildcard) {
			def.sc = SC_MAX;  // every server status; never a key of stock_status_hooks
			any_status_hooks = &def;
		} else if (stock) {
			def.sc = static_cast<sc_type>(constant);
			stock_status_hooks[def.sc] = &def;
		} else {
			def.index = static_cast<int32>(status_list.size());
			status_list.push_back(&def);
		}
	}
	def.mod = current_mod;

	int32 priority = read_priority(state, "status", name);

	lua_pushnil(state);
	while (lua_next(state, 2) != 0) {
		const char* key = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : "";

		if (strcmp(key, "priority") == 0) {
			lua_pop(state, 1);
			continue;
		}

		if (int32 which = name_index(key, status_hook_names, SHOOK_MAX); which >= 0) {
			if (stock && !stock_status_hook(which))
				return luaL_error(state, "status %s is the server's own: it takes on_start, on_end and chance, not %s", name, key);
			if (!stock && which == SHOOK_CHANCE)
				return luaL_error(state, "status %s: chance is for the server's own statuses; a mod's status lands when c:apply_status says so", name);
			if (!lua_isfunction(state, -1))
				return luaL_error(state, "status %s: %s must be a function", name, key);
			add_hook(state, def.hooks[which], priority);
			continue;
		}
		if (stock)
			return luaL_error(state, "status %s is the server's own: it takes on_start, on_end, chance and priority, not %s", name, key);

		if (strcmp(key, "icon") == 0) {
			int64 icon = EFST_BLANK;

			if (lua_isinteger(state, -1))
				icon = lua_tointeger(state, -1);
			else if (lua_type(state, -1) != LUA_TSTRING || !script_get_constant(lua_tostring(state, -1), &icon))
				return luaL_error(state, "status %s: icon must be a status icon, like \"EFST_TOXIN\"", name);
			if (icon < EFST_BLANK || icon >= EFST_MAX)
				return luaL_error(state, "status %s: %d is not a status icon", name, static_cast<int32>(icon));
			def.icon = static_cast<int32>(icon);
		} else if (strcmp(key, "duration") == 0) {
			def.duration = status_int(state, name, key, 0, 86400000);
		} else if (strcmp(key, "max_stacks") == 0) {
			def.max_stacks = status_int(state, name, key, 1, 10000);
		} else if (strcmp(key, "max_stacks_boss") == 0) {
			def.max_stacks_boss = status_int(state, name, key, 0, 10000);
		} else if (strcmp(key, "max_stacks_player") == 0) {
			def.max_stacks_player = status_int(state, name, key, 0, 10000);
		} else if (strcmp(key, "per_source") == 0) {
			def.per_source = status_bool(state, name, key);
		} else if (strcmp(key, "refresh") == 0) {
			def.refresh = status_bool(state, name, key);
		} else if (strcmp(key, "end_on_death") == 0) {
			def.end_on_death = status_bool(state, name, key);
		} else if (strcmp(key, "persist") == 0) {
			def.persist = status_bool(state, name, key);
			// It is kept under its name in the mod store, whose paths are
			// letters, digits and '_'.
			if (def.persist && std::any_of(def.name.begin(), def.name.end(), [](char c) { return !ISALNUM(c) && c != '_'; }))
				return luaL_error(state, "status %s: persist needs a name of letters, digits and '_' only", name);
		} else if (strcmp(key, "decay") == 0) {
			if (!lua_istable(state, -1))
				return luaL_error(state, "status %s: decay is a table like { every = 3000, stacks = 1 }", name);
			lua_getfield(state, -1, "every");
			def.decay_every = lua_isnil(state, -1) ? 0 : status_int(state, name, "decay.every", 250, 86400000);
			lua_pop(state, 1);
			lua_getfield(state, -1, "stacks");
			def.decay_stacks = lua_isnil(state, -1) ? 1 : status_int(state, name, "decay.stacks", 1, 10000);
			lua_pop(state, 1);
		} else {
			return luaL_error(state, "status %s: unknown key \"%s\" (a hook: %s; or icon, duration, max_stacks, max_stacks_boss, max_stacks_player, per_source, refresh, end_on_death, decay, persist, priority)",
				name, key, name_list(status_hook_names, SHOOK_MAX).c_str());
		}
		lua_pop(state, 1);
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

	protected_depth++;

	// "t": text only. Precompiled chunks can crash the VM on purpose.
	int32 status = luaL_loadfilex(L, path.c_str(), "t");

	if (status == LUA_OK)
		status = lua_pcall(L, 0, 0, -2);
	protected_depth--;
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
namespace {

// ---------------------------------------------------------------------------
// store: the mod's own data (mod_store.hpp)
//
//   store.get(path, default)   store.set(path, value)   store.inc(path, by)
//   store.delete(path)         store.exists(path)       store.keys(path)
//   store.count(path)          store.top(path, n)       store.used()
//   store.limits()             store.char(unit)         store.account(unit)
//
// store.char(c.caster) and store.account(c.caster) return the same methods
// for that player's character or account. Writes that fail return nil and a
// reason; nothing here raises an error a hook didn't ask for.
// ---------------------------------------------------------------------------

mod_store::s_target store_target(lua_State* state) {
	mod_store::s_target t;
	t.mod = current_mod == "server" ? "" : current_mod;
	t.scope = static_cast<mod_store::e_scope>(lua_tointeger(state, lua_upvalueindex(1)));
	t.owner = static_cast<uint32>(lua_tointeger(state, lua_upvalueindex(2)));
	return t;
}

std::string store_path(lua_State* state, int32 arg) {
	size_t len = 0;
	const char* text = luaL_optlstring(state, arg, "", &len);
	return std::string(text, len);
}

int32 store_fail(lua_State* state, const std::string& error) {
	lua_pushnil(state);
	lua_pushstring(state, error.c_str());
	return 2;
}

/// Write the Lua value at `index` under `path`: a table becomes a subtree.
bool store_write(lua_State* state, const mod_store::s_target& t, const std::string& path, int32 index, int32 depth, std::string& error) {
	index = lua_absindex(state, index);
	mod_store::s_value value;
	switch (lua_type(state, index)) {
		case LUA_TBOOLEAN:
			value.number = lua_toboolean(state, index) ? 1 : 0;
			break;
		case LUA_TNUMBER: {
			int32 is_integer = 0;
			value.number = lua_tointegerx(state, index, &is_integer);
			if (!is_integer) {
				error = "numbers in the store are integers";
				return false;
			}
			break;
		}
		case LUA_TSTRING: {
			size_t len = 0;
			const char* text = lua_tolstring(state, index, &len);
			value.is_string = true;
			value.text.assign(text, len);
			break;
		}
		case LUA_TTABLE: {
			if (depth > mod_store::depth_limit()) {
				error = "the table is nested too deeply";
				return false;
			}
			lua_pushnil(state);
			while (lua_next(state, index) != 0) {
				std::string child;
				if (lua_type(state, -2) == LUA_TNUMBER && lua_isinteger(state, -2))
					child = std::to_string(lua_tointeger(state, -2));
				else if (lua_type(state, -2) == LUA_TSTRING)
					child = lua_tostring(state, -2);
				else {
					lua_pop(state, 2);
					error = "table keys in the store are strings or integers";
					return false;
				}
				if (!store_write(state, t, path + "." + child, -1, depth + 1, error)) {
					lua_pop(state, 2);
					return false;
				}
				lua_pop(state, 1);
			}
			return true;
		}
		default:
			error = std::string("a ") + luaL_typename(state, index) + " can't be stored";
			return false;
	}
	return mod_store::set(t, path, value, error);
}

void push_store_value(lua_State* state, const mod_store::s_value& value) {
	if (value.is_string)
		lua_pushlstring(state, value.text.data(), value.text.size());
	else
		lua_pushinteger(state, value.number);
}

/// A segment as the table key it was written from: "3" was the integer 3
/// (store_write), so an array read back is an array again.
void push_store_key(lua_State* state, const std::string& segment) {
	bool integer = !segment.empty() && segment.size() <= 18
		&& (segment == "0" || segment[0] != '0')
		&& std::all_of(segment.begin(), segment.end(), [](char c) { return c >= '0' && c <= '9'; });
	if (integer)
		lua_pushinteger(state, std::stoll(segment));
	else
		lua_pushlstring(state, segment.data(), segment.size());
}

/// The value at a path, or everything under it as a table -- what store.set
/// wrote, read back.
int32 lua_store_get(lua_State* state) {
	std::vector<std::pair<std::string, mod_store::s_value>> entries;
	std::string error;
	if (!mod_store::read(store_target(state), store_path(state, 1), entries, error) || entries.empty()) {
		lua_settop(state, 2);  // the default, or nil
		return 1;
	}
	if (entries.size() == 1 && entries[0].first.empty()) {
		push_store_value(state, entries[0].second);
		return 1;
	}
	lua_newtable(state);
	const int32 root = lua_gettop(state);
	for (const auto& entry : entries) {
		lua_pushvalue(state, root);
		size_t start = 0;
		for (size_t dot = entry.first.find('.'); dot != std::string::npos; dot = entry.first.find('.', start)) {
			push_store_key(state, entry.first.substr(start, dot - start));
			lua_pushvalue(state, -1);
			if (lua_gettable(state, -3) != LUA_TTABLE) {
				lua_pop(state, 1);
				lua_newtable(state);
				lua_pushvalue(state, -2);  // key
				lua_pushvalue(state, -2);  // the new table
				lua_settable(state, -5);
			}
			lua_remove(state, -2);  // key
			lua_remove(state, -2);  // parent
			start = dot + 1;
		}
		push_store_key(state, entry.first.substr(start));
		push_store_value(state, entry.second);
		lua_settable(state, -3);
		lua_pop(state, 1);
	}
	return 1;
}

int32 lua_store_set(lua_State* state) {
	std::string error;
	if (lua_isnoneornil(state, 2))
		return store_fail(state, "set needs a value (use store.delete to remove one)");
	if (!store_write(state, store_target(state), store_path(state, 1), 2, 1, error))
		return store_fail(state, error);
	lua_pushboolean(state, 1);
	return 1;
}

int32 lua_store_inc(lua_State* state) {
	int64 result = 0;
	std::string error;
	if (!mod_store::inc(store_target(state), store_path(state, 1), luaL_optinteger(state, 2, 1), result, error))
		return store_fail(state, error);
	lua_pushinteger(state, result);
	return 1;
}

int32 lua_store_delete(lua_State* state) {
	int32 removed = 0;
	std::string error;
	if (!mod_store::remove(store_target(state), store_path(state, 1), removed, error))
		return store_fail(state, error);
	lua_pushinteger(state, removed);
	return 1;
}

int32 lua_store_exists(lua_State* state) {
	bool found = false;
	std::string error;
	mod_store::exists(store_target(state), store_path(state, 1), found, error);
	lua_pushboolean(state, found ? 1 : 0);
	return 1;
}

int32 lua_store_keys(lua_State* state) {
	std::vector<std::string> names;
	std::string error;
	if (!mod_store::keys(store_target(state), store_path(state, 1), names, error))
		return store_fail(state, error);
	lua_createtable(state, static_cast<int32>(names.size()), 0);
	for (size_t i = 0; i < names.size(); ++i) {
		lua_pushstring(state, names[i].c_str());
		lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
	}
	return 1;
}

int32 lua_store_count(lua_State* state) {
	std::vector<std::string> names;
	std::string error;
	mod_store::keys(store_target(state), store_path(state, 1), names, error);
	lua_pushinteger(state, static_cast<lua_Integer>(names.size()));
	return 1;
}

/// store.top(path, n): an array of { name = ..., value = ... }, highest first.
int32 lua_store_top(lua_State* state) {
	std::vector<std::pair<std::string, int64>> rows;
	std::string error;
	lua_Integer n = luaL_optinteger(state, 2, 10);
	if (!mod_store::top(store_target(state), store_path(state, 1), static_cast<size_t>(std::max<lua_Integer>(n, 0)), rows, error))
		return store_fail(state, error);
	lua_createtable(state, static_cast<int32>(rows.size()), 0);
	for (size_t i = 0; i < rows.size(); ++i) {
		lua_createtable(state, 0, 2);
		lua_pushstring(state, rows[i].first.c_str());
		lua_setfield(state, -2, "name");
		lua_pushinteger(state, rows[i].second);
		lua_setfield(state, -2, "value");
		lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
	}
	return 1;
}

int32 lua_store_used(lua_State* state) {
	mod_store::s_target t = store_target(state);
	bool ignored = false;
	std::string error;
	mod_store::exists(t, "_", ignored, error);  // loads the document on first use
	lua_pushinteger(state, static_cast<lua_Integer>(mod_store::used(t)));
	return 1;
}

int32 lua_store_limits(lua_State* state) {
	lua_createtable(state, 0, 5);
	set_int("global", static_cast<lua_Integer>(mod_store::limit(mod_store::SCOPE_GLOBAL)));
	set_int("account", static_cast<lua_Integer>(mod_store::limit(mod_store::SCOPE_ACCOUNT)));
	set_int("char", static_cast<lua_Integer>(mod_store::limit(mod_store::SCOPE_CHAR)));
	set_int("value", static_cast<lua_Integer>(mod_store::value_limit()));
	set_int("depth", static_cast<lua_Integer>(mod_store::depth_limit()));
	return 1;
}

/// Push a table of the store's methods, bound to `scope` and `owner`.
void push_store(lua_State* state, mod_store::e_scope scope, uint32 owner) {
	static const luaL_Reg methods[] = {
		{ "get", lua_store_get },       { "set", lua_store_set },
		{ "inc", lua_store_inc },       { "delete", lua_store_delete },
		{ "exists", lua_store_exists }, { "keys", lua_store_keys },
		{ "count", lua_store_count },   { "top", lua_store_top },
		{ "used", lua_store_used },     { nullptr, nullptr },
	};
	lua_createtable(state, 0, 12);
	for (const luaL_Reg* m = methods; m->name != nullptr; ++m) {
		lua_pushinteger(state, scope);
		lua_pushinteger(state, owner);
		lua_pushcclosure(state, m->func, 2);
		lua_setfield(state, -2, m->name);
	}
}

/// store.char(unit) / store.account(unit): `unit` is a hook's c.caster or
/// c.target, and must be a player.
int32 lua_store_scoped(lua_State* state, mod_store::e_scope scope) {
	luaL_checktype(state, 1, LUA_TTABLE);
	lua_getfield(state, 1, "id");
	int32 id = static_cast<int32>(lua_tointeger(state, -1));
	lua_pop(state, 1);
	map_session_data* sd = map_id2sd(id);
	if (sd == nullptr)
		return store_fail(state, std::string("store.") + mod_store::scope_name(scope) + " needs a player");
	push_store(state, scope, scope == mod_store::SCOPE_CHAR ? sd->status.char_id : sd->status.account_id);
	return 1;
}

int32 lua_store_char(lua_State* state) {
	return lua_store_scoped(state, mod_store::SCOPE_CHAR);
}

int32 lua_store_account(lua_State* state) {
	return lua_store_scoped(state, mod_store::SCOPE_ACCOUNT);
}

void register_store() {
	push_store(L, mod_store::SCOPE_GLOBAL, 0);
	lua_pushcfunction(L, lua_store_limits);
	lua_setfield(L, -2, "limits");
	lua_pushcfunction(L, lua_store_char);
	lua_setfield(L, -2, "char");
	lua_pushcfunction(L, lua_store_account);
	lua_setfield(L, -2, "account");
	lua_setglobal(L, "store");
}

}  // namespace

// ---------------------------------------------------------------------------
// Mods' statuses on units, and the hooks that run on their own
// ---------------------------------------------------------------------------

namespace {

/// skill_attack's dsrc while it works out a hit (skill_lua_dealt_by): the
/// ground skill unit that deals it, for a ground skill's later hits.
block_list* dealt_by = nullptr;

/// The power of what a c:cast or c:place_unit placed, by group id: the later
/// hits of its units deal that % of their damage. `created` (the group's
/// tick) tells it from a later group given the same id.
struct s_unit_power {
	int32 percent;
	t_tick created;
};
std::unordered_map<int32, s_unit_power> unit_power;

/// What hooks asked for where it cannot happen at once (a unit dying, a status
/// starting or ending inside the server's own code): a timer applies it right
/// after.
std::vector<std::unique_ptr<s_skill_lua_hit>> deferred;
int32 deferred_timer = INVALID_TIMER;
int32 sweep_timer = INVALID_TIMER;
int32 tick_timer = INVALID_TIMER;

constexpr int32 SWEEP_INTERVAL = 250;  ///< ms between looks for statuses that expire or decay
constexpr int32 TICK_INTERVAL = 1000;  ///< ms between tick hooks

TIMER_FUNC(skill_lua_deferred) {
	deferred_timer = INVALID_TIMER;

	std::vector<std::unique_ptr<s_skill_lua_hit>> due;

	due.swap(deferred);
	for (std::unique_ptr<s_skill_lua_hit>& hit : due)
		skill_lua_apply(hit);
	return 0;
}

void defer(std::unique_ptr<s_skill_lua_hit> hit) {
	if (hit == nullptr || hit->actions.empty())
		return;
	deferred.push_back(std::move(hit));
	if (deferred_timer == INVALID_TIMER)
		deferred_timer = add_timer(gettick(), skill_lua_deferred, 0, 0);
}

/// How many stacks of `def` `bl` can carry; 0 for none at all.
int32 stack_cap(const block_list& bl, const s_lua_status& def) {
	if (bl.type == BL_PC && def.max_stacks_player >= 0)
		return def.max_stacks_player;
	if (def.max_stacks_boss >= 0 && status_get_status_data(bl)->class_ == CLASS_BOSS)
		return def.max_stacks_boss;
	return def.max_stacks;
}

/// The icon of `def` on `bl` as it now is: the stacks of every instance, the
/// longest time left. clif_status_change sends it to whoever it concerns.
void show_icon(block_list& bl, const s_lua_status& def) {
	if (def.icon == EFST_BLANK)
		return;

	int32 stacks = 0;
	t_tick left = 0;
	bool forever = false;

	if (auto it = statuses_on.find(bl.id); it != statuses_on.end()) {
		for (const s_lua_status_on& on : it->second) {
			if (on.index != def.index)
				continue;
			stacks += on.stacks;
			if (on.expires == 0)
				forever = true;
			else
				left = std::max<t_tick>(left, DIFF_TICK(on.expires, gettick()));
		}
	}
	if (stacks == 0)
		clif_status_change(&bl, def.icon, 0, 0, 0, 0, 0);
	else
		clif_status_change(&bl, def.icon, 1, forever ? INFINITE_TICK : left, stacks, 0, 0);
}

/// Recalculate what `def`'s bonus and stats hooks give `bl`, and tell a
/// player's client the ranges its range hooks change.
void recalc(block_list& bl, const s_lua_status& def) {
	map_session_data* sd = BL_CAST(BL_PC, &bl);

	if (sd != nullptr && !def.hooks[SHOOK_BONUS].empty())
		status_calc_pc(sd, SCO_NONE);
	else if (!def.hooks[SHOOK_STATS].empty())
		status_calc_bl(&bl, { SCB_DEF, SCB_DEF2, SCB_MDEF, SCB_MDEF2, SCB_HIT, SCB_FLEE, SCB_FLEE2, SCB_CRI, SCB_SPEED, SCB_BATK, SCB_WATK, SCB_MATK, SCB_ASPD, SCB_MAXHP, SCB_MAXSP });
	if (sd != nullptr && !def.hooks[SHOOK_RANGE].empty())
		clif_skillinfoblock(*sd);  // the skill list carries each skill's range
}

/// on_start, on_end or tick of one instance of `def` on `unit`. What the hooks
/// ask for is applied right after (`now`), or by a timer when they run inside
/// the server's own code. c.caster is who applied the status.
void status_event(e_status_hook which, block_list& unit, s_lua_status& def, const s_lua_status_on& on, const char* reason, bool now) {
	if (def.hooks[which].empty())
		return;

	block_list* source = on.source_id != 0 ? map_id2bl(on.source_id) : nullptr;
	std::unique_ptr<s_skill_lua_hit> hit = new_hit(source, &unit, 0);

	{
		acting_for acting(hit.get());

		for (s_hook& hook : def.hooks[which]) {
			if (hook.ref == LUA_NOREF)
				continue;
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(0, 0, source, &unit, nullptr);
			add_status_fields(def, on, unit);
			if (reason != nullptr)
				set_string("reason", reason);
			call_owned(hook, def.name.c_str(), &unit);
		}
	}
	if (now)
		skill_lua_apply(hit);
	else
		defer(std::move(hit));
}

/// Put `stacks` of `def` on `bl` for `duration` ms (0: the status's own),
/// applied by `source_id`.
void apply_custom_status(block_list& bl, s_lua_status& def, int32 stacks, int64 duration, int32 source_id) {
	int32 cap = stack_cap(bl, def);

	if (cap <= 0 || stacks <= 0 || status_isdead(bl))
		return;

	t_tick now = gettick();
	int64 lasts = duration > 0 ? duration : def.duration;
	t_tick expires = lasts > 0 ? now + lasts : 0;
	std::vector<s_lua_status_on>& on_unit = statuses_on[bl.id];
	auto it = std::find_if(on_unit.begin(), on_unit.end(), [&](const s_lua_status_on& on) {
		return on.index == def.index && (!def.per_source || on.source_id == source_id);
	});
	bool started = it == on_unit.end();
	bool changed = started;

	if (started) {
		on_unit.push_back({ def.index, source_id, std::min(stacks, cap), expires, 0 });
		it = on_unit.end() - 1;
	} else {
		int32 before = it->stacks;

		it->stacks = std::min(it->stacks + stacks, cap);
		if (def.refresh)
			it->expires = expires;
		changed = it->stacks != before || it->source_id != source_id;
		it->source_id = source_id;  // per_source: the same one; otherwise the latest
	}
	if (def.decay_every > 0)
		it->next_decay = now + def.decay_every;

	const s_lua_status_on on = *it;  // what the hooks ask for may change the list

	show_icon(bl, def);
	if (started)
		status_event(SHOOK_ON_START, bl, def, on, nullptr, false);
	if (changed)
		recalc(bl, def);
}

/// Take `stacks` (-1: all) of `def` off `bl`; only those `source_id` applied,
/// unless 0.
void remove_custom_status(block_list& bl, s_lua_status& def, int32 stacks, int32 source_id) {
	auto it = statuses_on.find(bl.id);

	if (it == statuses_on.end() || stacks == 0)
		return;

	std::vector<s_lua_status_on>& on_unit = it->second;
	std::vector<s_lua_status_on> ended;
	bool changed = false;

	for (auto on = on_unit.begin(); on != on_unit.end() && stacks != 0;) {
		if (on->index != def.index || (source_id != 0 && on->source_id != source_id)) {
			++on;
			continue;
		}
		changed = true;
		if (stacks > 0 && stacks < on->stacks) {
			on->stacks -= stacks;
			break;
		}
		if (stacks > 0)
			stacks -= on->stacks;
		ended.push_back(*on);
		on = on_unit.erase(on);
	}
	if (on_unit.empty())
		statuses_on.erase(it);
	if (!changed)
		return;
	show_icon(bl, def);
	for (const s_lua_status_on& on : ended)
		status_event(SHOOK_ON_END, bl, def, on, "removed", false);
	recalc(bl, def);
}

/// `bl` died: its mod statuses end, all but a player's with end_on_death = false.
void clear_on_death(block_list& bl) {
	auto it = statuses_on.find(bl.id);

	if (it == statuses_on.end())
		return;

	std::vector<s_lua_status_on>& on_unit = it->second;
	std::vector<s_lua_status_on> ended;

	for (auto on = on_unit.begin(); on != on_unit.end();) {
		if (bl.type == BL_PC && !status_list[on->index]->end_on_death) {
			++on;
			continue;
		}
		ended.push_back(*on);
		on = on_unit.erase(on);
	}
	if (on_unit.empty())
		statuses_on.erase(it);

	std::vector<int32> indexes;

	for (const s_lua_status_on& on : ended) {
		status_event(SHOOK_ON_END, bl, *status_list[on.index], on, "death", false);
		if (std::find(indexes.begin(), indexes.end(), on.index) == indexes.end())
			indexes.push_back(on.index);
	}
	for (int32 index : indexes) {
		show_icon(bl, *status_list[index]);
		if (bl.type != BL_MOB)  // a monster is recalculated when it spawns again
			recalc(bl, *status_list[index]);
	}
}

/// How long is left of a cooldown timer `ms` from now, after cutting it.
t_tick cooldown_left(int32 tid, int64 ms) {
	const TimerData* timer = get_timer(tid);

	return timer != nullptr ? DIFF_TICK(timer->tick, gettick()) - ms : 0;
}

/// A unit's cooldown of `skill_id` starts again, `ms` from now (0: ended):
/// a player's, a homunculus's or a mercenary's. A running one is taken off
/// first, as the server's start would leave its timer running.
void set_cooldown(block_list& bl, uint16 skill_id, int64 ms) {
	if (map_session_data* sd = BL_CAST(BL_PC, &bl); sd != nullptr) {
		if (auto it = sd->scd.find(skill_id); it != sd->scd.end()) {
			delete_timer(it->second, skill_blockpc_end);
			sd->scd.erase(it);
		}
		if (ms > 0)
			skill_blockpc_start(*sd, skill_id, ms);
		else if (battle_config.display_status_timers)
			clif_skill_cooldown(*sd, skill_id, 0);
	} else if (homun_data* hd = BL_CAST(BL_HOM, &bl); hd != nullptr) {
		if (auto it = hd->scd.find(skill_id); it != hd->scd.end()) {
			delete_timer(it->second, skill_blockhomun_end);
			hd->scd.erase(it);
		}
		if (ms > 0)
			skill_blockhomun_start(*hd, skill_id, ms);
		else if (battle_config.display_status_timers && hd->master != nullptr)
			clif_skill_cooldown(*hd->master, skill_id, 0);
	} else if (s_mercenary_data* mc = BL_CAST(BL_MER, &bl); mc != nullptr) {
		if (auto it = mc->scd.find(skill_id); it != mc->scd.end()) {
			delete_timer(it->second, skill_blockmerc_end);
			mc->scd.erase(it);
		}
		if (ms > 0)
			skill_blockmerc_start(*mc, skill_id, ms);
	}
}

/// A unit's cooldown of `skill_id` ends `ms` sooner: a player's, a
/// homunculus's or a mercenary's.
void cut_cooldown(block_list& bl, uint16 skill_id, int64 ms) {
	if (map_session_data* sd = BL_CAST(BL_PC, &bl); sd != nullptr) {
		auto it = sd->scd.find(skill_id);

		if (it == sd->scd.end())
			return;

		t_tick left = cooldown_left(it->second, ms);

		delete_timer(it->second, skill_blockpc_end);
		sd->scd.erase(it);
		if (left > 0)
			skill_blockpc_start(*sd, skill_id, left);
		else if (battle_config.display_status_timers)
			clif_skill_cooldown(*sd, skill_id, 0);
	} else if (homun_data* hd = BL_CAST(BL_HOM, &bl); hd != nullptr) {
		auto it = hd->scd.find(skill_id);

		if (it == hd->scd.end())
			return;

		t_tick left = cooldown_left(it->second, ms);

		delete_timer(it->second, skill_blockhomun_end);
		hd->scd.erase(it);
		if (left > 0)
			skill_blockhomun_start(*hd, skill_id, left);
		else if (battle_config.display_status_timers && hd->master != nullptr)
			clif_skill_cooldown(*hd->master, skill_id, 0);
	} else if (s_mercenary_data* mc = BL_CAST(BL_MER, &bl); mc != nullptr) {
		auto it = mc->scd.find(skill_id);

		if (it == mc->scd.end())
			return;

		t_tick left = cooldown_left(it->second, ms);

		delete_timer(it->second, skill_blockmerc_end);
		mc->scd.erase(it);
		if (left > 0)
			skill_blockmerc_start(*mc, skill_id, left);
	}
}

/// `amount` of an item for a player; at their feet when it does not fit, or
/// when `floor` says so.
void give_item(map_session_data& sd, t_itemid nameid, int32 amount, bool floor = false) {
	std::shared_ptr<item_data> data = item_db.find(nameid);

	if (data == nullptr)
		return;

	item it = {};

	it.nameid = nameid;
	it.identify = 1;

	// What does not stack (equipment) comes one at a time, and no more than a
	// bag holds: a full bag drops each one on the floor.
	bool stacks = itemdb_isstackable2(data.get());
	int32 each = stacks ? amount : 1;

	if (!stacks)
		amount = std::min(amount, MAX_INVENTORY);

	for (int32 given = 0; given < amount; given += each) {
		if (floor) {
			map_addflooritem(&it, each, sd.m, sd.x, sd.y, 0, 0, 0, 0, 0);
			continue;
		}
		if (e_additem_result result = pc_additem(&sd, &it, each, LOG_TYPE_SCRIPT); result != ADDITEM_SUCCESS) {
			clif_additem(&sd, 0, 0, result);
			if (pc_candrop(&sd, &it))
				map_addflooritem(&it, each, sd.m, sd.x, sd.y, 0, 0, 0, 0, 0);
		}
	}
}

/// Where a mod's statuses with persist = true are kept, in its char store:
/// _statuses.<name> holds one "stacks,ms left,source,ms to the next decay"
/// for each instance, joined by ';' (-1: no end, no decay).
constexpr const char* PERSIST_PATH = "_statuses";

/// The mods that define a status with persist = true.
std::vector<std::string> persist_mods;

/// The players whose kept statuses are back on them since they logged in,
/// and whether any were kept. Until a player is in it, what is stored stays.
std::unordered_map<int32, bool> persist_loaded;

/// One kept instance of `def` on `sd`, as it was at the logout: it goes on,
/// so on_start does not run again. One put on since the login stays instead.
void restore_instance(map_session_data& sd, const s_lua_status& def, int64 stacks, int64 left, int64 source, int64 decay) {
	int32 cap = stack_cap(sd, def);

	if (cap <= 0 || stacks <= 0)
		return;

	std::vector<s_lua_status_on>& on_unit = statuses_on[sd.id];
	int32 source_id = static_cast<int32>(source);

	if (std::any_of(on_unit.begin(), on_unit.end(), [&](const s_lua_status_on& on) {
			return on.index == def.index && (!def.per_source || on.source_id == source_id);
		}))
		return;

	t_tick now = gettick();
	t_tick expires = left < 0 ? 0 : now + std::max<int64>(left, 1);
	t_tick next_decay = def.decay_every <= 0 ? 0 : now + (decay >= 0 ? decay : def.decay_every);

	on_unit.push_back({ def.index, source_id, static_cast<int32>(std::min<int64>(stacks, cap)), expires, next_decay });
}

/// `sd` has loaded the map after logging in: the statuses with persist = true
/// they had at their last logout come back.
void restore_persistent(map_session_data& sd) {
	bool any = false;
	std::vector<int32> restored;

	for (const std::string& mod : persist_mods) {
		mod_store::s_target t;
		std::string error;
		std::vector<std::pair<std::string, mod_store::s_value>> entries;

		if (!mod_store::target(mod, mod_store::SCOPE_CHAR, &sd, t, error) || !mod_store::read(t, PERSIST_PATH, entries, error))
			continue;
		for (const auto& [name, value] : entries) {
			any = true;

			s_lua_status* def = custom_status(name.c_str());

			// A status this mod no longer keeps is dropped at the logout.
			if (def == nullptr || !def->persist || def->mod != mod || !value.is_string)
				continue;
			for (size_t start = 0; start <= value.text.size();) {
				size_t end = value.text.find(';', start);
				std::string one = value.text.substr(start, end == std::string::npos ? std::string::npos : end - start);
				long long stacks, left, source, decay;

				if (sscanf(one.c_str(), "%lld,%lld,%lld,%lld", &stacks, &left, &source, &decay) == 4)
					restore_instance(sd, *def, stacks, left, source, decay);
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			if (std::find(restored.begin(), restored.end(), def->index) == restored.end())
				restored.push_back(def->index);
		}
	}
	persist_loaded[sd.id] = any;
	for (int32 index : restored) {
		show_icon(sd, *status_list[index]);
		recalc(sd, *status_list[index]);
	}
}

/// The players who have loaded the map since they logged in get their kept
/// statuses back.
void restore_logged_in() {
	std::vector<int32> due;
	s_mapiterator* iter = mapit_getallusers();

	for (block_list* bl = mapit_first(iter); mapit_exists(iter); bl = mapit_next(iter)) {
		const map_session_data* sd = BL_CAST(BL_PC, bl);

		if (sd != nullptr && sd->prev != nullptr && !sd->state.connect_new && persist_loaded.count(sd->id) == 0)
			due.push_back(sd->id);
	}
	mapit_free(iter);
	for (int32 id : due)
		if (map_session_data* sd = map_id2sd(id); sd != nullptr)
			restore_persistent(*sd);
}

/// mod_store::on_logout: `sd` leaves. Their statuses with persist = true go
/// into the char store of the mod that defined them; every mod status on
/// them ends with them.
void persist_on_logout(map_session_data& sd) {
	if (L == nullptr)
		return;

	auto loaded = persist_loaded.find(sd.id);

	// Before they were back on the player (a logout right after the login),
	// what is stored stays as it is.
	if (!persist_mods.empty() && loaded != persist_loaded.end()) {
		std::unordered_map<std::string, std::unordered_map<std::string, std::string>> kept;  // mod, status: instances

		if (auto it = statuses_on.find(sd.id); it != statuses_on.end()) {
			t_tick now = gettick();

			for (const s_lua_status_on& on : it->second) {
				const s_lua_status& def = *status_list[on.index];

				if (!def.persist)
					continue;

				// A player's id is their account's, the same after a restart;
				// any other unit's is not.
				int32 source = on.source_id != 0 && map_id2sd(on.source_id) != nullptr ? on.source_id : 0;
				std::string& text = kept[def.mod][def.name];

				if (!text.empty())
					text += ';';
				text += std::to_string(on.stacks) + ',' + std::to_string(on.expires == 0 ? -1 : std::max<int64>(1, DIFF_TICK(on.expires, now)))
					+ ',' + std::to_string(source) + ',' + std::to_string(on.next_decay == 0 ? -1 : std::max<int64>(0, DIFF_TICK(on.next_decay, now)));
			}
		}
		// Nothing to keep and nothing kept: the store is left alone.
		if (loaded->second || !kept.empty()) {
			for (const std::string& mod : persist_mods) {
				mod_store::s_target t;
				std::string error;
				int32 removed = 0;

				if (!mod_store::target(mod, mod_store::SCOPE_CHAR, &sd, t, error) || !mod_store::remove(t, PERSIST_PATH, removed, error))
					continue;
				if (auto m = kept.find(mod); m != kept.end()) {
					for (const auto& [name, text] : m->second) {
						mod_store::s_value value;

						value.is_string = true;
						value.text = text;
						mod_store::set(t, std::string(PERSIST_PATH) + "." + name, value, error);
					}
				}
			}
		}
	}
	persist_loaded.erase(sd.id);
	statuses_on.erase(sd.id);
}

/// Every SWEEP_INTERVAL: statuses run out, lose stacks to decay, or leave
/// with their unit (a player logging out, a monster gone from the server).
TIMER_FUNC(skill_lua_sweep) {
	struct s_ended {
		int32 unit_id;
		s_lua_status_on on;
		const char* reason;
	};
	std::vector<s_ended> ended;
	std::vector<std::pair<int32, int32>> changed;  // (unit, status index)

	if (!persist_mods.empty())
		restore_logged_in();

	for (auto it = statuses_on.begin(); it != statuses_on.end();) {
		if (map_id2bl(it->first) == nullptr) {
			it = statuses_on.erase(it);
			continue;
		}

		std::vector<s_lua_status_on>& on_unit = it->second;

		for (auto on = on_unit.begin(); on != on_unit.end();) {
			const s_lua_status& def = *status_list[on->index];
			const char* reason = nullptr;

			if (on->expires != 0 && DIFF_TICK(tick, on->expires) >= 0) {
				reason = "expired";
			} else if (def.decay_every > 0 && on->next_decay != 0 && DIFF_TICK(tick, on->next_decay) >= 0) {
				on->next_decay = tick + def.decay_every;
				on->stacks = std::max(0, on->stacks - def.decay_stacks);
				changed.emplace_back(it->first, on->index);
				if (on->stacks == 0)
					reason = "decayed";
			}
			if (reason == nullptr) {
				++on;
				continue;
			}
			ended.push_back({ it->first, *on, reason });
			changed.emplace_back(it->first, on->index);
			on = on_unit.erase(on);
		}
		it = on_unit.empty() ? statuses_on.erase(it) : std::next(it);
	}

	for (const s_ended& e : ended)
		if (block_list* bl = map_id2bl(e.unit_id); bl != nullptr)
			status_event(SHOOK_ON_END, *bl, *status_list[e.on.index], e.on, e.reason, true);

	std::sort(changed.begin(), changed.end());
	changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
	for (const auto& [unit_id, index] : changed) {
		if (block_list* bl = map_id2bl(unit_id); bl != nullptr) {
			show_icon(*bl, *status_list[index]);
			recalc(*bl, *status_list[index]);
		}
	}
	return 0;
}

/// Every TICK_INTERVAL: the learners' tick hooks, for each player online and
/// alive, then the tick hooks of the statuses on every living unit.
TIMER_FUNC(skill_lua_tick) {
	if (!learner_skills[HOOK_TICK].empty()) {
		std::vector<int32> players;
		s_mapiterator* iter = mapit_getallusers();

		for (block_list* bl = mapit_first(iter); mapit_exists(iter); bl = mapit_next(iter))
			players.push_back(bl->id);
		mapit_free(iter);

		for (int32 id : players) {
			map_session_data* sd = map_id2sd(id);

			if (sd == nullptr || sd->prev == nullptr || pc_isdead(sd))
				continue;

			std::unique_ptr<s_skill_lua_hit> hit = new_hit(sd, sd, 0);

			{
				acting_for acting(hit.get());

				for_each_learned(HOOK_TICK, *sd, [&](uint16 skill_id, uint16 lv, s_hook& hook, const char* name) {
					lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
					push_context(skill_id, lv, sd, sd, nullptr);
					call_owned(hook, name, sd);
				});
			}
			skill_lua_apply(hit);
		}
	}

	std::vector<std::pair<int32, s_lua_status_on>> due;

	for (const auto& it : statuses_on)
		for (const s_lua_status_on& on : it.second)
			if (!status_list[on.index]->hooks[SHOOK_TICK].empty())
				due.emplace_back(it.first, on);
	for (const auto& [unit_id, on] : due) {
		s_lua_status& def = *status_list[on.index];
		block_list* bl = map_id2bl(unit_id);

		// Still there: an earlier hook's actions may have ended it.
		if (bl != nullptr && !status_isdead(*bl) && status_stacks_on(unit_id, on.index, def.per_source ? on.source_id : 0) > 0)
			status_event(SHOOK_TICK, *bl, def, on, nullptr, true);
	}
	return 0;
}

}  // namespace

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
	lua_register(L, "status", lua_register_status);
	lua_register(L, "const", lua_constant);
	lua_register(L, "log", lua_log);
	lua_register(L, "print", lua_log);
	lua_register(L, "now", lua_now);
	lua_register(L, "skill_info", lua_skill_info);
	lua_register(L, "units_around", lua_units_around);
	lua_register(L, "units_in_line", lua_units_in_line);
	lua_register(L, "ground_units_around", lua_ground_units_around);
	lua_register(L, "battle_config", lua_battle_config);
	lua_register(L, "element_rate", lua_element_rate);
	lua_register(L, "size_rate", lua_size_rate);
	lua_register(L, "def_rate", lua_def_rate);
	register_store();
	register_unit_methods();
	register_context_methods();
	lua_sethook(L, count_hook, LUA_MASKCOUNT, 1000);

	current_mod = "server";
	budget = LOAD_BUDGET;
	next_load_order = 0;
	if (luaL_dostring(L, PRELUDE) != LUA_OK) {
		ShowError("Lua: prelude failed: %s\n", lua_tostring(L, -1));
		do_final_skill_lua();
		return;
	}

	size_t loaded = 0;

	loading = true;
	for (const auto& file : files)
		loaded += run_file(file.second, file.first) ? 1 : 0;
	loading = false;

	size_t skill_hooks_total = 0;

	for (const auto& it : hooks_by_name)
		for (int32 i = 0; i < HOOK_MAX; i++)
			skill_hooks_total += it.second.hooks[i].size();

	size_t item_hooks_total = 0;

	for (const auto& it : item_hooks_by_name)
		for (int32 i = 0; i < ITEM_HOOK_MAX; i++)
			item_hooks_total += it.second.hooks[i].size();
	ShowStatus("Lua: loaded %zu of %zu file(s), %zu skill hook(s) across %zu skill(s), %zu item hook(s) across %zu item(s), %zu status(es) of mods' own and hooks on %zu of the server's.\n",
		loaded, files.size(), skill_hooks_total, hooks_by_name.size(), item_hooks_total, item_hooks_by_name.size(), status_list.size(), stock_status_hooks.size());
	skill_lua_attach();

	for (const s_lua_status* def : status_list)
		if (def->persist && std::find(persist_mods.begin(), persist_mods.end(), def->mod) == persist_mods.end())
			persist_mods.push_back(def->mod);
	// A player's mod statuses end when they leave; those with persist = true
	// are kept in the mod store first.
	if (!status_list.empty())
		mod_store::on_logout(persist_on_logout);

	add_timer_func_list(skill_lua_deferred, "skill_lua_deferred");
	add_timer_func_list(skill_lua_sweep, "skill_lua_sweep");
	add_timer_func_list(skill_lua_tick, "skill_lua_tick");
	if (!status_list.empty())
		sweep_timer = add_timer_interval(gettick() + SWEEP_INTERVAL, skill_lua_sweep, 0, 0, SWEEP_INTERVAL);
	if (!status_list.empty() || !learner_skills[HOOK_TICK].empty())
		tick_timer = add_timer_interval(gettick() + TICK_INTERVAL, skill_lua_tick, 0, 0, TICK_INTERVAL);
}

void do_final_skill_lua() {
	if (sweep_timer != INVALID_TIMER)
		delete_timer(sweep_timer, skill_lua_sweep);
	if (tick_timer != INVALID_TIMER)
		delete_timer(tick_timer, skill_lua_tick);
	if (deferred_timer != INVALID_TIMER)
		delete_timer(deferred_timer, skill_lua_deferred);
	sweep_timer = tick_timer = deferred_timer = INVALID_TIMER;
	deferred.clear();
	unit_power.clear();
	persist_mods.clear();
	persist_loaded.clear();
	statuses_on.clear();
	status_list.clear();
	stock_status_hooks.clear();
	statuses_by_name.clear();
	for (std::vector<uint16>& skills : learner_skills)
		skills.clear();
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
	for (std::vector<uint16>& skills : learner_skills)
		skills.clear();
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
		e_skill id = static_cast<e_skill>(skill->nameid);

		for (e_hook which : LEARNER_HOOKS)
			if (!h.hooks[which].empty())
				learner_skills[which].push_back(skill->nameid);

		if (dynamic_cast<const LuaSkillImpl*>(skill->impl.get()) != nullptr)
			continue;  // wrapped already
		if (h.replace || (h.custom && skill->impl == nullptr)) {
			// A mod's own skill, or one it reworks: the generic class does
			// what the skill_db entry says, under the mod's hooks.
			skill->impl = std::make_unique<LuaSkillImpl>(id, std::make_unique<LuaGenericSkillImpl>(id));
			continue;
		}
		if (h.custom)
			ShowWarning("Lua: %s has a skill class of its own in this server, so custom = true leaves it as it is (replace = true would drop it).\n", skill->name);

		bool wants_class = !h.hooks[HOOK_RATIO].empty() || !h.hooks[HOOK_HIT].empty() || !h.hooks[HOOK_ELEMENT].empty()
			|| !h.hooks[HOOK_HITS].empty() || !h.hooks[HOOK_BEFORE_CAST].empty();

		if (!wants_class)
			continue;
		if (skill->impl == nullptr) {
			// Its formula is still in battle.cpp's switch, which has no hook.
			ShowWarning("Lua: %s has no skill class in this server, so its ratio, hit, element, hits and before_cast hooks cannot run (on_hit and on_cast still do; custom = true gives a skill of a mod's own one).\n", skill->name);
			continue;
		}
		skill->impl = std::make_unique<LuaSkillImpl>(id, std::move(skill->impl));
	}

	for (const auto& it : hooks_by_name)
		if (!found.count(it.first))
			ShowWarning("Lua: there is no skill called %s (use the AegisName, like MG_FIREBOLT; a skill of a mod's own goes in db/import/skill_db.yml).\n", it.first.c_str());

	// Resolve each registered item AegisName to its nameid. An item that
	// does not exist is named loudly rather than silently ignored.
	item_hooks_by_id.clear();
	for (auto& it : item_hooks_by_name) {
		std::shared_ptr<item_data> id = item_db.search_aegisname(it.first.c_str());

		if (id == nullptr) {
			ShowWarning("Lua: there is no item called %s (use the AegisName, like VORPAL_BLADE).\n", it.first.c_str());
			continue;
		}
		item_hooks_by_id[id->nameid] = &it.second;
	}
}

namespace {

/// Iterate a unit's equipped items (0 if not a player), and the cards slotted
/// in them, looking up each nameid in the item-hook table. Fire `which` on
/// each match, in priority order within each item. One nameid fires at most
/// once per attack, even if the wearer has several copies equipped or
/// slotted (the same accessory twice, four of one card in a weapon).
void fire_item_hooks(e_item_hook which, const block_list* bl, uint16 skill_id, uint16 skill_lv, const block_list* src, const block_list* target, const s_damage_ctx& dmg) {
	const map_session_data* sd = bl != nullptr ? BL_CAST(BL_PC, bl) : nullptr;

	if (sd == nullptr || item_hooks_by_id.empty())
		return;

	constexpr equip_index slots[] = {
		EQI_HAND_R, EQI_HAND_L, EQI_ARMOR, EQI_SHOES, EQI_GARMENT,
		EQI_HEAD_TOP, EQI_HEAD_MID, EQI_HEAD_LOW, EQI_ACC_L, EQI_ACC_R,
	};
	t_itemid fired[sizeof(slots) / sizeof(slots[0]) * (1 + MAX_SLOTS)] = {};
	size_t fired_n = 0;

	auto fire = [&](t_itemid nameid) {
		if (nameid == 0)
			return;
		for (size_t i = 0; i < fired_n; i++)
			if (fired[i] == nameid)
				return;

		s_item_hooks* hooks = item_hooks_for(nameid);
		if (hooks == nullptr || hooks->hooks[which].empty())
			return;

		fired[fired_n++] = nameid;
		for (s_hook& hook : hooks->hooks[which]) {
			if (hook.ref == LUA_NOREF)
				continue;
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, skill_lv, src, target, &dmg);
			call_owned(hook, hooks->aegis.c_str(), bl);
		}
	};

	for (equip_index e : slots) {
		int16 idx = sd->equip_index[e];
		if (idx < 0)
			continue;
		const item& equipped = sd->inventory.u.items_inventory[idx];
		fire(equipped.nameid);
		// card[0] of a forged or named item holds a marker, not a card.
		if (!itemdb_isspecial(equipped.card[0]))
			for (int32 c = 0; c < MAX_SLOTS; c++)
				fire(equipped.card[c]);
	}
}

}  // namespace

std::unique_ptr<s_skill_lua_hit> skill_lua_on_damage(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, int64 damage, int32 attack_type, int32 dmg_lv, bool critical, int32 element) {
	// Shared pending hit, reused by any skill()/item()/status() hook that
	// fires. A re-entry (a hook that somehow triggers another attack) is
	// refused to keep the shared actions list unambiguous.
	if (L == nullptr || src == nullptr || target == nullptr || current_hit != nullptr || in_lua_cast)
		return nullptr;

	s_skill_hooks* skill_hooks = skill_id != 0 ? hooks_for(skill_id) : nullptr;
	map_session_data* ssd = BL_CAST(BL_PC, src);
	map_session_data* tsd = BL_CAST(BL_PC, target);
	bool has_skill_hooks = skill_hooks != nullptr && !skill_hooks->hooks[HOOK_ON_HIT].empty();
	bool has_item_hooks = !item_hooks_by_id.empty() && (src->type == BL_PC || target->type == BL_PC);
	bool has_learner_hooks = (ssd != nullptr && !learner_skills[HOOK_ON_ATTACK].empty()) || (tsd != nullptr && !learner_skills[HOOK_ON_HIT_TAKEN].empty());
	bool has_status_hooks = has_status_hook(src->id, SHOOK_ON_ATTACK) || has_status_hook(target->id, SHOOK_ON_HIT_TAKEN);

	if (!has_skill_hooks && !has_item_hooks && !has_learner_hooks && !has_status_hooks)
		return nullptr;

	auto hit = std::make_unique<s_skill_lua_hit>();
	const status_data* tstatus = status_get_status_data(*target);

	hit->src_id = src->id;
	hit->target_id = target->id;
	hit->skill_id = skill_id;
	hit->damage = damage;
	hit->race = tstatus->race;
	hit->class_ = tstatus->class_;

	s_damage_ctx dmg = { damage, dmg_lv, element, attack_type, critical, skill_id, skill_lv };
	acting_for acting(hit.get());

	// Order: skill on_hit first (same point as before this feature), then
	// attacker's item on_attack, then defender's item on_hit_taken, then the
	// passives' on_attack and on_hit_taken, then the statuses'. Each stage
	// runs every registered mod in priority order.
	if (has_skill_hooks) {
		for (s_hook& hook : skill_hooks->hooks[HOOK_ON_HIT]) {
			if (hook.ref == LUA_NOREF)
				continue;
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, skill_lv, src, target, &dmg);
			call_owned(hook, skill_hooks->skill.c_str(), src);
		}
	}
	fire_item_hooks(ITEM_HOOK_ON_ATTACK, src, skill_id, skill_lv, src, target, dmg);
	fire_item_hooks(ITEM_HOOK_ON_HIT_TAKEN, target, skill_id, skill_lv, src, target, dmg);

	if (ssd != nullptr) {
		for_each_learned(HOOK_ON_ATTACK, *ssd, [&](uint16 passive, uint16 lv, s_hook& hook, const char* name) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(passive, lv, src, target, &dmg);
			call_owned(hook, name, src);
		});
	}
	if (tsd != nullptr) {
		for_each_learned(HOOK_ON_HIT_TAKEN, *tsd, [&](uint16 passive, uint16 lv, s_hook& hook, const char* name) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(passive, lv, src, target, &dmg);
			call_owned(hook, name, target);
		});
	}
	for_each_status(SHOOK_ON_ATTACK, *src, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, skill_lv, src, target, &dmg);
		add_status_fields(def, on, *src);
		call_owned(hook, def.name.c_str(), src);
	});
	for_each_status(SHOOK_ON_HIT_TAKEN, *target, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, skill_lv, src, target, &dmg);
		add_status_fields(def, on, *target);
		call_owned(hook, def.name.c_str(), target);
	});

	if (hit->actions.empty())
		return nullptr;
	return hit;
}

namespace {

/// The % of damage a c:cast or c:attack deals while it runs (100: as is):
/// that skill's damage by that unit only, not a counter or an autospell it
/// sets off.
struct s_lua_power {
	int32 percent = 100;
	int32 src_id = 0;
	uint16 skill_id = 0;
} lua_power;

/// While it lives, damage `src` deals with `skill_id` is `percent` %.
class with_power {
	s_lua_power previous_;

public:
	with_power(int32 percent, const block_list& src, uint16 skill_id) : previous_(lua_power) {
		lua_power = { percent, src.id, skill_id };
	}
	~with_power() { lua_power = previous_; }
	with_power(const with_power&) = delete;
	with_power& operator=(const with_power&) = delete;
};

/// The % of its damage a hit of `skill_id` by `src` deals: a running c:cast
/// or c:attack's power, or that of the ground unit dealing it.
int32 power_of(const block_list& src, uint16 skill_id) {
	if (lua_power.percent != 100 && src.id == lua_power.src_id && skill_id == lua_power.skill_id)
		return lua_power.percent;
	if (dealt_by == nullptr || dealt_by->type != BL_SKILL || unit_power.empty())
		return 100;

	const s_skill_unit_group* group = static_cast<const skill_unit*>(dealt_by)->group.get();

	if (group == nullptr || group->skill_id != skill_id || group->src_id != src.id)
		return 100;

	auto it = unit_power.find(group->group_id);

	return it != unit_power.end() && it->second.created == group->tick ? it->second.percent : 100;
}

/// The ids of the ground skill groups `bl` owns.
std::vector<int32> group_ids(block_list& bl) {
	std::vector<int32> ids;

	if (unit_data* ud = unit_bl2ud(&bl); ud != nullptr)
		for (const std::shared_ptr<s_skill_unit_group>& group : ud->skillunits)
			if (group != nullptr)
				ids.push_back(group->group_id);
	return ids;
}

/// The groups of `skill_id` that `bl` owns now and did not `before` deal
/// their later hits at `percent` %. Groups gone since are forgotten.
void keep_power(block_list& bl, uint16 skill_id, int32 percent, const std::vector<int32>& before) {
	for (auto it = unit_power.begin(); it != unit_power.end();) {
		std::shared_ptr<s_skill_unit_group> group = skill_id2group(it->first);

		it = group == nullptr || group->tick != it->second.created ? unit_power.erase(it) : std::next(it);
	}

	unit_data* ud = unit_bl2ud(&bl);

	if (ud == nullptr)
		return;
	for (const std::shared_ptr<s_skill_unit_group>& group : ud->skillunits)
		if (group != nullptr && group->skill_id == skill_id && std::find(before.begin(), before.end(), group->group_id) == before.end())
			unit_power[group->group_id] = { percent, group->tick };
}

}  // namespace

skill_lua_dealt_by::skill_lua_dealt_by(block_list* dsrc) : previous_(dealt_by) {
	dealt_by = dsrc;
}

skill_lua_dealt_by::~skill_lua_dealt_by() {
	dealt_by = previous_;
}

/// Cast a skill the way bAutoSpell does (skill_additional_effect): the same
/// "can this be cast here" check, ground-skill limit, cost (its HP, spheres
/// and zeny; no SP, no items) and after-cast delay. No hook that can act runs
/// for it (in_lua_cast). A ground skill goes to (x, y) when given, else to
/// where `target` stands.
void cast_like_autospell(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, int16 x, int16 y) {
	map_session_data* sd = BL_CAST(BL_PC, src);
	t_tick tick = gettick();

	if (x < 0 || y < 0) {
		x = target->x;
		y = target->y;
	}

	if (sd != nullptr) {
		sd->state.autocast = 1;
		bool refused = skill_isNotOk(skill_id, *sd);
		sd->state.autocast = 0;
		if (refused)
			return;
	}

	e_cast_type type = skill_get_casttype(skill_id);

	if (type == CAST_GROUND && !skill_pos_maxcount_check(src, x, y, skill_id, skill_lv, BL_PC, false))
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
			skill_castend_pos2(src, x, y, skill_id, skill_lv, tick, 0);
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

namespace {

const char* drop_type_name(int32 type) {
	switch (type) {
		case IT_CARD: return "card";
		case IT_WEAPON: return "weapon";
		case IT_ARMOR: return "armor";
		default: return "etc";
	}
}

/// Build c.drops: an array of the mob's stealable drops, in the mob db's
/// own slot order. Each entry carries nameid, rate (out of 10000, after the
/// server multiplier) and the item's type as both booleans (is_card,
/// is_equip, is_etc) and a short string ("card", "weapon", "armor", "etc"),
/// so a hook can filter without consulting the item database itself.
void push_drops(const mob_data& md) {
	int32 drop_i = 0;

	lua_createtable(L, (int32)md.db->dropitem.size(), 0);
	for (const std::shared_ptr<s_mob_drop>& entry : md.db->dropitem) {
		if (entry->steal_protected)
			continue;
		if (!item_db.exists(entry->nameid))
			continue;

		std::shared_ptr<item_data> id = item_db.find(entry->nameid);
		int32 type = id != nullptr ? id->type : -1;

		lua_createtable(L, 0, 6);
		set_int("nameid", entry->nameid);
		set_int("rate", entry->rate);
		lua_pushboolean(L, type == IT_CARD);
		lua_setfield(L, -2, "is_card");
		lua_pushboolean(L, type == IT_WEAPON || type == IT_ARMOR);
		lua_setfield(L, -2, "is_equip");
		lua_pushboolean(L, type != IT_CARD && type != IT_WEAPON && type != IT_ARMOR);
		lua_setfield(L, -2, "is_etc");
		lua_pushstring(L, drop_type_name(type));
		lua_setfield(L, -2, "type");

		lua_rawseti(L, -2, ++drop_i);
	}
}

/// Pull a nameid out of what an on_steal hook returned. The hook may return
/// a drop table (one from c.drops, or any table with a `nameid`), the
/// nameid as an integer, or nil to pass. Returns 0 for nil or an
/// unrecognised shape (and logs the shape).
t_itemid read_chosen_drop(const s_hook& hook) {
	if (lua_isinteger(L, -1))
		return static_cast<t_itemid>(lua_tointeger(L, -1));

	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "nameid");
		t_itemid id = lua_isinteger(L, -1) ? static_cast<t_itemid>(lua_tointeger(L, -1)) : 0;
		lua_pop(L, 1);
		return id;
	}

	if (!lua_isnil(L, -1))
		ShowError("Lua: %s's on_steal hook returned a %s, not a drop, nameid or nil; falling through.\n", hook.mod.c_str(), luaL_typename(L, -1));
	return 0;
}

/// True if `nameid` is a stealable drop (`steal_protected == false` and
/// known to item_db) on this mob, so the hook cannot force an item the
/// server would not otherwise give.
bool is_stealable(const mob_data& md, t_itemid nameid) {
	for (const std::shared_ptr<s_mob_drop>& entry : md.db->dropitem) {
		if (entry->nameid != nameid)
			continue;
		if (entry->steal_protected)
			continue;
		if (!item_db.exists(entry->nameid))
			continue;
		return true;
	}
	return false;
}

} // namespace

int32 skill_lua_level(uint16 skill_id, const block_list* bl, int32 stock) {
	return call_level_hook(HOOK_LEVEL, skill_id, bl, stock);
}

int32 skill_lua_job_level(uint16 skill_id, const block_list* bl, int32 stock) {
	return call_level_hook(HOOK_JOB_LEVEL, skill_id, bl, stock);
}

int32 skill_lua_heal(uint16 skill_id, uint16 skill_lv, const block_list* src, const block_list* target, int32 stock, int32 base) {
	s_skill_hooks* hooks = hooks_for(skill_id);

	// Not for a heal a hook's c:cast does: a heal hook may act, and one that
	// casts a heal would set itself off again.
	if (hooks == nullptr || hooks->hooks[HOOK_HEAL].empty() || current_hit != nullptr || in_lua_cast)
		return stock;

	int64 value = stock;
	const int64 before_bonus = base;
	std::unique_ptr<s_skill_lua_hit> hit = new_hit(src, target, skill_id);

	{
		// A heal hook may act (a shield from what overflows, a charge for
		// healing done); that happens a moment later, once the heal has.
		acting_for acting(hit.get());

		call_number_hook(HOOK_HEAL, skill_id, skill_lv, src, target, value, &before_bonus);
	}
	defer(std::move(hit));

	if (value == stock)
		return stock;  // no hook, or none changed it: the stock amount, untouched
	return static_cast<int32>(std::clamp<int64>(value, 0, INT32_MAX));
}

t_itemid skill_lua_on_steal(block_list* src, block_list* target, uint16 skill_lv) {
	// in_lua_cast refuses re-entry the same way on_damage does: an item
	// whose on_attack cast of TF_STEAL otherwise reruns the on_steal hook
	// against the Lua state the outer call still owns.
	if (L == nullptr || src == nullptr || target == nullptr || target->type != BL_MOB || in_lua_cast)
		return 0;

	s_skill_hooks* skill_hooks = hooks_for(TF_STEAL);

	if (skill_hooks == nullptr || skill_hooks->hooks[HOOK_ON_STEAL].empty())
		return 0;

	const mob_data& md = *reinterpret_cast<mob_data*>(target);
	std::unique_ptr<s_skill_lua_hit> hit = new_hit(src, target, TF_STEAL);
	// An on_steal hook may act (a buff for a steal that worked); that
	// happens a moment later.
	acting_for acting(hit.get());
	struct defer_on_exit {
		std::unique_ptr<s_skill_lua_hit>& hit;
		~defer_on_exit() { defer(std::move(hit)); }
	} later{ hit };

	for (s_hook& hook : skill_hooks->hooks[HOOK_ON_STEAL]) {
		if (hook.ref == LUA_NOREF)
			continue;

		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);

		lua_createtable(L, 0, 7);
		lua_pushstring(L, skill_get_name(TF_STEAL));
		lua_setfield(L, -2, "skill");
		set_int("skill_id", TF_STEAL);
		set_int("skill_lv", skill_lv);
		push_unit(src);
		lua_setfield(L, -2, "caster");
		push_unit(target);
		lua_setfield(L, -2, "target");
		lua_rawgeti(L, LUA_REGISTRYINDEX, context_meta_ref);
		lua_setmetatable(L, -2);
		push_drops(md);
		lua_setfield(L, -2, "drops");

		if (!call_owned(hook, skill_hooks->skill.c_str(), src, 1, 1))
			continue;

		t_itemid chosen = read_chosen_drop(hook);

		lua_pop(L, 1);

		if (chosen == 0)
			continue;  // next hook in the chain

		if (!is_stealable(md, chosen)) {
			ShowWarning("Lua: %s's on_steal hook returned nameid %u, which is not a stealable drop on %s; falling through.\n", hook.mod.c_str(), chosen, md.db->name.c_str());
			continue;
		}
		return chosen;
	}

	return 0;
}

namespace {

/// The most spirit spheres `sd` may hold, as the server's own skills count
/// them: 5, and more under Raising Dragon.
int32 sphere_limit(map_session_data& sd) {
	int32 limit = 5;

	if (const status_change_entry* sce = sd.sc.getSCE(SC_RAISINGDRAGON); sce != nullptr)
		limit += sce->val1;
	return std::min(limit, MAX_SPIRITBALL);
}

/// Put a skill into a player's Plagiarism (slot 1) or Reproduce (slot 2)
/// slot, as skill_do_copy does once it has decided to copy.
void copy_skill(map_session_data& sd, uint16 skill_id, uint8 lv, int32 slot) {
	uint16 idx = skill_get_index(skill_id);

	if (idx == 0)
		return;

	if (sd.status.skill[idx].id != 0 && sd.status.skill[idx].flag != SKILL_FLAG_PLAGIARIZED)
		return;  // a skill the player has of their own

	uint16& mine = slot == 2 ? sd.reproduceskill_idx : sd.cloneskill_idx;
	uint16& other = slot == 2 ? sd.cloneskill_idx : sd.reproduceskill_idx;

	// The same skill in the other slot goes, as the server's own copy does.
	if (other != 0 && sd.status.skill[other].flag == SKILL_FLAG_PLAGIARIZED && sd.status.skill[other].id == skill_id)
		pc_skill_plagiarism_reset(sd, slot == 2 ? 1 : 2);
	pc_skill_plagiarism_reset(sd, static_cast<uint8>(slot));

	mine = idx;
	pc_setglobalreg(&sd, add_str(slot == 2 ? SKILL_VAR_REPRODUCE : SKILL_VAR_PLAGIARISM), skill_id);
	pc_setglobalreg(&sd, add_str(slot == 2 ? SKILL_VAR_REPRODUCE_LV : SKILL_VAR_PLAGIARISM_LV), lv);
	sd.status.skill[idx].id = skill_id;
	sd.status.skill[idx].lv = lv;
	sd.status.skill[idx].flag = SKILL_FLAG_PLAGIARIZED;
	clif_addskill(sd, skill_id);
}

/// What c:mount() changes: the option bits.
int32 mount_option(int32 kind) {
	switch (kind) {
		case SKILL_LUA_MOUNT_PECO: return OPTION_RIDING;
		case SKILL_LUA_MOUNT_DRAGON: return OPTION_DRAGON1;
		case SKILL_LUA_MOUNT_WARG: return OPTION_WUG | OPTION_WUGRIDER;
		case SKILL_LUA_MOUNT_MADO: return OPTION_MADOGEAR;
		case SKILL_LUA_MOUNT_FALCON: return OPTION_FALCON;
		default: return 0;
	}
}

void mount(map_session_data& sd, int32 kind, bool on) {
	if (kind == SKILL_LUA_MOUNT_CART) {
		pc_setcart(&sd, on ? 1 : 0);
		return;
	}

	int32 bits = mount_option(kind);
	int32 option = static_cast<int32>(sd.sc.option);

	if (on)
		option |= bits;
	else if (kind == SKILL_LUA_MOUNT_DRAGON)
		option &= ~OPTION_DRAGON;  // whatever its colour
	else if (kind == SKILL_LUA_MOUNT_WARG)
		option &= ~OPTION_WUGRIDER;  // the warg stays, unridden
	else
		option &= ~bits;
	if (option != static_cast<int32>(sd.sc.option))
		pc_setoption(&sd, option);
}

/// Summon `amount` of `mob_id` for `owner`, as the summon script command does;
/// or, `neutral`, monsters on nobody's side that leave when their time is up.
void summon(block_list& owner, int32 mob_id, int32 amount, int64 duration, int16 x, int16 y, bool neutral) {
	if (x < 0 || y < 0) {
		x = owner.x;
		y = owner.y;
	}

	t_tick tick = gettick();

	for (int32 i = 0; i < amount; i++) {
		mob_data* md = mob_once_spawn_sub(&owner, owner.m, x, y, "--ja--", mob_id, "", SZ_SMALL, AI_NONE);

		if (md == nullptr)
			return;
		if (!neutral) {
			md->master_id = owner.id;
			md->special_state.ai = AI_ATTACK;
		}
		if (md->deletetimer != INVALID_TIMER)
			delete_timer(md->deletetimer, mob_timer_delete);
		md->deletetimer = add_timer(tick + duration, mob_timer_delete, md->id, 0);
		mob_spawn(md);
		clif_specialeffect(md, EF_ENTRY2, AREA);
		if (!neutral)
			sc_start4(nullptr, md, SC_MODECHANGE, 100, 1, 0, MD_AGGRESSIVE, 0, duration);
	}
}

/// The players c:itemskill offered a skill to. The server tells a skill that
/// comes from no item by abra_flag, as Hocus-pocus's does; if the player uses
/// an item instead, the flag is cleared again (skill_lua_use_item), so that
/// the item's own skill is paid for.
std::unordered_set<int32> itemskill_offered;

/// Take `amount` of `nameid` from a player: all of it, or nothing.
void take_item(map_session_data& sd, t_itemid nameid, int32 amount) {
	int32 have = 0;

	for (int32 i = 0; i < MAX_INVENTORY; i++)
		if (sd.inventory.u.items_inventory[i].nameid == nameid)
			have += sd.inventory.u.items_inventory[i].amount;
	if (have < amount)
		return;
	for (int32 i = 0; i < MAX_INVENTORY && amount > 0; i++) {
		if (sd.inventory.u.items_inventory[i].nameid != nameid)
			continue;

		int32 n = std::min<int32>(amount, sd.inventory.u.items_inventory[i].amount);

		pc_delitem(&sd, i, n, 0, 0, LOG_TYPE_SCRIPT);
		amount -= n;
	}
}

}  // namespace

void skill_lua_apply(std::unique_ptr<s_skill_lua_hit>& hit) {
	if (hit == nullptr)
		return;

	// An action can kill a unit (c:hurt, c:cast). Some callers -- the
	// deferred actions, the sweep, the tick -- hold no lock of their own, so a
	// monster freed on the spot must not be freed under the next action: each
	// one looks its units up again, and freed memory waits for the lock.
	FreeBlockLock freeLock;

	for (const s_skill_lua_action& action : hit->actions) {
		block_list* src = map_id2bl(hit->src_id);
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
					status_heal(bl, action.hp, action.sp, 0, action.flag != 0 ? 1 : 0);
				break;
			case SKILL_LUA_STATUS:
				// status_change_start takes the chance out of 10000, as c:status does.
				if (bl != nullptr && !status_isdead(*bl))
					status_change_start(src != nullptr ? src : bl, bl, static_cast<sc_type>(action.type), action.rate, action.val1, action.val2, action.val3, action.val4, static_cast<t_tick>(action.duration), SCSTART_NONE);
				break;
			case SKILL_LUA_CAST: {
				block_list* caster = action.by_id != 0 ? map_id2bl(action.by_id) : src;

				if (caster != nullptr && bl != nullptr && !status_isdead(*caster) && !status_isdead(*bl) && caster->m == bl->m) {
					with_power power(action.power, *caster, static_cast<uint16>(action.type));
					std::vector<int32> before = action.power != 100 ? group_ids(*caster) : std::vector<int32>();

					cast_like_autospell(caster, bl, static_cast<uint16>(action.type), static_cast<uint16>(action.val1), action.x, action.y);
					// What it placed on the ground deals its later hits at the same power.
					if (action.power != 100)
						keep_power(*caster, static_cast<uint16>(action.type), action.power, before);
				}
				break;
			}
			case SKILL_LUA_ATTACK: {
				block_list* attacker = action.by_id != 0 ? map_id2bl(action.by_id) : src;

				if (attacker != nullptr && bl != nullptr && !status_isdead(*attacker) && !status_isdead(*bl) && attacker->m == bl->m
					&& battle_check_target(attacker, bl, BCT_ENEMY) > 0) {
					with_power power(action.power, *attacker, static_cast<uint16>(action.type));
					bool was = in_lua_cast;

					uint16 skill_id = static_cast<uint16>(action.type);
					// A Splash Split skill divides its damage by the count of
					// targets the flag carries: this hit is on one.
					int32 flag = skill_get_nk(skill_id, NK_SPLASHSPLIT) ? 1 : 0;

					in_lua_cast = true;  // no hook that can act runs for it: no hit triggers itself
					skill_attack(skill_get_type(skill_id), attacker, attacker, bl, skill_id, static_cast<uint16>(action.val1), gettick(), flag);
					in_lua_cast = was;
				}
				break;
			}
			case SKILL_LUA_END_STATUS:
				if (bl != nullptr)
					status_change_end(bl, static_cast<sc_type>(action.type));
				break;
			case SKILL_LUA_STATUS_VAL:
				if (status_change* sc = bl != nullptr ? status_get_sc(bl) : nullptr; sc != nullptr) {
					if (status_change_entry* sce = sc->getSCE(static_cast<sc_type>(action.type)); sce != nullptr) {
						int32* vals[] = { &sce->val1, &sce->val2, &sce->val3, &sce->val4 };

						*vals[action.rate - 1] = action.val1;
						if (const auto& flags = status_db.getCalcFlag(static_cast<sc_type>(action.type)); flags.any())
							status_calc_bl_(bl, flags);
					}
				}
				break;
			case SKILL_LUA_SPHERES:
				if (map_session_data* sd = bl != nullptr ? BL_CAST(BL_PC, bl) : nullptr; sd != nullptr && !pc_isdead(sd)) {
					if (action.val1 < 0) {
						pc_delspiritball(sd, -action.val1, 0);
					} else {
						int32 limit = action.flag > 0 ? action.flag : sphere_limit(*sd);

						for (int32 i = 0; i < action.val1 && sd->spiritball < limit; i++)
							pc_addspiritball(sd, static_cast<int32>(action.duration), limit);
					}
				}
				break;
			case SKILL_LUA_SLIDE:
				if (bl != nullptr && !status_isdead(*bl) && unit_movepos(bl, action.x, action.y, 2, true))
					clif_blown(bl);
				break;
			case SKILL_LUA_KNOCKBACK:
				if (bl != nullptr && !status_isdead(*bl)) {
					block_list* from = action.by_id != 0 ? map_id2bl(action.by_id) : src;
					int16 fx = action.x >= 0 ? action.x : (from != nullptr ? from->x : bl->x);
					int16 fy = action.y >= 0 ? action.y : (from != nullptr ? from->y : bl->y);

					if (fx != bl->x || fy != bl->y)
						skill_blown(from != nullptr ? from : bl, bl, static_cast<char>(action.val1), static_cast<int8>(map_calc_dir(bl, fx, fy)), BLOWN_NONE);
				}
				break;
			case SKILL_LUA_INTERRUPT:
				if (bl != nullptr)
					unit_skillcastcancel(bl, 0);
				break;
			case SKILL_LUA_SUMMON:
				if (bl != nullptr && !status_isdead(*bl))
					summon(*bl, action.type, action.val1, action.duration, action.x, action.y, action.flag != 0);
				break;
			case SKILL_LUA_SET_TARGET:
				if (mob_data* md = bl != nullptr ? BL_CAST(BL_MOB, bl) : nullptr; md != nullptr && !status_isdead(*bl)) {
					if (block_list* target = map_id2bl(action.by_id); target != nullptr && target->m == bl->m && !status_isdead(*target))
						mob_target(md, target, AREA_SIZE);
				}
				break;
			case SKILL_LUA_TAKE_ITEM:
				if (map_session_data* sd = bl != nullptr ? BL_CAST(BL_PC, bl) : nullptr; sd != nullptr)
					take_item(*sd, static_cast<t_itemid>(action.type), action.val1);
				break;
			case SKILL_LUA_COPY_SKILL:
				if (map_session_data* sd = bl != nullptr ? BL_CAST(BL_PC, bl) : nullptr; sd != nullptr)
					copy_skill(*sd, static_cast<uint16>(action.type), static_cast<uint8>(action.val1), action.flag);
				break;
			case SKILL_LUA_ITEMSKILL:
				if (map_session_data* sd = bl != nullptr ? BL_CAST(BL_PC, bl) : nullptr; sd != nullptr && sd->ud.skilltimer == INVALID_TIMER && !pc_isdead(sd)) {
					sd->skillitem = static_cast<uint16>(action.type);
					sd->skillitemlv = static_cast<uint16>(action.val1);
					sd->skillitem_keep_requirement = false;
					sd->state.abra_flag = 1;  // no item to use up when it is cast
					itemskill_offered.insert(sd->id);
					clif_item_skill(sd, sd->skillitem, sd->skillitemlv);
				}
				break;
			case SKILL_LUA_SPEND:
				if (bl != nullptr && !status_isdead(*bl)) {
					const status_data* st = status_get_status_data(*bl);
					int64 hp = std::min<int64>(action.hp, st->hp > 0 ? st->hp - 1 : 0);
					int64 sp = std::min<int64>(action.sp, st->sp);

					if (hp > 0 || sp > 0)
						status_zap(bl, hp, sp);
				}
				break;
			case SKILL_LUA_PLACE_UNIT:
				if (bl != nullptr && !status_isdead(*bl)) {
					std::vector<int32> before = action.power != 100 ? group_ids(*bl) : std::vector<int32>();
					std::shared_ptr<s_skill_unit_group> group = skill_unitsetting(bl, static_cast<uint16>(action.type), static_cast<uint16>(action.val1), action.x, action.y, 0);

					if (group != nullptr && action.duration > 0)
						group->limit = static_cast<t_tick>(action.duration);
					if (group != nullptr && action.power != 100)
						keep_power(*bl, static_cast<uint16>(action.type), action.power, before);
				}
				break;
			case SKILL_LUA_END_UNIT:
				if (std::shared_ptr<s_skill_unit_group> group = skill_id2group(action.type); group != nullptr)
					skill_delunitgroup(group);
				break;
			case SKILL_LUA_EXPIRE:
				// Only what was summoned to leave: a monster of the map's own
				// would vanish without its drops.
				if (mob_data* md = bl != nullptr ? BL_CAST(BL_MOB, bl) : nullptr; md != nullptr && !status_isdead(*bl) && (md->master_id != 0 || md->deletetimer != INVALID_TIMER)) {
					if (md->deletetimer != INVALID_TIMER)
						delete_timer(md->deletetimer, mob_timer_delete);
					md->deletetimer = add_timer(gettick() + action.duration, mob_timer_delete, md->id, 0);
				}
				break;
			case SKILL_LUA_PRODUCE_MENU:
				if (map_session_data* sd = bl != nullptr ? BL_CAST(BL_PC, bl) : nullptr; sd != nullptr && !pc_isdead(sd))
					clif_skill_produce_mix_list(*sd, -1, action.val1);
				break;
			case SKILL_LUA_MOUNT:
				if (map_session_data* sd = bl != nullptr ? BL_CAST(BL_PC, bl) : nullptr; sd != nullptr && !pc_isdead(sd))
					mount(*sd, action.type, action.flag != 0);
				break;
			case SKILL_LUA_TELEPORT:
				if (bl != nullptr && !status_isdead(*bl) && bl->m >= 0 && !map_getmapflag(bl->m, MF_NOTELEPORT)) {
					if (map_session_data* sd = BL_CAST(BL_PC, bl); sd != nullptr)
						pc_randomwarp(sd, CLR_TELEPORT);
					else
						unit_warp(bl, bl->m, -1, -1, CLR_TELEPORT);
				}
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
			case SKILL_LUA_APPLY_STATUS:
				if (bl != nullptr && action.type >= 0 && action.type < static_cast<int32>(status_list.size()))
					apply_custom_status(*bl, *status_list[action.type], action.val1, action.duration, action.source_id);
				break;
			case SKILL_LUA_REMOVE_STATUS:
				if (bl != nullptr && action.type >= 0 && action.type < static_cast<int32>(status_list.size()))
					remove_custom_status(*bl, *status_list[action.type], action.val1, action.source_id);
				break;
			case SKILL_LUA_CUT_DELAY:
				if (unit_data* ud = bl != nullptr ? unit_bl2ud(bl) : nullptr; ud != nullptr) {
					t_tick now = gettick();

					if (DIFF_TICK(ud->canact_tick, now) > 0) {
						ud->canact_tick = std::max<t_tick>(now, ud->canact_tick - action.duration);
						if (battle_config.display_status_timers && bl->type == BL_PC)
							clif_status_change(bl, EFST_POSTDELAY, 1, DIFF_TICK(ud->canact_tick, now), 0, 0, 0);
					}
				}
				break;
			case SKILL_LUA_CUT_COOLDOWN:
				if (bl != nullptr)
					cut_cooldown(*bl, static_cast<uint16>(action.type), action.duration);
				break;
			case SKILL_LUA_COOLDOWN:
				if (bl != nullptr)
					set_cooldown(*bl, static_cast<uint16>(action.type), action.duration);
				break;
			case SKILL_LUA_GIVE_ITEM:
				if (map_session_data* sd = bl != nullptr ? BL_CAST(BL_PC, bl) : nullptr; sd != nullptr)
					give_item(*sd, static_cast<t_itemid>(action.type), action.val1, action.flag != 0);
				break;
			case SKILL_LUA_DAMAGE:
				if (bl != nullptr && !status_isdead(*bl) && action.hp > 0) {
					// Passive damage, as poison's: it interrupts no cast.
					clif_damage(*bl, *bl, gettick(), 0, 0, action.hp, 1, DMG_NORMAL, 0, false);
					status_damage(src, bl, action.hp, 0, 0, 1, 0);
				}
				break;
		}
	}
	hit.reset();
}

// ---------------------------------------------------------------------------
// What the upstream files call for mods' own skills and statuses
// ---------------------------------------------------------------------------

namespace {

/// c.attack_skill and c.attack_skill_id: the skill a blow was dealt with.
void add_attack_skill(uint16 skill_id) {
	set_string("attack_skill", skill_id != 0 ? skill_get_name(skill_id) : "");
	set_int("attack_skill_id", skill_id);
}

/// What a cost hook changed in the table at `cost`.
/// The items a cost table lists, as {item id, amount}, in order. Entries
/// naming no item are skipped. Read raw: no metamethod runs.
std::vector<std::pair<t_itemid, int32>> read_cost_items(int32 items) {
	std::vector<std::pair<t_itemid, int32>> out;

	for (int32 i = 1; static_cast<int32>(out.size()) < MAX_SKILL_ITEM_REQUIRE; i++) {
		if (lua_rawgeti(L, items, i) == LUA_TNIL) {
			lua_pop(L, 1);
			break;
		}
		if (lua_istable(L, -1)) {
			std::shared_ptr<item_data> id;

			lua_pushstring(L, "item");
			lua_rawget(L, -2);
			if (lua_type(L, -1) == LUA_TSTRING)
				id = item_db.search_aegisname(lua_tostring(L, -1));
			else if (lua_isinteger(L, -1))
				id = item_db.find(static_cast<t_itemid>(lua_tointeger(L, -1)));
			lua_pushstring(L, "amount");
			lua_rawget(L, -3);

			// 0: the player must have it, and keeps it (a Paint Brush).
			lua_Integer amount = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 1;

			lua_pop(L, 2);
			if (id != nullptr && amount >= 0)
				out.emplace_back(id->nameid, static_cast<int32>(std::min<lua_Integer>(amount, MAX_AMOUNT)));
		}
		lua_pop(L, 1);
	}
	return out;
}

/// What a cost hook changed in the table at `cost`. `pushed` is the item
/// list it was given: the items are only rewritten when the hook changed
/// them, because some skills find their item by its slot (Aid Potion's by
/// the level used).
void read_cost(int32 cost, s_skill_condition& req, const std::vector<std::pair<t_itemid, int32>>& pushed) {
	auto field = [&](const char* name, int32& out, int32 min) {
		lua_pushstring(L, name);
		if (lua_rawget(L, cost) == LUA_TNUMBER && lua_isinteger(L, -1))
			out = static_cast<int32>(std::clamp<lua_Integer>(lua_tointeger(L, -1), min, INT32_MAX));
		lua_pop(L, 1);
	};

	field("sp", req.sp, 0);
	field("hp", req.hp, 0);
	field("ap", req.ap, 0);
	field("zeny", req.zeny, 0);
	field("spiritball", req.spiritball, -1);  // -1: all of them
	field("ammo_qty", req.ammo_qty, 0);

	lua_pushstring(L, "items");
	if (lua_rawget(L, cost) == LUA_TTABLE) {
		std::vector<std::pair<t_itemid, int32>> wanted = read_cost_items(lua_gettop(L));

		if (wanted != pushed) {
			memset(req.itemid, 0, sizeof(req.itemid));
			memset(req.amount, 0, sizeof(req.amount));
			for (size_t n = 0; n < wanted.size(); n++) {
				req.itemid[n] = wanted[n].first;
				req.amount[n] = wanted[n].second;
			}
		}
	}
	lua_pop(L, 1);
}

}  // namespace

void skill_lua_bonus(map_session_data* sd) {
	if (L == nullptr || sd == nullptr)
		return;
	if (learner_skills[HOOK_BONUS].empty() && !has_status_hook(sd->id, SHOOK_BONUS))
		return;

	acting_for none(nullptr);
	map_session_data* previous = bonus_sd;

	bonus_sd = sd;
	for_each_learned(HOOK_BONUS, *sd, [&](uint16 skill_id, uint16 lv, s_hook& hook, const char* name) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, lv, sd, nullptr, nullptr);
		protected_call(hook, name, 1, 0);
	});
	for_each_status(SHOOK_BONUS, *sd, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(0, 0, on.source_id != 0 ? map_id2bl(on.source_id) : nullptr, sd, nullptr);
		add_status_fields(def, on, *sd);
		protected_call(hook, def.name.c_str(), 1, 0);
	});
	bonus_sd = previous;
}

namespace {

/// The table a stats hook gets and changes, at the top of the stack.
/// The fastest attack motion the server allows `bl`: what read_stats keeps a
/// hook's amotion within.
int32 amotion_floor(block_list& bl) {
	if (map_session_data* sd = BL_CAST(BL_PC, &bl); sd != nullptr)
		return pc_maxaspd(sd) / AMOTION_DIVIDER_PC;
	return MAX_ASPD_NOPC / AMOTION_DIVIDER_NOPC;
}

void push_stats(block_list& bl, const status_data& st) {
	lua_createtable(L, 0, 25);
	set_int("def", st.def);
	set_int("def2", st.def2);
	set_int("mdef", st.mdef);
	set_int("mdef2", st.mdef2);
	set_int("hit", st.hit);
	set_int("flee", st.flee);
	set_int("flee2", st.flee2);
	set_int("cri", st.cri);
	set_int("speed", st.speed);
	set_int("batk", st.batk);
#ifdef RENEWAL
	set_int("atk", st.watk);
#else
	set_int("atk", st.rhw.atk);
#endif
	set_int("matk_min", st.matk_min);
	set_int("matk_max", st.matk_max);
	set_int("amotion", st.amotion);
	set_int("maxhp", st.max_hp);
	set_int("maxsp", st.max_sp);
	// To read: a stats hook runs after they count; and how far amotion goes.
	set_int("amotion_min", amotion_floor(bl));
	set_int("str", st.str);
	set_int("agi", st.agi);
	set_int("vit", st.vit);
	set_int("int", st.int_);
	set_int("dex", st.dex);
	set_int("luk", st.luk);
}

/// Take back what a stats hook changed in the table at `stats`: only what
/// status_calc_bl_main recalculated just now (`flag`). Anything else keeps
/// what it has, which already holds this hook's last change. The server
/// leaves batk, cri and flee2 alone while their base is 0 (as a monster's
/// critical is by default), so they are left alone too.
template <size_t N>
void read_stats(block_list& bl, status_data& st, int32 stats, const std::bitset<N>& flag) {
	auto field = [&](e_scb_flag scb, const char* name, int64 min, int64 max, auto& out, bool recalculated = true) {
		if (!flag[scb] || !recalculated)
			return false;
		lua_pushstring(L, name);

		bool changed = lua_rawget(L, stats) == LUA_TNUMBER && lua_isinteger(L, -1);

		if (changed)
			out = static_cast<std::remove_reference_t<decltype(out)>>(std::clamp<int64>(lua_tointeger(L, -1), min, max));
		lua_pop(L, 1);
		return changed;
	};
	const status_data* base = status_get_base_status(&bl);

	field(SCB_DEF, "def", DEFTYPE_MIN, DEFTYPE_MAX, st.def);
	field(SCB_DEF2, "def2", INT16_MIN, INT16_MAX, st.def2);
	field(SCB_MDEF, "mdef", DEFTYPE_MIN, DEFTYPE_MAX, st.mdef);
	field(SCB_MDEF2, "mdef2", INT16_MIN, INT16_MAX, st.mdef2);
	field(SCB_HIT, "hit", 1, INT16_MAX, st.hit);
	field(SCB_FLEE, "flee", 1, INT16_MAX, st.flee);
	field(SCB_FLEE2, "flee2", 0, INT16_MAX, st.flee2, base->flee2 != 0);
	field(SCB_CRI, "cri", 0, INT16_MAX, st.cri, base->cri != 0);
	field(SCB_SPEED, "speed", MIN_WALK_SPEED, MAX_WALK_SPEED, st.speed);
	field(SCB_BATK, "batk", 0, INT32_MAX, st.batk, base->batk != 0);
#ifdef RENEWAL
	field(SCB_WATK, "atk", 0, UINT16_MAX, st.watk);
#else
	field(SCB_WATK, "atk", 0, UINT16_MAX, st.rhw.atk);
#endif
	field(SCB_MATK, "matk_min", 0, UINT16_MAX, st.matk_min);
	field(SCB_MATK, "matk_max", 0, UINT16_MAX, st.matk_max);

	// Attack motion, kept within what the server allows the unit, with the
	// attack delay that follows it.
	if (map_session_data* sd = BL_CAST(BL_PC, &bl); sd != nullptr) {
		if (field(SCB_ASPD, "amotion", amotion_floor(bl), MIN_ASPD / AMOTION_DIVIDER_PC, st.amotion))
			st.adelay = AMOTION_DIVIDER_PC * st.amotion;
	} else if (field(SCB_ASPD, "amotion", amotion_floor(bl), MIN_ASPD / AMOTION_DIVIDER_NOPC, st.amotion)) {
		st.adelay = bl.type == BL_HOM ? AMOTION_DIVIDER_NOPC * st.amotion : std::max<int32>(st.adelay, AMOTION_DIVIDER_NOPC * st.amotion);
	}
	if (field(SCB_MAXHP, "maxhp", 1, INT32_MAX, st.max_hp) && st.hp > st.max_hp)
		st.hp = st.max_hp;
	if (field(SCB_MAXSP, "maxsp", 1, INT32_MAX, st.max_sp) && st.sp > st.max_sp)
		st.sp = st.max_sp;
}

}  // namespace

template <size_t N>
void skill_lua_stats(block_list& bl, const std::bitset<N>& flag) {
	static_assert(N == SCB_MAX, "skill_lua_stats takes status_calc_bl_main's flags");

	if (L == nullptr)
		return;

	map_session_data* sd = BL_CAST(BL_PC, &bl);
	bool learners = sd != nullptr && !learner_skills[HOOK_STATS].empty();

	if (!learners && !has_status_hook(bl.id, SHOOK_STATS))
		return;

	status_data* st = status_get_status_data(bl);
	acting_for none(nullptr);

	// The passives' first, then the statuses': each sees what the one before
	// left.
	if (learners) {
		for_each_learned(HOOK_STATS, *sd, [&](uint16 skill_id, uint16 lv, s_hook& hook, const char* name) {
			push_stats(bl, *st);

			int32 stats = lua_gettop(L);

			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, lv, sd, sd, nullptr);
			lua_pushvalue(L, stats);
			if (protected_call(hook, name, 2, 0))
				read_stats(bl, *st, stats, flag);
			lua_settop(L, stats - 1);
		});
	}
	for_each_status(SHOOK_STATS, bl, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
		push_stats(bl, *st);

		int32 stats = lua_gettop(L);

		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(0, 0, on.source_id != 0 ? map_id2bl(on.source_id) : nullptr, &bl, nullptr);
		add_status_fields(def, on, bl);
		lua_pushvalue(L, stats);
		if (protected_call(hook, def.name.c_str(), 2, 0))
			read_stats(bl, *st, stats, flag);
		lua_settop(L, stats - 1);
	});
}

// status_calc_bl_main passes its flags as they are; the header needs no status.hpp.
template void skill_lua_stats<SCB_MAX>(block_list&, const std::bitset<SCB_MAX>&);

void skill_lua_damage(int32 attack_type, block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, Damage& d, int32 count) {
	if (L == nullptr || src == nullptr || target == nullptr || d.damage + d.damage2 <= 0)
		return;

	// A c:cast or c:attack's power, or that of what a ground skill placed
	// with one, scales the damage first: the hooks see what the hit deals.
	if (int32 percent = power_of(*src, skill_id); percent != 100) {
		d.damage = d.damage * percent / 100;
		d.damage2 = d.damage2 * percent / 100;
		if (d.damage + d.damage2 <= 0)
			return;
	}

	map_session_data* ssd = BL_CAST(BL_PC, src);
	map_session_data* tsd = BL_CAST(BL_PC, target);

	if (!(ssd != nullptr && !learner_skills[HOOK_DAMAGE_DEALT].empty()) && !(tsd != nullptr && !learner_skills[HOOK_DAMAGE_TAKEN].empty())
		&& !has_status_hook(src->id, SHOOK_DAMAGE_DEALT) && !has_status_hook(target->id, SHOOK_DAMAGE_TAKEN))
		return;

	const int64 right_before = d.damage, left_before = d.damage2;
	int32 element = skill_id != 0 ? skill_get_ele(skill_id, skill_lv) : -1;

	if (element < 0)  // the weapon's, which carries any endow
		element = status_get_status_data(*src)->rhw.ele;

	s_damage_ctx dmg = { d.damage + d.damage2, ATK_DEF, element, d.flag | attack_type, d.type == DMG_CRITICAL || d.type == DMG_MULTI_HIT_CRITICAL, skill_id, skill_lv };
	int64 right = d.damage, left = d.damage2;
	acting_for none(nullptr);

	dmg.hits = d.div_ < 0 ? -d.div_ : d.div_;
	// What the server divides a Splash Split skill's damage by (and, in
	// pre-renewal, an auto-cast Blitz Beat's); for any other skill the number
	// means something else, or nothing.
	bool split = skill_id != 0 && skill_get_nk(skill_id, NK_SPLASHSPLIT);
#ifndef RENEWAL
	split = split || skill_id == HT_BLITZBEAT;
#endif
	dmg.targets = count > 0 && split ? count : 0;

	// The hook at the top with its context: returns the new total and, if
	// it says so, the left hand's part of it. Otherwise both hands keep
	// their shares.
	auto call = [&](s_hook& hook, const char* what, const char* owner) {
		lua_pushinteger(L, right + left);
		if (!protected_call(hook, owner, 2, 2))
			return;

		int64 total = right + left;

		lua_pushvalue(L, -2);
		read_number(hook, what, owner, total);  // the first result: the new total
		lua_pop(L, 1);
		total = std::max<int64>(0, total);
		if (lua_type(L, -1) == LUA_TNUMBER) {   // the second: the left hand's part of it
			int64 asked = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : static_cast<int64>(lua_tonumber(L, -1));
			int64 new_left = std::clamp<int64>(asked, 0, total);

			left = new_left;
			right = total - new_left;
		} else if (total != right + left) {
			if (left == 0) {
				right = total;
			} else {
				int64 new_right = static_cast<int64>(static_cast<double>(right) * static_cast<double>(total) / static_cast<double>(right + left));

				right = new_right;
				left = total - new_right;
			}
		}
		lua_pop(L, 2);
	};
	auto before = [&]() {
		dmg.damage = right + left;
		dmg.damage_left = left;
	};

	// The attacker's side first, then the defender's; each sees what the
	// one before returned, as c.damage and as its second argument.
	if (ssd != nullptr) {
		for_each_learned(HOOK_DAMAGE_DEALT, *ssd, [&](uint16 passive, uint16 lv, s_hook& hook, const char* name) {
			before();
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(passive, lv, src, target, &dmg);
			call(hook, "damage_dealt", name);
		});
	}
	for_each_status(SHOOK_DAMAGE_DEALT, *src, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
		before();
		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, skill_lv, src, target, &dmg);
		add_status_fields(def, on, *src);
		call(hook, "damage_dealt", def.name.c_str());
	});
	if (tsd != nullptr) {
		for_each_learned(HOOK_DAMAGE_TAKEN, *tsd, [&](uint16 passive, uint16 lv, s_hook& hook, const char* name) {
			before();
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(passive, lv, src, target, &dmg);
			call(hook, "damage_taken", name);
		});
	}
	for_each_status(SHOOK_DAMAGE_TAKEN, *target, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
		before();
		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, skill_lv, src, target, &dmg);
		add_status_fields(def, on, *target);
		call(hook, "damage_taken", def.name.c_str());
	});

	if (right != right_before || left != left_before) {
		d.damage = right;
		d.damage2 = left;
	}
}

namespace {

/// c.used_skill, c.used_skill_id, c.used_skill_lv: the skill a passive's or
/// a status's hook is asked about.
void add_used_skill(uint16 skill_id, uint16 skill_lv) {
	set_string("used_skill", skill_id != 0 ? skill_get_name(skill_id) : "");
	set_int("used_skill_id", skill_id);
	set_int("used_skill_lv", skill_lv);
}

/// Call a hook that answers true or false (critical): `value` is what the
/// one before decided; nil keeps it.
void call_bool(s_hook& hook, const char* what, const char* owner, bool& value) {
	lua_pushboolean(L, value);
	if (!protected_call(hook, owner, 2, 1))
		return;
	if (lua_isboolean(L, -1))
		value = lua_toboolean(L, -1) != 0;
	else if (!lua_isnil(L, -1))
		ShowError("Lua: %s's %s hook for %s returned a %s, not true, false or nil; keeping the running answer.\n", hook.mod.c_str(), what, owner, luaL_typename(L, -1));
	lua_pop(L, 1);
}

}  // namespace

void skill_lua_critical(Damage& wd, block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv) {
	if (L == nullptr || src == nullptr || target == nullptr || current_hit != nullptr)
		return;

	s_skill_hooks* hooks = skill_id != 0 ? hooks_for(skill_id) : nullptr;
	bool own = hooks != nullptr && !hooks->hooks[HOOK_CRITICAL].empty();

	if (!own && !has_status_hook(src->id, SHOOK_CRITICAL))
		return;

	const bool stock = wd.type == DMG_CRITICAL || wd.type == DMG_MULTI_HIT_CRITICAL;
	bool value = stock;
	acting_for none(nullptr);

	// The skill's own first, then the attacker's statuses'.
	if (own) {
		for (s_hook& hook : hooks->hooks[HOOK_CRITICAL]) {
			if (hook.ref == LUA_NOREF)
				continue;
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, skill_lv, src, target, nullptr);
			call_bool(hook, "critical", hooks->skill.c_str(), value);
		}
	}
	for_each_status(SHOOK_CRITICAL, *src, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, skill_lv, src, target, nullptr);
		add_status_fields(def, on, *src);
		add_used_skill(skill_id, skill_lv);
		call_bool(hook, "critical", def.name.c_str(), value);
	});

	if (value == stock)
		return;
	if (value) {
#if PACKETVER >= 20161207
		wd.type = wd.type == DMG_MULTI_HIT ? DMG_MULTI_HIT_CRITICAL : DMG_CRITICAL;
#else
		wd.type = DMG_CRITICAL;
#endif
	} else {
		wd.type = wd.type == DMG_MULTI_HIT_CRITICAL ? DMG_MULTI_HIT : DMG_NORMAL;
	}
}

void skill_lua_on_cast(block_list* src, block_list* target, int16 x, int16 y, uint16 skill_id, uint16 skill_lv) {
	if (L == nullptr || src == nullptr || current_hit != nullptr || in_lua_cast)
		return;

	s_skill_hooks* hooks = hooks_for(skill_id);
	map_session_data* sd = BL_CAST(BL_PC, src);
	bool own = hooks != nullptr && !hooks->hooks[HOOK_ON_CAST].empty();
	bool learners = sd != nullptr && !learner_skills[HOOK_ON_SKILL].empty();
	bool statuses = has_status_hook(src->id, SHOOK_ON_SKILL);

	if (!own && !learners && !statuses)
		return;

	std::unique_ptr<s_skill_lua_hit> hit = new_hit(src, target, skill_id);

	{
		acting_for acting(hit.get());

		if (own) {
			for (s_hook& hook : hooks->hooks[HOOK_ON_CAST]) {
				if (hook.ref == LUA_NOREF)
					continue;
				lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
				push_context(skill_id, skill_lv, src, target, nullptr);
				set_int("x", x);
				set_int("y", y);
				call_owned(hook, hooks->skill.c_str(), src);
			}
		}
		if (learners) {
			for_each_learned(HOOK_ON_SKILL, *sd, [&](uint16 passive, uint16 lv, s_hook& hook, const char* name) {
				lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
				push_context(passive, lv, src, target, nullptr);
				add_used_skill(skill_id, skill_lv);
				set_int("x", x);
				set_int("y", y);
				call_owned(hook, name, src);
			});
		}
		for_each_status(SHOOK_ON_SKILL, *src, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, skill_lv, src, target, nullptr);
			add_status_fields(def, on, *src);
			add_used_skill(skill_id, skill_lv);
			set_int("x", x);
			set_int("y", y);
			call_owned(hook, def.name.c_str(), src);
		});
	}
	skill_lua_apply(hit);
}

bool skill_lua_can_cast(map_session_data& sd, uint16 skill_id, uint16 skill_lv) {
	if (L == nullptr)
		return true;

	s_skill_hooks* hooks = hooks_for(skill_id);
	bool own = hooks != nullptr && !hooks->hooks[HOOK_CAN_CAST].empty();

	if (!own && !has_status_hook(sd.id, SHOOK_CAN_CAST))
		return true;

	acting_for none(nullptr);
	bool refused = false;
	std::string message;

	// What a can_cast hook at the top returned: false refuses, with an
	// optional message.
	auto answer = [&](s_hook& hook, const char* owner) {
		if (!protected_call(hook, owner, 1, 2))
			return;  // a broken hook stops no one
		refused = lua_isboolean(L, -2) && !lua_toboolean(L, -2);
		message = refused && lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
		lua_pop(L, 2);
	};

	// The skill's own, then the player's statuses' (which see c.used_skill).
	if (own) {
		for (s_hook& hook : hooks->hooks[HOOK_CAN_CAST]) {
			if (hook.ref == LUA_NOREF)
				continue;
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, skill_lv, &sd, nullptr, nullptr);
			answer(hook, hooks->skill.c_str());
			if (refused)
				break;
		}
	}
	if (!refused) {
		for_each_status(SHOOK_CAN_CAST, sd, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
			if (refused)
				return;
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, skill_lv, &sd, nullptr, nullptr);
			add_status_fields(def, on, sd);
			add_used_skill(skill_id, skill_lv);
			answer(hook, def.name.c_str());
		});
	}
	if (!refused)
		return true;
	if (!message.empty())
		clif_displaymessage(sd.fd, message.c_str());
	clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL);
	return false;
}

namespace {

/// The table a cost hook gets and changes, at the top of the stack; the
/// items it lists go into `pushed`.
void push_cost(const s_skill_condition& req, std::vector<std::pair<t_itemid, int32>>& pushed) {
	lua_createtable(L, 0, 7);
	set_int("sp", req.sp);
	set_int("hp", req.hp);
	set_int("ap", req.ap);
	set_int("zeny", req.zeny);
	set_int("spiritball", req.spiritball);
	set_int("ammo_qty", req.ammo_qty);
	lua_createtable(L, MAX_SKILL_ITEM_REQUIRE, 0);
	for (int32 i = 0; i < MAX_SKILL_ITEM_REQUIRE; i++) {
		std::shared_ptr<item_data> id = req.itemid[i] != 0 ? item_db.find(req.itemid[i]) : nullptr;

		if (id == nullptr)
			continue;
		lua_createtable(L, 0, 2);
		set_string("item", id->name.c_str());
		set_int("amount", req.amount[i]);
		lua_rawseti(L, -2, static_cast<lua_Integer>(pushed.size()) + 1);
		pushed.emplace_back(id->nameid, req.amount[i]);
	}
	lua_setfield(L, -2, "items");
}

}  // namespace

void skill_lua_requirement(map_session_data& sd, uint16 skill_id, uint16 skill_lv, s_skill_condition& req) {
	if (L == nullptr)
		return;

	s_skill_hooks* hooks = hooks_for(skill_id);
	bool own = hooks != nullptr && !hooks->hooks[HOOK_COST].empty();

	if (!own && !has_status_hook(sd.id, SHOOK_COST))
		return;

	acting_for none(nullptr);

	// Where the use goes, once the server knows: while it charges for a use
	// it has started (the cast's end), not when it first checks one. A unit,
	// or a cell for a ground skill.
	block_list* target = nullptr;
	bool cell = false;

	if (sd.ud.skill_id == skill_id) {
		if (sd.ud.skilltarget != 0)
			target = map_id2bl(sd.ud.skilltarget);
		else
			cell = sd.ud.skillx != 0 || sd.ud.skilly != 0;
	}

	auto where = [&]() {
		if (cell) {
			set_int("x", sd.ud.skillx);
			set_int("y", sd.ud.skilly);
		}
	};

	// The skill's own, then the player's statuses' (which see c.used_skill);
	// each sees what the one before changed.
	if (own) {
		for (s_hook& hook : hooks->hooks[HOOK_COST]) {
			if (hook.ref == LUA_NOREF)
				continue;

			std::vector<std::pair<t_itemid, int32>> pushed;

			push_cost(req, pushed);

			int32 cost = lua_gettop(L);

			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, skill_lv, &sd, target, nullptr);
			where();
			lua_pushvalue(L, cost);
			if (protected_call(hook, hooks->skill.c_str(), 2, 0))
				read_cost(cost, req, pushed);
			lua_settop(L, cost - 1);
		}
	}
	for_each_status(SHOOK_COST, sd, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
		std::vector<std::pair<t_itemid, int32>> pushed;

		push_cost(req, pushed);

		int32 cost = lua_gettop(L);

		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, skill_lv, &sd, target, nullptr);
		where();
		add_status_fields(def, on, sd);
		add_used_skill(skill_id, skill_lv);
		lua_pushvalue(L, cost);
		if (protected_call(hook, def.name.c_str(), 2, 0))
			read_cost(cost, req, pushed);
		lua_settop(L, cost - 1);
	});
}

namespace {

/// The most a range hook may make a skill's range: past what a client shows.
constexpr int64 MAX_LUA_RANGE = 30;

/// True while range hooks run: a range they ask for (unit:skill_range) is
/// the server's own.
bool in_range_hook = false;

}  // namespace

int32 skill_lua_range(const block_list* bl, uint16 skill_id, uint16 skill_lv, int32 range) {
	if (L == nullptr || bl == nullptr || in_range_hook)
		return range;

	s_skill_hooks* hooks = hooks_for(skill_id);
	bool own = hooks != nullptr && !hooks->hooks[HOOK_RANGE].empty();

	if (!own && !has_status_hook(bl->id, SHOOK_RANGE))
		return range;

	int64 value = range;
	acting_for none(nullptr);

	in_range_hook = true;
	// The skill's own, then the unit's statuses' (which see c.used_skill);
	// each sees what the one before returned.
	if (own)
		call_number_hook(HOOK_RANGE, skill_id, skill_lv, bl, nullptr, value);
	for_each_status(SHOOK_RANGE, *bl, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, skill_lv, bl, nullptr, nullptr);
		add_status_fields(def, on, *bl);
		add_used_skill(skill_id, skill_lv);
		call_number(hook, "range", def.name.c_str(), value);
	});
	in_range_hook = false;
	if (value == range)
		return range;  // no hook changed it: the server's own, untouched
	return static_cast<int32>(std::clamp<int64>(value, 0, MAX_LUA_RANGE));
}

bool skill_lua_on_kill(block_list* src, block_list* target, uint16 skill_id, int64 damage, int64 hp_lost) {
	if (L == nullptr || target == nullptr)
		return false;

	map_session_data* killer = src != nullptr ? BL_CAST(BL_PC, src) : nullptr;
	map_session_data* dying = BL_CAST(BL_PC, target);
	bool kill_hooks = (killer != nullptr && !learner_skills[HOOK_ON_KILL].empty()) || (src != nullptr && has_status_hook(src->id, SHOOK_ON_KILL));
	bool death_hooks = (dying != nullptr && !learner_skills[HOOK_ON_DEATH].empty()) || has_status_hook(target->id, SHOOK_ON_DEATH);
	bool survived = false;

	if (kill_hooks || death_hooks) {
		std::unique_ptr<s_skill_lua_hit> hit = new_hit(src, target, skill_id);
		int32 element = skill_id != 0 ? skill_get_ele(skill_id, 1) : -1;

		if (element < 0 && src != nullptr)  // the weapon's, which carries any endow
			element = status_get_status_data(*src)->rhw.ele;

		// c.attack_skill, c.damage (the whole blow), c.overkill (what the
		// blow had left over) and c.element (the blow's). With exp_calc_type
		// 1 the server has already cut the blow down to the HP it took, so
		// there c.damage is that and c.overkill is 0.
		auto add_blow = [&]() {
			add_attack_skill(skill_id);
			set_int("damage", damage);
			set_int("overkill", std::max<int64>(0, damage - hp_lost));
			set_int("element", std::max(element, 0));
		};

		{
			acting_for acting(hit.get());

			// The dying unit's hooks first: a kill that c:survive() undid is
			// no kill, and runs no on_kill.
			if (death_hooks) {
				bool* previous = current_survive;

				current_survive = &survived;
				if (dying != nullptr) {
					for_each_learned(HOOK_ON_DEATH, *dying, [&](uint16 passive, uint16 lv, s_hook& hook, const char* name) {
						lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
						push_context(passive, lv, src, target, nullptr);
						add_blow();
						call_owned(hook, name, target);
					});
				}
				for_each_status(SHOOK_ON_DEATH, *target, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
					lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
					push_context(skill_id, 0, src, target, nullptr);
					add_status_fields(def, on, *target);
					add_blow();
					call_owned(hook, def.name.c_str(), target);
				});
				current_survive = previous;
			}
			if (kill_hooks && !survived) {
				if (killer != nullptr) {
					for_each_learned(HOOK_ON_KILL, *killer, [&](uint16 passive, uint16 lv, s_hook& hook, const char* name) {
						lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
						push_context(passive, lv, src, target, nullptr);
						add_blow();
						call_owned(hook, name, src);
					});
				}
				for_each_status(SHOOK_ON_KILL, *src, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
					lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
					push_context(skill_id, 0, src, target, nullptr);
					add_status_fields(def, on, *src);
					add_blow();
					call_owned(hook, def.name.c_str(), src);
				});
			}
		}
		// The unit is dying inside the server's own code: later.
		defer(std::move(hit));
	}

	if (survived) {
		status_get_status_data(*target)->hp = 1;
		// The blow's own update has shown 0 HP already.
		switch (target->type) {
			case BL_PC: clif_updatestatus(*dying, SP_HP); break;
			case BL_MOB: mob_heal(reinterpret_cast<mob_data*>(target), 1); break;
			case BL_HOM: hom_heal(reinterpret_cast<homun_data&>(*target), true, false); break;
			case BL_MER: mercenary_heal(reinterpret_cast<s_mercenary_data*>(target), 1, 0); break;
			case BL_ELEM: elemental_heal(reinterpret_cast<s_elemental_data*>(target), 1, 0); break;
			default: break;
		}
		return true;
	}
	clear_on_death(*target);
	return false;
}

namespace {

/// The hook chains a server status's `which` runs: its own status() entry's,
/// then status("*")'s.
std::vector<s_lua_status*> stock_chains(int32 type, e_status_hook which) {
	std::vector<s_lua_status*> chains;

	if (auto it = stock_status_hooks.find(type); it != stock_status_hooks.end() && !it->second->hooks[which].empty())
		chains.push_back(it->second);
	if (any_status_hooks != nullptr && !any_status_hooks->hooks[which].empty())
		chains.push_back(any_status_hooks);
	return chains;
}

/// What a hook of a server status sees: c.status_name, c.debuff, c.unit and
/// c.by_skill (the skill its source was using, as far as the server knows:
/// best for a skill's own statuses, "" for one from an item or a script).
void add_stock_status_fields(int32 type, const char* name, const block_list* src, const block_list& bl) {
	std::shared_ptr<s_status_change_db> scdb = status_db.find(static_cast<sc_type>(type));
	uint16 by_skill = src != nullptr ? battle_getcurrentskill(src) : 0;

	set_string("status_name", name);
	set_bool("debuff", is_debuff(type, scdb.get()));
	push_unit(&bl);
	lua_setfield(L, -2, "unit");
	set_string("by_skill", by_skill != 0 && skill_get_index(by_skill) != 0 ? skill_get_name(by_skill) : "");
}

}  // namespace

int32 skill_lua_status_rate(block_list* src, block_list* bl, int32 type, int32 rate, uint8 flag) {
	if (L == nullptr || bl == nullptr || (stock_status_hooks.empty() && any_status_hooks == nullptr) || (flag & (SCSTART_LOADED | SCSTART_NOAVOID)) || current_hit != nullptr)
		return rate;

	std::vector<s_lua_status*> chains = stock_chains(type, SHOOK_CHANCE);

	if (chains.empty())
		return rate;

	int64 value = rate;
	acting_for none(nullptr);

	for (s_lua_status* def : chains) {
		for (s_hook& hook : def->hooks[SHOOK_CHANCE]) {
			if (hook.ref == LUA_NOREF)
				continue;
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(0, 0, src, bl, nullptr);
			add_stock_status_fields(type, def->name.c_str()[0] == '*' ? status_name(type) : def->name.c_str(), src, *bl);
			call_number(hook, "chance", status_name(type), value);
		}
	}
	// The server passes rates above 10000 on purpose (stacked bAddEff), and
	// resistance takes its share of those: only a changed rate is kept within
	// what a rate can be.
	if (value == rate)
		return rate;
	return static_cast<int32>(std::clamp<int64>(value, 0, INT32_MAX));
}

bool skill_lua_status_start(block_list* src, block_list* bl, int32 type, int32 val1, int32 val2, int32 val3, int32 val4, int32& tick, uint8 flag) {
	if (L == nullptr || bl == nullptr || (stock_status_hooks.empty() && any_status_hooks == nullptr) || (flag & SCSTART_LOADED))
		return true;

	std::vector<s_lua_status*> chains = stock_chains(type, SHOOK_ON_START);

	if (chains.empty())
		return true;

	std::unique_ptr<s_skill_lua_hit> hit = new_hit(src, bl, 0);
	bool refused = false;

	{
		acting_for acting(hit.get());

		for (s_lua_status* def : chains) {
			for (s_hook& hook : def->hooks[SHOOK_ON_START]) {
				if (hook.ref == LUA_NOREF)
					continue;
				lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
				push_context(0, 0, src, bl, nullptr);
				add_stock_status_fields(type, def->name.c_str()[0] == '*' ? status_name(type) : def->name.c_str(), src, *bl);
				set_int("duration", tick);
				set_int("val1", val1);
				set_int("val2", val2);
				set_int("val3", val3);
				set_int("val4", val4);
				if (!call_owned(hook, status_name(type), bl, 1, 1))
					continue;
				if (lua_isboolean(L, -1)) {
					refused = !lua_toboolean(L, -1);
				} else if (lua_type(L, -1) == LUA_TNUMBER) {
					// A duration; 0 refuses, as false does.
					int64 value = tick;

					read_number(hook, "on_start", status_name(type), value);
					if (value == 0)
						refused = true;
					else
						tick = static_cast<int32>(std::clamp<int64>(value, INFINITE_TICK, INT32_MAX));
				} else {
					int64 kept = tick;

					read_number(hook, "on_start", status_name(type), kept);  // nil keeps it; anything else is reported
				}
				lua_pop(L, 1);
				if (refused)
					break;
			}
			if (refused)
				break;
		}
	}
	defer(std::move(hit));
	return !refused;
}

void skill_lua_status_end(block_list* bl, int32 type, int32 val1, int32 val2, int32 val3, int32 val4) {
	if (L == nullptr || bl == nullptr || (stock_status_hooks.empty() && any_status_hooks == nullptr))
		return;

	std::vector<s_lua_status*> chains = stock_chains(type, SHOOK_ON_END);

	if (chains.empty())
		return;

	std::unique_ptr<s_skill_lua_hit> hit = new_hit(nullptr, bl, 0);

	{
		acting_for acting(hit.get());

		for (s_lua_status* def : chains) {
			for (s_hook& hook : def->hooks[SHOOK_ON_END]) {
				if (hook.ref == LUA_NOREF)
					continue;
				lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
				push_context(0, 0, nullptr, bl, nullptr);
				add_stock_status_fields(type, def->name.c_str()[0] == '*' ? status_name(type) : def->name.c_str(), nullptr, *bl);
				set_int("val1", val1);  // what it held: it is gone by now
				set_int("val2", val2);
				set_int("val3", val3);
				set_int("val4", val4);
				call_owned(hook, status_name(type), bl);
			}
		}
	}
	defer(std::move(hit));
}

namespace {

/// c.craft_skill, c.craft_skill_id, c.item_id, c.item_name and c.base: what
/// a craft or on_craft hook sees about what is being made.
void add_craft_fields(uint16 skill_id, t_itemid nameid, int64 base) {
	std::shared_ptr<item_data> data = item_db.find(nameid);

	set_string("craft_skill", skill_id != 0 && skill_get_index(skill_id) != 0 ? skill_get_name(skill_id) : "");
	set_int("craft_skill_id", skill_id);
	set_int("item_id", nameid);
	set_string("item_name", data != nullptr ? data->name.c_str() : "");
	if (base >= 0)
		set_int("base", base);
}

/// Take back what an on_craft hook changed in the item table at `arg`: how
/// many (for what stacks) and the random options.
void read_crafted(int32 arg, item& made) {
	std::shared_ptr<item_data> data = item_db.find(made.nameid);

	lua_pushstring(L, "amount");
	if (lua_rawget(L, arg) == LUA_TNUMBER && lua_isinteger(L, -1) && data != nullptr && itemdb_isstackable2(data.get()))
		made.amount = static_cast<int16>(std::clamp<lua_Integer>(lua_tointeger(L, -1), 1, MAX_AMOUNT));
	lua_pop(L, 1);

	lua_pushstring(L, "options");
	if (lua_rawget(L, arg) == LUA_TTABLE) {
		int32 options = lua_gettop(L);

		memset(made.option, 0, sizeof(made.option));
		for (int32 i = 1, o = 0; o < MAX_ITEM_RDM_OPT; i++) {
			if (lua_rawgeti(L, options, i) != LUA_TTABLE) {
				lua_pop(L, 1);
				break;
			}

			int32 opt = lua_gettop(L);
			int64 id = 0;

			lua_pushstring(L, "id");
			if (lua_rawget(L, opt) == LUA_TSTRING)
				script_get_constant(lua_tostring(L, -1), &id);
			else if (lua_isinteger(L, -1))
				id = lua_tointeger(L, -1);
			lua_pop(L, 1);
			lua_pushstring(L, "value");
			lua_rawget(L, opt);
			int64 value = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
			lua_pop(L, 1);
			lua_pushstring(L, "param");
			lua_rawget(L, opt);
			int64 param = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
			lua_pop(L, 2);  // param, and the option table

			if (id <= 0 || !random_option_db.exists(static_cast<uint16>(id))) {
				ShowWarning("Lua: an on_craft hook gave %s a random option that does not exist (%d); it is left out.\n", data != nullptr ? data->name.c_str() : "an item", static_cast<int32>(id));
				continue;
			}
			made.option[o].id = static_cast<int16>(id);
			made.option[o].value = static_cast<int16>(std::clamp<int64>(value, INT16_MIN, INT16_MAX));
			made.option[o].param = static_cast<char>(std::clamp<int64>(param, 0, 127));
			o++;
		}
	}
	lua_pop(L, 1);
}

}  // namespace

int32 skill_lua_craft_rate(map_session_data* sd, uint16 skill_id, t_itemid nameid, int32 rate) {
	if (L == nullptr || sd == nullptr || current_hit != nullptr)
		return rate;

	bool learners = !learner_skills[HOOK_CRAFT].empty();

	if (!learners && !has_status_hook(sd->id, SHOOK_CRAFT))
		return rate;

	int64 value = rate;
	const int64 base = rate;
	acting_for none(nullptr);

	// The passives' first, then the statuses'; each sees what the one before
	// returned, and c.base is the server's own chance.
	if (learners) {
		for_each_learned(HOOK_CRAFT, *sd, [&](uint16 passive, uint16 lv, s_hook& hook, const char* name) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(passive, lv, sd, sd, nullptr);
			add_craft_fields(skill_id, nameid, base);
			call_number(hook, "craft", name, value);
		});
	}
	for_each_status(SHOOK_CRAFT, *sd, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(0, 0, on.source_id != 0 ? map_id2bl(on.source_id) : nullptr, sd, nullptr);
		add_status_fields(def, on, *sd);
		add_craft_fields(skill_id, nameid, base);
		call_number(hook, "craft", def.name.c_str(), value);
	});
	return static_cast<int32>(std::clamp<int64>(value, 0, 100000));
}

void skill_lua_crafted(map_session_data* sd, uint16 skill_id, item& made) {
	if (L == nullptr || sd == nullptr || current_hit != nullptr)
		return;

	bool learners = !learner_skills[HOOK_ON_CRAFT].empty();

	if (!learners && !has_status_hook(sd->id, SHOOK_ON_CRAFT))
		return;

	std::unique_ptr<s_skill_lua_hit> hit = new_hit(sd, sd, skill_id);

	{
		acting_for acting(hit.get());

		// The hook at the top with its context: the item made is its second
		// argument, and what the hook changes in it is kept.
		auto call = [&](s_hook& hook, const char* owner) {
			push_item(made);          // fn, c, item
			lua_pushvalue(L, -1);
			lua_insert(L, -4);        // item, fn, c, item: a copy to read back
			if (call_owned(hook, owner, sd, 2, 0))
				read_crafted(lua_gettop(L), made);
			lua_pop(L, 1);
		};

		if (learners) {
			for_each_learned(HOOK_ON_CRAFT, *sd, [&](uint16 passive, uint16 lv, s_hook& hook, const char* name) {
				lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
				push_context(passive, lv, sd, sd, nullptr);
				add_craft_fields(skill_id, made.nameid, -1);
				call(hook, name);
			});
		}
		for_each_status(SHOOK_ON_CRAFT, *sd, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(0, 0, on.source_id != 0 ? map_id2bl(on.source_id) : nullptr, sd, nullptr);
			add_status_fields(def, on, *sd);
			add_craft_fields(skill_id, made.nameid, -1);
			call(hook, def.name.c_str());
		});
	}
	defer(std::move(hit));
}

void skill_lua_use_item(map_session_data* sd, const item& used) {
	if (sd != nullptr && itemskill_offered.erase(sd->id) > 0)
		sd->state.abra_flag = 0;  // what c:itemskill offered went unused: this item's skill is paid for
	if (L == nullptr || sd == nullptr || current_hit != nullptr)
		return;

	s_item_hooks* hooks = item_hooks_for(used.nameid);
	bool own = hooks != nullptr && !hooks->hooks[ITEM_HOOK_ON_USE].empty();
	bool learners = !learner_skills[HOOK_ON_USE].empty();

	if (!own && !learners && !has_status_hook(sd->id, SHOOK_ON_USE))
		return;

	std::unique_ptr<s_skill_lua_hit> hit = new_hit(sd, sd, 0);

	{
		acting_for acting(hit.get());

		// c.item: what is being used, with who brewed or forged it.
		auto add_item = [&]() {
			push_item(used);
			lua_setfield(L, -2, "item");
		};

		// The item's own first, then the user's passives' and statuses'.
		if (own) {
			for (s_hook& hook : hooks->hooks[ITEM_HOOK_ON_USE]) {
				if (hook.ref == LUA_NOREF)
					continue;
				lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
				push_context(0, 0, sd, sd, nullptr);
				add_item();
				call_owned(hook, hooks->aegis.c_str(), sd);
			}
		}
		if (learners) {
			for_each_learned(HOOK_ON_USE, *sd, [&](uint16 passive, uint16 lv, s_hook& hook, const char* name) {
				lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
				push_context(passive, lv, sd, sd, nullptr);
				add_item();
				call_owned(hook, name, sd);
			});
		}
		for_each_status(SHOOK_ON_USE, *sd, [&](s_lua_status& def, const s_lua_status_on& on, s_hook& hook) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(0, 0, on.source_id != 0 ? map_id2bl(on.source_id) : nullptr, sd, nullptr);
			add_status_fields(def, on, *sd);
			add_item();
			call_owned(hook, def.name.c_str(), sd);
		});
	}
	// Right after the item's own script.
	defer(std::move(hit));
}
