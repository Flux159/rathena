// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef SKILL_LUA_HPP
#define SKILL_LUA_HPP

#include <memory>
#include <vector>

#include <common/cbasetypes.hpp>

/**
 * Skill hooks written in Lua.
 *
 * The files listed in db/import/lua/load.txt (or, with no list, every .lua
 * file in db/import/lua) are run once at start-up, in a sandbox, and
 * register hooks by skill name:
 *
 *   skill("MG_FIREBOLT", {
 *     ratio   = function(c, stock) return stock + c.caster.int // 2 end,
 *     hit     = function(c, stock) ... end,   -- accuracy
 *     element = function(c, stock) ... end,   -- an ELE_* constant
 *     on_hit  = function(c) ... end,          -- after each hit is calculated
 *   })
 *
 * ratio, hit and element run on top of the skill's own class (src/map/skills),
 * so they reach every skill that has one; the stock result is passed in and
 * returning nil keeps it. on_hit runs from skill_attack for any skill, and
 * may ask for a few actions (drain, heal, status, polymorph), which are
 * applied once the hit has been dealt.
 *
 * Items hook the same way, by AegisName, for whoever has them equipped (a
 * card counts when it is slotted in something equipped):
 *
 *   item("My_Card", {
 *     on_attack    = function(c) ... end,  -- the wearer lands a hit
 *     on_hit_taken = function(c) ... end,  -- the wearer is hit
 *   })
 *
 * They run where rAthena's own bAutoSpell and bAutoSpellWhenHit do, and
 * c:cast(skill, level, who) casts the way those do. Nothing a script does can reach the
 * disk, the network or the server's memory; an error or a runaway loop
 * switches that one hook off and says so in the log.
 */

struct block_list;

enum e_skill_lua_action : uint8 {
	SKILL_LUA_DRAIN = 0,
	SKILL_LUA_HEAL,
	SKILL_LUA_STATUS,
	SKILL_LUA_POLYMORPH,
	SKILL_LUA_CAST,
};

/// One thing an on_hit hook asked for. Units are kept by id, never by
/// pointer: by the time it is applied, the target may have died.
struct s_skill_lua_action {
	e_skill_lua_action kind;
	int32 unit_id;      ///< who it happens to
	int32 type;         ///< SKILL_LUA_STATUS: the sc_type; SKILL_LUA_CAST: the skill id
	int32 rate;         ///< SKILL_LUA_STATUS: chance out of 10000
	int32 val1;         ///< SKILL_LUA_STATUS; SKILL_LUA_CAST: the skill level
	int64 duration;     ///< SKILL_LUA_STATUS, in milliseconds
	int64 hp, sp;       ///< SKILL_LUA_HEAL
};

struct s_skill_lua_hit {
	int32 src_id;
	int32 target_id;
	uint16 skill_id;
	int64 damage;
	int32 race;         ///< the target's, when the hit was calculated
	int32 class_;
	std::vector<s_skill_lua_action> actions;
};

void do_init_skill_lua();
void do_final_skill_lua();

/// Put the Lua wrapper on the skills that have hooks. Called whenever the
/// skill database has (re)built its skill classes.
void skill_lua_attach();

/// Run a skill's on_hit hook, if it has one. The actions it asks for are
/// returned rather than done, so they happen after the hit is dealt.
std::unique_ptr<s_skill_lua_hit> skill_lua_on_hit(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, int64 damage, int32 attack_type);

/// Apply what an on_hit hook asked for. Does nothing for an empty hit.
void skill_lua_apply(std::unique_ptr<s_skill_lua_hit>& hit);

/// Item hooks: `src` hit `target`, with skill_id 0 for a normal attack. Called
/// from skill_additional_effect and skill_counter_additional_effect, beside
/// the autospells; what the hooks ask for is applied before they return.
void skill_lua_item_attack(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, int32 attack_type);
void skill_lua_item_hit_taken(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, int32 attack_type);

#endif /* SKILL_LUA_HPP */
