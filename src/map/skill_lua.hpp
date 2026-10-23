// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef SKILL_LUA_HPP
#define SKILL_LUA_HPP

#include <bitset>
#include <memory>
#include <vector>

#include <common/cbasetypes.hpp>
#include <common/mmo.hpp>

/**
 * Skill, item and player hooks written in Lua.
 *
 * Every hook, and what it decides (each is described below):
 *
 *   skill(name, {...})   ratio      the skill's damage ratio
 *                        hit        its accuracy
 *                        element    its element
 *                        on_hit     actions after each hit (drain, heal, status, ...)
 *                        on_steal   which drop TF_STEAL takes
 *                        level      the base level its formula reads for a player
 *                        job_level  the job level its formula reads for a player
 *                        heal       what AL_HEAL, AB_HIGHNESSHEAL or AB_CHEAL restores
 *   item(aegis, {...})   on_attack     the wearer attacked
 *                        on_hit_taken  the wearer was attacked
 *   player({...})        base_stat  what a player's base stat counts for
 *                        cast_dex   the DEX the cast-time and skill-delay formulas read
 *
 * A hook is off until a mod registers it; with none registered the server
 * behaves as stock rAthena. Server extensions, the other way a mod switches
 * behaviour on, are in doc/extensions.md.
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
 * One more skill hook, only meaningful on TF_STEAL:
 *
 *   skill("TF_STEAL", {
 *     on_steal = function(c)
 *       for _, drop in ipairs(c.drops) do
 *         if drop.is_card and c:chance(drop.rate) then
 *           return drop                       -- give this drop
 *         end
 *       end
 *       return nil                            -- fall through to stock
 *     end,
 *   })
 *
 * on_steal runs from pc_steal_item after the DEX/level success check, with
 * c.drops holding the mob's stealable drops ({nameid, rate, is_card,
 * is_equip, is_etc, type}). The hook may return one of those entries (or any
 * table with a matching `nameid`, or the nameid as an integer) to force that
 * drop; returning nil lets the stock slot-order loop run as if no hook
 * existed. First non-nil return in the priority chain wins.
 *
 * Two more hooks decide which level a skill's formula reads:
 *
 *   skill("SR_GATEOFHELL", {
 *     level     = function(c, stock) return math.min(stock, 99) end,
 *     job_level = function(c, stock) return 70 end,
 *   })
 *
 * They run wherever a formula reads a player's base or job level for that
 * skill's output, also for skills with no class of their own. level:
 * RK_DRAGONBREATH, RK_DRAGONBREATH_WATER, RK_ENCHANTBLADE,
 * NC_SELFDESTRUCTION, SR_GATEOFHELL, SR_CRESCENTELBOW, AB_RENOVATIO,
 * LG_EXEEDBREAK, LG_PRESTIGE (its magic evasion and DEF), WL_FREEZE_SP (how
 * many points the spellbooks hold), WM_LULLABY_DEEPSLEEP (its chance),
 * OB_ZANGETSU, KO_JYUMONJIKIRI, SJ_GRAVITYCONTROL, the
 * Elemental Master's EM_EL_ skills (the master's level), and in pre-renewal
 * AL_HEAL, AB_HIGHNESSHEAL, PR_TURNUNDEAD; AL_DEMONBANE, SL_SMA,
 * TK_JUMPKICK, SG_SUN_ANGER, SG_MOON_ANGER, SG_STAR_ANGER. job_level:
 * RL_QD_SHOT, LG_EXEEDBREAK, MI_RUSH_WINDMILL, WA_MOONLIT_SERENADE,
 * WM_LULLABY_DEEPSLEEP.
 * `stock` is the real level. c.caster is the player; there is
 * no c.target and c.skill_lv is 0, so a hook that reads c.target fails and is
 * switched off. The result is kept between 0 and the server's level limit.
 * Monsters' levels are never asked about.
 *
 * And one decides what a heal skill restores:
 *
 *   skill("AB_CHEAL", {
 *     heal = function(c, stock) return c.base * 85 // 100 end,
 *   })
 *
 * heal runs for AL_HEAL, AB_HIGHNESSHEAL and AB_CHEAL when they heal (not
 * when Heal damages an Undead). `stock` is the amount the server would
 * restore; c.base is that amount before the skill's own bonus, which only
 * AB_CHEAL has (2.5 % per party member). c.target is the unit being healed.
 *
 * For a player, c.caster and c.target also carry base_str, base_agi,
 * base_vit, base_int, base_dex and base_luk: the stats as the player raised
 * them, where str, agi and the rest are the totals in use (with job, gear
 * and status bonuses). A hook can so reward what was invested in a stat:
 *
 *   heal = function(c, stock)
 *     return stock * (100 + math.max(0, c.caster.base_int - 99)) // 100
 *   end
 *
 * Player hooks are not tied to a skill or an item; they run for every player:
 *
 *   player({
 *     priority  = 5,
 *     base_stat = function(c, stock)          -- c.stat: "str", "agi", ... "luk"
 *       if stock <= 99 then return stock end
 *       return 99 + (stock - 99) // 3         -- above 99 a point counts for a third
 *     end,
 *     cast_dex  = function(c, stock)          -- stock: the DEX in use, bonuses included
 *       return stock
 *     end,
 *   })
 *
 * base_stat decides what a stat as the player raised it (c.caster.base_str
 * and so on, `stock` here) counts for wherever the server uses it: the
 * status calculation, the status window's bonus column, the weight limit,
 * Marionette Control, Beast Bane with the Hunter spirit, and the Vacuum
 * Extreme and Bind Trap resistances. Job, equipment and status bonuses are
 * added on top afterwards; allocation, resets and saving keep the raised
 * value. cast_dex decides the DEX the cast-time formula reads for a player
 * (pre-renewal's cast time; renewal's variable cast time, beside INT), and the
 * skill-delay one with delay_dependon_dex. Both run while the
 * player's status is being worked out, so c.caster carries only id, name,
 * job, level, job_level and base_str .. base_luk -- no totals, no HP or SP --
 * and there is no c.target. The results are kept between 0 and 65535.
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
 *
 * Skills a mod adds, and skills it reworks:
 *
 *   skill("TC_SHADOW_STRIKE", {
 *     custom   = true,                        -- a skill of the mod's own (no server class)
 *     ratio    = function(c, stock) ... end,  -- its damage, as for any skill
 *     on_cast  = function(c) ... end,         -- once per use, after its effect
 *     can_cast = function(c) return c.caster:has_status("TC_WINDOW"), "Not now" end,
 *     cost     = function(c, cost) cost.sp = cost.sp // 2 end,
 *   })
 *
 * custom = true gives a skill that has no class of its own one that does what
 * its skill_db entry says: an Attack skill hits its target (SplashArea makes
 * it an area), a Self or Support skill starts its Status (a Self skill with a
 * SplashArea that deals damage hits around the caster), a Ground skill places
 * its Unit, or hits the area when it has none. replace = true does the same
 * for a skill that has a class, dropping the stock behaviour (a reused ID).
 * stock_checks = false skips the checks skill.cpp makes for that skill alone
 * before it may be used (Spell Fist only while casting a bolt) and on its
 * target (pre-renewal's Back Stab only from behind); its skill_db
 * requirements still apply.
 * on_cast runs for any skill a unit uses (c.x, c.y: where); can_cast refuses
 * a player's use by returning false (and a message); cost changes sp, hp, ap,
 * zeny, spiritball, ammo_qty and items ({ item = "AEGIS", amount = n }) of
 * what the skill needs (ammo_qty: the check only; the server uses up the
 * skill's own AmmoAmount). cost must not change anything else: the server
 * asks several times per use. When it asks at the end of a cast, the use's
 * target is known (c.target, or c.x and c.y for a ground skill); its first
 * check, before the cast, has neither. range returns how far the skill
 * reaches for c.caster (`range` in: its Range with Vulture's Eye and the
 * like); unit:skill_range(skill, lv) reads it. A player's client learns
 * ranges with its skill list, which a mod status with a range hook sends
 * again when it starts and ends.
 *
 * Hooks that fire for every player who has learned the skill, whatever the
 * player is doing (a passive's effect; c.skill_lv is the learned level):
 *
 *   bonus        = function(c) c:bonus("bCritical", 2 * c.skill_lv) end,
 *   on_attack    = function(c) ... end,  -- the learner attacked (hit or not)
 *   on_hit_taken = function(c) ... end,  -- the learner was attacked
 *   damage_dealt = function(c, damage) return damage end,  -- final damage out
 *   damage_taken = function(c, damage) return damage end,  -- final damage in
 *   on_kill      = function(c) ... end,  -- the learner killed c.target
 *   on_death     = function(c) c:survive() end,  -- the learner is dying
 *   on_skill     = function(c) ... end,  -- the learner used c.used_skill
 *   tick         = function(c) ... end,  -- every second, while online and alive
 *
 * bonus runs whenever the player's stats are calculated and takes item bonuses
 * by name: c:bonus("bStr", 5), c:bonus2("bSkillAtk", "AS_SONICBLOW", 20) and
 * so on up to bonus5, as in doc/item_bonus.txt; c:grant_skill(name, level)
 * lends a skill the way a card does. In hooks about an attack, c.attack_skill,
 * c.attack_skill_id and c.attack_skill_lv are the attack's skill (0 for a
 * normal attack), c.ranged, c.missed and c.perfect_dodge what happened.
 *
 * Statuses a mod adds -- buffs, debuffs, marks, counters -- with stacks:
 *
 *   status("TC_TOXIN", {
 *     icon = "EFST_TOXIN", duration = 10000, max_stacks = 10,
 *     max_stacks_boss = 3, max_stacks_player = 3, per_source = true,
 *     decay = { every = 3000, stacks = 1 },
 *     damage_taken = function(c, damage)       -- c.unit has the status
 *       if not c.source or c.caster.id ~= c.source.id then return nil end
 *       return damage * (100 + 3 * c.stacks) // 100
 *     end,
 *   })
 *
 * A status is put on and taken off by the actions c:apply_status(name,
 * { stacks = 1, duration = ms, source = unit }, who) and c:remove_status(name,
 * who, stacks, source). Its hooks: bonus (players), stats (any unit: a table
 * of def, mdef, hit, flee, flee2, cri, speed, batk, atk, matk_min, matk_max to
 * change), damage_dealt, damage_taken, on_attack, on_hit_taken, on_start,
 * on_end (c.reason: "expired", "decayed", "removed" or "death") and tick
 * (every second). They see c.unit, c.source (who applied it; nil once that
 * unit is gone), c.stacks and c.remaining (ms; -1 for no end). persist =
 * true keeps a player's over a logout and a restart, with its stacks and
 * time left, in the char store of the mod that defined it (_statuses); it
 * goes on when they are back, without on_start.
 *
 * icon is a status icon the client has ("EFST_TOXIN", or its number), or one
 * of a mod's own: a number from EFST_MOD_FIRST to EFST_MOD_LAST (20000 to
 * 22999), which the server sends as it is, for players and monsters alike.
 * The client knows such an icon only from the mod's client tables (efstids,
 * stateiconimginfo, stateiconinfo); a mod keeps to a block of the range.
 *
 * status() with a server status's name
 * (SC_HIDING) takes on_start and on_end only: on_start may return a new
 * duration in ms, or false to refuse the status.
 *
 * Units gain unit:skill_lv(name), unit:status_stacks(name, source),
 * unit:party(range) and unit:in_skill_area(skill, owner); has_status() also
 * knows a mod's statuses. Every hook sees c.pvp, c.gvg and c.bg. Further
 * actions: c:cut_delay(ms, who), c:cut_cooldown(skill, ms, who),
 * c:give_item(item, amount, who), c:give_zeny(amount, who), c:hurt(hp,
 * who). Everything a hook asks for happens after the hook has run; for
 * on_kill, on_death and a status's on_start and on_end, a moment later.
 *
 * The rest of the toolkit, for reworking whole classes:
 *
 *  - More hooks on a skill: before_cast (once per use, before its effect;
 *    c.caster still stands where it cast from; false stops the effect, the
 *    use still paid for), hits (how many hits, the stock count in and out),
 *    critical (true or false for a weapon hit).
 *    A heal hook and an on_steal hook may act.
 *  - More passive hooks: stats (as a status's), craft (the chance, out of
 *    10000, to make c.item_id with c.craft_skill; c.base is the server's own),
 *    on_craft (the item made, a table whose amount and options the hook may
 *    change) and on_use (c.item: a consumable being used, with its creator).
 *  - A mod's status also takes on_kill, on_death (c:survive() keeps any unit
 *    alive), on_skill, cost, can_cast, range, critical, craft, on_craft and
 *    on_use: for every skill its holder uses (c.used_skill) and everything
 *    it does.
 *  - A server status takes chance (out of 10000, before resistance), and
 *    status("*", { ... }) hooks every server status at once; their hooks see
 *    c.status_name, c.debuff and c.by_skill.
 *  - item(name, { on_use = ... }) for a consumable.
 *  - Queries: now(), skill_info(skill, lv), units_around(where, radius,
 *    filter), units_in_line(from, to, width, filter), ground_units_around(
 *    where, radius, filter), battle_config(name), and the server's tables:
 *    element_rate(element, unit or element, level), size_rate(weapon_type,
 *    size), def_rate(def); unit fields (char_id, weapon_type, spheres,
 *    amotion, aspd, master_id, riding, copied_skill, element_level, ...) and
 *    methods status_info, statuses, ground_units, equipment, count_item,
 *    find_item, mob_skills, devotees, homunculus, master, cooldown_left,
 *    skill_range.
 *  - Actions: status (now with vals and the chance out of 10000, as
 *    documented), end_status, status_val, cast (opts by, x, y, power; the
 *    power reaches what the cast places on the ground), attack, spheres,
 *    slide, knockback, interrupt, summon (opts neutral), set_target,
 *    take_item, copy_skill, itemskill, spend, place_unit (opts duration, by,
 *    power), end_unit, expire, produce_menu, mount, teleport, cooldown; heal
 *    (opts force) defaults to the caster, as documented; give_item (opts
 *    floor).
 *  - Context: c.blocked and c.autocast on attacks; c.hits, c.damage_left
 *    (a damage hook's second result sets the left hand's part) and c.targets
 *    on damage; c.damage, c.overkill and c.element on on_kill and on_death.
 *
 * docs/MODDING.md in the app is the reference for every one of these.
 */

struct block_list;
class map_session_data;

/// The status icons kept for mods' own statuses: the server sends these
/// numbers as they are, and a mod's client tables give them names, pictures
/// and tooltips. rAthena's own icons stay below EFST_MAX.
constexpr int32 EFST_MOD_FIRST = 20000;
constexpr int32 EFST_MOD_LAST = 22999;
struct Damage;
struct s_skill_condition;

enum e_skill_lua_action : uint8 {
	SKILL_LUA_DRAIN = 0,
	SKILL_LUA_HEAL,
	SKILL_LUA_STATUS,
	SKILL_LUA_POLYMORPH,
	SKILL_LUA_CAST,
	SKILL_LUA_APPLY_STATUS,   ///< a mod's status on
	SKILL_LUA_REMOVE_STATUS,  ///< a mod's status off
	SKILL_LUA_CUT_DELAY,      ///< shorten the unit's after-cast delay
	SKILL_LUA_CUT_COOLDOWN,   ///< shorten a player's cooldown of one skill
	SKILL_LUA_GIVE_ITEM,      ///< an item for a player
	SKILL_LUA_DAMAGE,         ///< HP lost, without defence or element
	SKILL_LUA_END_STATUS,     ///< a server status ends
	SKILL_LUA_STATUS_VAL,     ///< a value of a running server status changes
	SKILL_LUA_ATTACK,         ///< one hit of a skill, nothing paid
	SKILL_LUA_SPHERES,        ///< spirit spheres gained or lost
	SKILL_LUA_SLIDE,          ///< moved to a cell at once
	SKILL_LUA_KNOCKBACK,      ///< pushed away from a cell
	SKILL_LUA_INTERRUPT,      ///< a cast cancelled
	SKILL_LUA_SUMMON,         ///< monsters that serve the summoner, or on nobody's side
	SKILL_LUA_SET_TARGET,     ///< a monster turns on a unit
	SKILL_LUA_TAKE_ITEM,      ///< items taken from a player
	SKILL_LUA_COPY_SKILL,     ///< a skill copied into Plagiarism's or Reproduce's slot
	SKILL_LUA_ITEMSKILL,      ///< one use of a skill for a player, as a scroll gives
	SKILL_LUA_SPEND,          ///< HP and SP paid, without a hit
	SKILL_LUA_PLACE_UNIT,     ///< a ground skill placed
	SKILL_LUA_END_UNIT,       ///< a ground skill removed
	SKILL_LUA_EXPIRE,         ///< a summoned monster leaves after a while
	SKILL_LUA_PRODUCE_MENU,   ///< the crafting window opens
	SKILL_LUA_MOUNT,          ///< a mount, falcon or cart on or off
	SKILL_LUA_TELEPORT,       ///< to a random cell of the map, as Teleport does
	SKILL_LUA_COOLDOWN,       ///< a skill's cooldown started, as a use would
	SKILL_LUA_GIVE_ZENY,      ///< zeny for a player
};

/// One thing an on_hit hook asked for. Units are kept by id, never by
/// pointer: by the time it is applied, the target may have died.
struct s_skill_lua_action {
	e_skill_lua_action kind;
	int32 unit_id = 0;  ///< who it happens to
	int32 type = 0;     ///< STATUS, END_STATUS, STATUS_VAL: the sc_type; CAST, ATTACK, CUT_COOLDOWN, COOLDOWN, COPY_SKILL, ITEMSKILL, PLACE_UNIT: the skill id; *_STATUS: the mod status; GIVE_ITEM, TAKE_ITEM: the item id; SUMMON: the monster id; END_UNIT: the group id; MOUNT: what (e_skill_lua_mount)
	int32 rate = 0;     ///< STATUS: chance out of 10000; STATUS_VAL: which value (1..4)
	int32 val1 = 0;     ///< STATUS: val1; CAST, ATTACK, COPY_SKILL, ITEMSKILL, PLACE_UNIT: the skill level; *_STATUS: stacks (-1: all); GIVE_ITEM, TAKE_ITEM, SUMMON: amount; GIVE_ZENY: zeny; SPHERES: how many (negative: lost); KNOCKBACK: cells; STATUS_VAL: the value; PRODUCE_MENU: the recipes' item level
	int64 duration = 0; ///< STATUS, APPLY_STATUS, CUT_DELAY, CUT_COOLDOWN, COOLDOWN, SUMMON, PLACE_UNIT, EXPIRE, SPHERES, in milliseconds
	int64 hp = 0, sp = 0;  ///< HEAL, SPEND; DAMAGE: hp
	int32 source_id = 0;  ///< APPLY_STATUS: who applied it; REMOVE_STATUS: only theirs, unless 0
	int32 val2 = 0, val3 = 0, val4 = 0;  ///< STATUS
	int32 by_id = 0;    ///< CAST, ATTACK, PLACE_UNIT, SUMMON, SET_TARGET: who does it, or the unit turned on; 0: the hit's caster
	int16 x = -1, y = -1;  ///< a cell: CAST at it, SLIDE to it, KNOCKBACK away from it, PLACE_UNIT, SUMMON
	int32 power = 100;  ///< CAST, ATTACK: % of the damage dealt
	int32 flag = 0;     ///< HEAL: 1 heals through Berserk; GIVE_ITEM: 1 drops it at the player's feet; COPY_SKILL: 2 Reproduce; MOUNT: 1 on; SPHERES: the most to have
};

/// What c:mount() puts on or takes off.
enum e_skill_lua_mount : uint8 {
	SKILL_LUA_MOUNT_PECO = 0,
	SKILL_LUA_MOUNT_DRAGON,
	SKILL_LUA_MOUNT_WARG,
	SKILL_LUA_MOUNT_MADO,
	SKILL_LUA_MOUNT_FALCON,
	SKILL_LUA_MOUNT_CART,
	SKILL_LUA_MOUNT_MAX
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

/// Give TF_STEAL's on_steal hook a chance to pick which stealable drop the
/// thief gets. Returns the chosen drop's nameid, or 0 to let the stock
/// slot-order loop in pc_steal_item run as normal. Caller must still check
/// that the returned nameid names a stealable entry on the mob.
t_itemid skill_lua_on_steal(struct block_list* src, struct block_list* target, uint16 skill_lv);

/// The base level a skill's formula should read for `bl`: `stock` unless a
/// mod's level hook on that skill says otherwise. Always `stock` when no hook
/// is registered.
int32 skill_lua_level(uint16 skill_id, const block_list* bl, int32 stock);

/// The player() hooks: the value a base stat counts for (`stat` is "str",
/// "agi", "vit", "int", "dex" or "luk"), and the DEX the cast-time and
/// skill-delay formulas read. Both return `stock` when no hook is registered.
class map_session_data;
int32 skill_lua_base_stat(const map_session_data& sd, const char* stat, int32 stock);
int32 skill_lua_cast_dex(const map_session_data& sd, int32 stock);

/// As skill_lua_level(), for the job level and the job_level hook.
int32 skill_lua_job_level(uint16 skill_id, const block_list* bl, int32 stock);

/// The amount a heal skill restores: `stock` unless a mod's heal hook on that
/// skill says otherwise. `base` is the amount before the skill's own bonus
/// (Coluceo Heal's bonus per party member); the hook reads it as c.base.
int32 skill_lua_heal(uint16 skill_id, uint16 skill_lv, const block_list* src, const block_list* target, int32 stock, int32 base);

// The calls below are the whole of what the upstream files carry for mods'
// custom skills and statuses: one line each, the logic is here.

/// status_calc_pc_: bonuses from the bonus hooks of the skills `sd` has
/// learned and of the mod statuses on it.
void skill_lua_bonus(map_session_data* sd);

/// status_calc_bl_main: the stats hooks of the skills a player has learned
/// and of the mod statuses on `bl`. Only the fields it has just recalculated
/// (`flag`, with what they depend on) are changed.
template <size_t N>
void skill_lua_stats(block_list& bl, const std::bitset<N>& flag);

/// battle_calc_attack: mods' damage_dealt and damage_taken hooks, and the
/// power of a c:cast, c:attack or c:place_unit. `count` is
/// battle_calc_attack's own flag: for an area skill whose damage is split,
/// how many targets share it.
void skill_lua_damage(int32 attack_type, block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, Damage& d, int32 count);

/// skill_attack, around its damage: the hit is dealt through `dsrc`, which
/// for a ground skill's later hits is its unit. skill_lua_damage then knows
/// the power the unit was placed with.
class skill_lua_dealt_by {
	block_list* previous_;

public:
	explicit skill_lua_dealt_by(block_list* dsrc);
	~skill_lua_dealt_by();
	skill_lua_dealt_by(const skill_lua_dealt_by&) = delete;
	skill_lua_dealt_by& operator=(const skill_lua_dealt_by&) = delete;
};

/// skill_get_range2: how far `bl` reaches with the skill, `range` as the
/// server has it; mods' range hooks may change it.
int32 skill_lua_range(const block_list* bl, uint16 skill_id, uint16 skill_lv, int32 range);

/// battle_calc_weapon_attack, once it has decided whether the attack is a
/// critical: mods' critical hooks may change that.
void skill_lua_critical(Damage& wd, block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv);

/// skill_castend_id and skill_castend_pos: a unit used a skill (after its
/// effect). `target` is nullptr for a ground skill.
void skill_lua_on_cast(block_list* src, block_list* target, int16 x, int16 y, uint16 skill_id, uint16 skill_lv);

/// skill_check_condition_castbegin: false if a mod's can_cast hook refuses
/// the use; the player has been told why.
bool skill_lua_can_cast(map_session_data& sd, uint16 skill_id, uint16 skill_lv);

/// skill_check_condition_castbegin and _castend, and skill_castend_id_check:
/// false when a mod turns off the server's skill-specific checks for the
/// skill (stock_checks = false), such as Spell Fist's "only while casting a
/// bolt" or pre-renewal Back Stab's "only from behind".
bool skill_lua_stock_checks(uint16 skill_id);

/// skill_get_requirement: mods' cost hooks.
void skill_lua_requirement(map_session_data& sd, uint16 skill_id, uint16 skill_lv, s_skill_condition& req);

/// status_damage: `target` is about to die from `damage` (the whole blow),
/// of which `hp_lost` was the HP it had left. Runs the killer's on_kill and
/// the dying unit's on_death hooks. True when a hook kept the target alive;
/// its HP is then 1.
bool skill_lua_on_kill(block_list* src, block_list* target, uint16 skill_id, int64 damage, int64 hp_lost);

/// status_change_start, before the server rolls `rate` (out of 10000)
/// against the target's resistance: mods' chance hooks may change it.
int32 skill_lua_status_rate(block_list* src, block_list* bl, int32 type, int32 rate, uint8 flag);

/// status_change_start: a stock status is about to start. False refuses it;
/// a hook may change `tick`, its duration. Not asked for a status loaded with
/// the character (SCSTART_LOADED in `flag`).
bool skill_lua_status_start(block_list* src, block_list* bl, int32 type, int32 val1, int32 val2, int32 val3, int32 val4, int32& tick, uint8 flag);

/// status_change_end: a stock status has ended, with the values it had.
void skill_lua_status_end(block_list* bl, int32 type, int32 val1, int32 val2, int32 val3, int32 val4);

/// skill_produce_mix: the chance (out of 10000) that `sd` makes `nameid`
/// with `skill_id`; mods' craft hooks may change it.
int32 skill_lua_craft_rate(map_session_data* sd, uint16 skill_id, t_itemid nameid, int32 rate);

/// skill_produce_mix: `sd` has made `made`, which is about to go into the
/// bag. Mods' on_craft hooks may change how many and its random options.
void skill_lua_crafted(map_session_data* sd, uint16 skill_id, item& made);

/// pc_useitem: `sd` is using `used` (already taken out of the bag), right
/// before its script runs. Runs the item's, the passives' and the statuses'
/// on_use hooks; what they ask for happens right after the script.
void skill_lua_use_item(map_session_data* sd, const item& used);

#endif /* SKILL_LUA_HPP */
