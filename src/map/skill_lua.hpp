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
 *     priority = 3,                           -- 0..10, lower runs first; 5 is the default
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
 * applied once the hit has been dealt. Nothing a script does can reach the
 * disk, the network or the server's memory; an error or a runaway loop
 * switches that one hook off and says so in the log.
 *
 * Two mods may hook the same part of the same skill: every registered hook
 * runs, in ascending priority order (ties broken by mod load order). For
 * ratio, hit and element each hook sees what the previous one returned as
 * `stock`, so the chain composes. For on_hit each hook queues its own
 * actions (drain, heal, status, polymorph) and they are applied together
 * once the hit is dealt. A mod that redeclares a hook replaces its own
 * previous registration -- a mod never fights itself.
 *
 * Equipped items can also run hooks, through a separate registration keyed
 * by item AegisName:
 *
 *   item("VORPAL_BLADE", {
 *     priority = 5,                          -- same 0..10 chain as skill()
 *     on_attack = function(c)                -- the wearer swung at something
 *       if c.connected and c.critical then c:heal(50, 0) end
 *     end,
 *     on_hit_taken = function(c) ... end,    -- something swung at the wearer
 *   })
 *
 * on_attack fires for every attack by any unit that has the item equipped
 * (a card counts when it is slotted in something equipped)
 * -- normal attacks and skill attacks, hits and misses -- after the damage
 * calculation is finalized. on_hit_taken fires the same way, but keyed on
 * the defender's equipment. In both hooks `c` carries the full outcome:
 * `c.connected` (did damage apply), `c.critical`, `c.damage`, `c.element`,
 * `c.skill_id` (0 for a normal attack), plus the usual `c.caster` and
 * `c.target` -- which also now expose each equip slot's item id, so a hook
 * can gate on gear without re-registering.
 *
 * Besides drain, heal, status and polymorph, any hook may ask for
 * c:cast(skill, level, who): a skill cast the way bAutoSpell casts one, at
 * "target" (default) or "caster". No Lua hook runs during that cast, so an
 * item that casts a bolt on every hit cannot trigger itself in a loop. A
 * ground skill's later hits (Storm Gust's ticks) are not part of the cast
 * and do run hooks: an on_attack that casts one should check c.skill_id.
 *
 * item() hooks chain the same way skill() hooks do: multiple mods can
 * register for the same item, and each runs in priority order, queuing
 * actions into the same pending hit. A unit without the item equipped --
 * mobs, homunculi, players who took it off -- does not fire the hook.
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

/// Fire every hook interested in this attack:
///  - a skill()'s on_hit, if skill_id is set and the skill is hooked;
///  - item()'s on_attack, for each item equipped on `src`;
///  - item()'s on_hit_taken, for each item equipped on `target`.
/// Each hook may queue drain/heal/status/polymorph actions; they are
/// returned together rather than applied here, so they happen after the
/// hit is dealt. Returns nullptr when no action was queued.
///
/// `dmg_lv` is the attack's connection result (ATK_DEF means it landed),
/// `critical` is true for a critical, `element` is the final attack element
/// (ELE_*), and `attack_type` carries the usual BF_WEAPON/BF_MAGIC/BF_MISC
/// mask. skill_id is 0 for a normal weapon attack.
std::unique_ptr<s_skill_lua_hit> skill_lua_on_damage(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, int64 damage, int32 attack_type, int32 dmg_lv, bool critical, int32 element);

/// Apply what the hooks asked for. Does nothing for an empty hit.
void skill_lua_apply(std::unique_ptr<s_skill_lua_hit>& hit);

#endif /* SKILL_LUA_HPP */
