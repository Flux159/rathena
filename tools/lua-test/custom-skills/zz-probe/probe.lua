-- zz-probe: exercises the queries, actions and hooks the example mod doesn't
-- use, through the probe skills in test_skills.yml. Every line it
-- logs starts with V| for the test to read.
local function V(...)
  local parts = { "V|" }
  for _, v in ipairs({ ... }) do parts[#parts + 1] = tostring(v) end
  log(table.concat(parts, " "))
end

local fb = skill_info("MG_FIREBOLT", 5)
V("skill_info", fb.hits, fb.type, fb.target, fb.max_level)

local function find_mob(c, mob_id, radius)
  for _, u in ipairs(units_around(c.caster, radius or 10, { kind = "mob" })) do
    if u.mob_id == mob_id then return u end
  end
end

local function count_mobs(list, ...)
  local want, n = { ... }, 0
  for _, u in ipairs(list) do
    for _, id in ipairs(want) do if u.mob_id == id then n = n + 1 end end
  end
  return n
end

local t0 = now()
local veto_armed = false
local watch_acted = false
local floor_logged = false

skill("TST_PROBE", {
  custom = true,
  on_cast = function(c)
    local lv = c.skill_lv
    if lv == 1 then
      local push = find_mob(c, 25994)
      V("around", count_mobs(units_around(c.caster, 3, { enemy_of = c.caster, kind = "mob" }), 25994, 25990))
      V("line", push and count_mobs(units_in_line(c.caster, push, 0, { kind = "mob" }), 25994, 25990) or -1)
      V("me", c.caster.weapon_type, c.caster.char_id, c.caster.aspd, c.caster.weapon_id)
      local w = c.caster:equipment("weapon")
      V("weapon", w and w.id or 0, w and w.refine or -1, w and w.creator or -1)
      V("now", now() - t0 > 0)
      c:status("SC_BLESSING", 10000, 60000, 10, "caster")
      c:status("SC_SKILLRATE_UP", 10000, 60000, { val1 = skill_info("MO_TRIPLEATTACK").id, val2 = 50 }, "caster")
      if push then
        c:status("SC_CURSE", 2500, 3000, 1, push)
        c:status("SC_STUN", 10000, 3000, 1, push)
        c:knockback(push, 2)
      end
      c:spheres(3, "caster")
      c:give_item("RED_POTION", 3)
      c:take_item("RED_POTION", 2)
      c:spend(100, 10)
      c:mount("falcon", true)
      c:copy_skill("MG_COLDBOLT", 3)
      c:place_unit("MG_FIREWALL", 1, c.caster.x, c.caster.y - 3)
      c:summon("PORING", { duration = 60000 })
    elseif lv == 2 then
      local bl = c.caster:status_info("SC_BLESSING")
      local sr = c.caster:status_info("SC_SKILLRATE_UP")
      V("spheres", c.caster.spheres)
      V("blessing", bl and bl.val1 or 0, sr and sr.val2 or 0)
      V("copied", c.caster.copied_skill, c.caster.copied_skill_lv)
      V("falcon", c.caster.falcon)
      local fw = c.caster:ground_units("MG_FIREWALL")
      V("firewall", #fw, fw[1] and #fw[1].cells or 0)
      V("firewall_xy", fw[1] ~= nil and type(fw[1].x) == "number" and type(fw[1].y) == "number", fw.x == nil)
      local push = find_mob(c, 25994)
      V("stun", push ~= nil and push:has_status("SC_STUN"))
      local mine = 0
      for _, u in ipairs(units_around(c.caster, 10, { kind = "mob" })) do
        if u.master_id == c.caster.id then mine = mine + 1; c:expire(u, 500) end
      end
      V("summoned", mine)
      V("ground_around", #ground_units_around(c.caster, 5), #ground_units_around(c.caster, 5, { enemy_of = c.caster }))
      c:status_val("SC_BLESSING", 1, 5, "caster")
      if fw[1] then c:end_unit(fw[1].group) end
      c:mount("falcon", false)
      c:spheres(-1, "caster")
    elseif lv == 3 then
      local bl = c.caster:status_info("SC_BLESSING")
      V("after", bl and bl.val1 or 0, #c.caster:ground_units("MG_FIREWALL"), c.caster.falcon, c.caster.spheres)
      local mine = 0
      for _, u in ipairs(units_around(c.caster, 10, { kind = "mob" })) do
        if u.master_id == c.caster.id then mine = mine + 1 end
      end
      V("summoned_after", mine)
      c:end_status("SC_BLESSING", "caster")
      c:slide("caster", c.caster.x - 2, c.caster.y)
    elseif lv == 4 then
      V("ended", c.caster:status_info("SC_BLESSING") == nil)
      for _, s in ipairs(c.caster:statuses()) do
        if s.name == "TST_FLEE" then V("statuses_mod", s.mod, s.stacks) end
      end
      c:apply_status("TST_DISCOUNT", {}, "caster")
      c:apply_status("TST_LOCK", {}, "caster")
      c:apply_status("TST_CRIT", {}, "caster")
      c:apply_status("TST_WATCH", {}, "caster")
      c:apply_status("TST_HASTE", {}, "caster")
    elseif lv == 5 then
      -- c:attack: a fixed 1000 at power 50 on the dummy, a fixed 10000 on the weakling
      local d = find_mob(c, 25990, 5)
      local w = find_mob(c, 25991, 5)
      if d then c:attack("TST_SPLASH_STRIKE", 1, d, { power = 50 }) end
      if w then c:attack("TST_SPLASH_STRIKE", 1, w) end
    elseif lv == 6 then
      local w = find_mob(c, 25991, 5)
      if w then c:apply_status("TST_SECOND_LIFE", {}, w) end
    elseif lv == 7 then
      local w = find_mob(c, 25991, 5)
      if w then c:attack("TST_SPLASH_STRIKE", 1, w) end
    elseif lv == 8 then
      local sword = c.caster:find_item("SWORD")
      local o = sword and sword.options[1]
      V("crafted", sword ~= nil, sword ~= nil and sword.creator == c.caster.char_id, o and o.id or 0, o and o.value or 0)
      local push = find_mob(c, 25994, 30)
      V("teleport_target", push and push.id or 0, push and push.x or -1, push and push.y or -1)
      if push then c:teleport(push) end
      c:remove_status("TST_LOCK", "caster")
      c:remove_status("TST_DISCOUNT", "caster")
      c:remove_status("TST_CRIT", "caster")
      c:remove_status("TST_WATCH", "caster")
      c:remove_status("TST_HASTE", "caster")
    elseif lv == 9 then
      veto_armed = true
      c:summon("TEST_DUMMY", { neutral = true, duration = 60000, x = c.caster.x + 4, y = c.caster.y + 4 })
      V("zeny_cost", skill_info("MC_MAMMONITE", 10).zeny)
      c:itemskill("MG_SOULSTRIKE", 3)
    elseif lv == 10 then
      local neutral, mine = 0, 0
      for _, u in ipairs(units_around({ map = c.caster.map, x = c.caster.x + 4, y = c.caster.y + 4 }, 1, { kind = "mob" })) do
        if u.mob_id == 25990 then
          if u.master_id == 0 then neutral = neutral + 1 else mine = mine + 1 end
          c:expire(u, 0)
        end
      end
      V("neutral", neutral, mine)
    end
  end,
})

-- before_cast returning false: the use is paid for, its effect does not
-- happen (the probe's level 9 arms it once).
skill("TST_SUPPORT_BUFF", {
  before_cast = function(c)
    if veto_armed then
      veto_armed = false
      V("vetoed", c.target ~= nil and c.target.name or "none")
      return false
    end
  end,
})

-- A passive with the hooks passives have newly.
skill("TST_STATS_PASSIVE", {
  priority = 10,
  stats = function(c, s)
    s.hit = s.hit + 7; s.amotion = 800; s.maxhp = s.maxhp + 1000
    if not floor_logged then floor_logged = true; V("amotion_min", s.amotion_min) end
  end,
  damage_dealt = function(c, damage)
    if c.attack_skill == "TST_GROUND_TICK" then V("tick_hit", c.target.id, damage) end
    if c.attack_skill == "TST_SPLASH_STRIKE" and c.target.kind == "mob" then
      V("dealt", c.target.mob_id, "hits", c.hits, "critical", c.critical, "left", c.damage_left, "targets", c.targets, "damage", damage)
      if c.target.mob_id == 25990 then return 1000 end
      if c.target.mob_id == 25991 then return 10000 end
    end
  end,
  on_attack = function(c) if c.attack_skill_id == 0 then V("auto", c.critical) end end,
  craft = function(c, rate) V("craft", c.item_name, c.craft_skill, rate == c.base); return 100000 end,
  on_craft = function(c, item)
    V("on_craft", item.name, item.amount)
    item.options = { { id = "RDMOPT_VAR_MAXHPAMOUNT", value = 50 } }
  end,
  on_use = function(c) V("passive_use", c.item.name) end,
})

skill("TST_SPLASH_STRIKE", {
  priority = 9,
  before_cast = function(c) V("before_cast", c.target ~= nil and c.target.kind or "none") end,
  hits = function(c, stock) return 3 end,
  critical = function(c, stock) return true end,
  on_hit = function(c) V("tst_strike", "critical", c.critical) end,
})

status("TST_DISCOUNT", { duration = 60000, cost = function(c, cost) if c.used_skill == "TST_SPLASH_STRIKE" then cost.sp = cost.sp // 2 end end })
status("TST_LOCK", { duration = 60000, can_cast = function(c) if c.used_skill == "TST_SELF_BUFF" then return false, "Locked" end end })
status("TST_CRIT", { duration = 60000, critical = function(c, stock) if c.used_skill_id == 0 then return true end end })
status("TST_WATCH", {
  duration = 60000,
  on_attack = function(c)
    V("watch_attack", c.attack_skill_id, c.critical, c.connected)
    -- c.damage and c.status_name are fields; c:hurt() and c:status() still work
    if c.attack_skill_id == 0 and not watch_acted then
      watch_acted = true
      c:status("SC_ENDURE", 10000, 30000, 1, "caster")
      c:hurt(1, "target")
      V("watch_actions", c.status_name, type(c.damage))
    end
  end,
  on_skill = function(c) V("watch_skill", c.used_skill) end,
  on_kill = function(c) V("watch_kill", c.target.mob_id, c.damage, c.overkill, c.attack_skill) end,
})
status("TST_HASTE", { duration = 60000, stats = function(c, s) s.amotion = s.amotion - 50 end })
status("TST_SECOND_LIFE", { duration = 60000, on_death = function(c) V("second_life", c.unit.mob_id); c:survive() end })
status("TST_OVERFLOW", { duration = 5000, on_start = function(c) V("overflow", c.unit.name) end })

-- A heal hook that acts.
skill("AL_HEAL", { heal = function(c, stock) V("heal", stock > 0); c:apply_status("TST_OVERFLOW", {}, c.target) end })

-- Server statuses: the chance they land, and every status's start.
status("SC_CURSE", { chance = function(c, rate) V("curse_rate", rate, c.status_name) end })
status("SC_STUN", { chance = function(c, rate) if c.unit.mob_id == 25994 then return 0 end end })
status("*", { on_start = function(c) if c.status_name == "SC_BLESSING" then V("any_start", c.status_name, c.by_skill, c.debuff) end end })

-- An item that is used.
item("RED_POTION", { on_use = function(c) V("used", c.item.name, c.item.creator, c.caster.name) end })

-- c:itemskill: a skill the player does not have, offered once.
skill("MG_SOULSTRIKE", { on_hit = function(c) V("itemskill_hit", c.skill_lv, c.caster.name) end })

-- before_cast once per use: Counter Slash calls its own castend again.
skill("GC_COUNTERSLASH", { before_cast = function(c) V("cs_before", c.skill_lv) end })

-- A server status's on_end sees the values it had.
status("SC_BLESSING", { on_end = function(c) V("blessing_end", c.val1, c.status_name) end })

-- TST_PROBE3: battle_config, the element, size and DEF tables, a cooldown
-- started and read, and a cost hook that sees where the use goes.
local cost_seen = {}
local keep_starts = 0

skill("TST_PROBE3", {
  custom = true,
  on_cast = function(c)
    local lv = c.skill_lv
    if lv == 1 then
      V("battle_config", battle_config("max_parameter"), battle_config("no_such_setting"))
      V("element_rate", element_rate(const("ELE_FIRE"), const("ELE_EARTH"), 1),
        element_rate(const("ELE_WATER"), const("ELE_FIRE"), 2),
        element_rate(const("ELE_HOLY"), const("ELE_UNDEAD"), 1))
      local d = find_mob(c, 25990)
      V("element_rate_unit", d and element_rate(const("ELE_FIRE"), d) or -1, d and d.element or -1, d and d.element_level or -1)
      V("size_rate", size_rate(const("W_KNUCKLE"), 2), size_rate(const("W_DAGGER"), 2), size_rate(const("W_2HSWORD"), 1), size_rate(const("W_FIST"), 2))
      V("def_rate", def_rate(0), def_rate(50), def_rate(100))
      c:cooldown("AL_HEAL", 5000, "caster")
    elseif lv == 2 then
      V("cooldown_left", c.caster:cooldown_left("AL_HEAL"))
      c:cooldown("AL_HEAL", 0, "caster")
    elseif lv == 3 then
      V("cooldown_cleared", c.caster:cooldown_left("AL_HEAL"))
    elseif lv == 4 then
      -- A ground skill's later hits at the power of what placed it: one
      -- placed as it is, one at 50 and one cast at 200, each on a dummy.
      -- The dummies 2, 4 and 6 cells east.
      local at = {}
      for _, u in ipairs(units_around(c.caster, 6, { kind = "mob" })) do
        if u.mob_id == 25990 and u.y == c.caster.y then at[u.x - c.caster.x] = u end
      end
      local plain, half, double = at[2], at[4], at[6]
      V("tick_units", plain and plain.id or 0, half and half.id or 0, double and double.id or 0)
      if plain and half and double then
        c:place_unit("TST_GROUND_TICK", 1, plain.x, plain.y)
        c:place_unit("TST_GROUND_TICK", 1, half.x, half.y, { power = 50 })
        c:cast("TST_GROUND_TICK", 1, double, { power = 200 })
      end
    elseif lv == 5 then
      V("range", c.caster:skill_range("TST_RANGE_STRIKE"))
    elseif lv == 6 then
      c:apply_status("TST_LONG_REACH", {}, "caster")
    elseif lv == 7 then
      V("range_long", c.caster:skill_range("TST_RANGE_STRIKE"))
    elseif lv == 8 then
      c:apply_status("TST_KEEP", { stacks = 3, duration = 40000 }, "caster")
      c:apply_status("TST_GONE", {}, "caster")
    elseif lv == 9 then
      local left = -1
      for _, st in ipairs(c.caster:statuses()) do
        if st.name == "TST_KEEP" then left = st.remaining end
      end
      V("kept", c.caster:status_stacks("TST_KEEP"), left, c.caster:has_status("TST_GONE"), keep_starts)
    elseif lv == 10 then
      c:remove_status("TST_KEEP", "caster")
    end
  end,
})

-- The ground skill of the power test, as its skill_db entry says.
skill("TST_GROUND_TICK", { custom = true })

-- A strike with Range 3: its own range hook adds 1, and TST_LONG_REACH 4
-- more for whoever has it.
skill("TST_RANGE_STRIKE", {
  custom = true,
  range = function(c, range) return range + 1 end,
})

status("TST_LONG_REACH", {
  duration = 60000,
  range = function(c, range)
    if c.used_skill == "TST_RANGE_STRIKE" then return range + 4 end
  end,
})

-- Kept over a logout, with its stacks, time left and stats; TST_GONE isn't.
status("TST_KEEP", {
  icon = "EFST_ATKER_BLOOD",
  duration = 60000,
  max_stacks = 5,
  persist = true,
  stats = function(c, s) s.flee = s.flee + 11 * c.stacks end,
  on_start = function(c) keep_starts = keep_starts + 1 end,
})

status("TST_GONE", { duration = 60000 })

skill("TST_COST_STRIKE", {
  custom = true,
  cost = function(c, cost)
    if c.target ~= nil then
      cost_seen.target = c.target.mob_id or -1
      if c.target.mob_id == 25990 then cost.sp = 1 end
    end
  end,
  on_cast = function(c) V("cost_target", cost_seen.target or 0) end,
})

skill("TST_GROUND_BURST", {
  cost = function(c, cost)
    if c.x ~= nil then cost_seen.cell = c.x .. " " .. c.y end
  end,
  on_cast = function(c) V("cost_cell", cost_seen.cell or "none") end,
})
