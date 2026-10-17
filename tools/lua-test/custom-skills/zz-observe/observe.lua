-- zz-observe: watches the custom-skills example from a second mod. Every line
-- it logs starts with T| for the test to read.
local function T(...)
  local parts = { "T|" }
  for _, v in ipairs({ ... }) do parts[#parts + 1] = tostring(v) end
  log(table.concat(parts, " "))
end

local function name(unit) return unit and unit.name or "-" end

-- Runs after the example's ratio (priority 9): its result is our stock.
skill("GCR_VENOM_BURST", {
  priority = 9,
  ratio = function(c, stock)
    T("venom_ratio", stock, "stacks", c.target:status_stacks("GCR_TOXIN", c.caster), "lv", c.skill_lv)
  end,
  on_cast = function(c) T("venom_cast", name(c.target)) end,
})

skill("GCR_TOXIC_CLOUD", {
  priority = 9,
  on_hit = function(c) T("cloud_hit", name(c.target), "connected", c.connected, "element", c.element) end,
  on_cast = function(c) T("cloud_cast", c.x, c.y) end,
})

skill("GC_CROSSIMPACT", {
  priority = 0,  -- before the example's: it always runs
  can_cast = function(c) T("ci_can_cast", "window", c.caster:has_status("GCR_SONIC_WINDOW")) end,
})
skill("GC_CROSSIMPACT", {
  priority = 9,
  cost = function(c, cost) T("ci_cost_sp", cost.sp) end,
  on_cast = function(c) T("ci_cast") end,
})

skill("GCR_EXECUTIONER", {
  priority = 9,
  damage_dealt = function(c, damage)
    if c.target.kind == "mob" and c.target.mob_id == 25991 then
      T("exec_dealt", damage, "hp", c.target.hp, "maxhp", c.target.maxhp)
    end
  end,
  on_kill = function(c)
    T("exec_kill", name(c.target), "attack_skill", c.attack_skill)
    c:give_item("RED_POTION", 2, "caster")
  end,
  bonus = function(c) c:grant_skill("AL_HEAL", 3) end,
})

skill("GCR_SHADOW_INSTINCT", {
  priority = 9,
  bonus = function(c) T("instinct_bonus", c.skill_lv); c:bonus("bFlee2", 90) end,  -- near-certain perfect dodge, for the test
  on_hit_taken = function(c)
    if c.perfect_dodge then T("perfect_dodge", name(c.caster)) end
  end,
})

skill("GCR_SHADOW_PACT", {
  priority = 9,
  on_death = function(c) T("on_death", name(c.target)); c:survive() end,
})

skill("GCR_CONTACT_POISON", {
  priority = 9,
  on_skill = function(c) T("on_skill", c.used_skill, c.used_skill_lv) end,
})

status("GCR_TOXIN", {
  priority = 9,
  on_start = function(c) T("toxin_start", name(c.unit), "stacks", c.stacks, "source", name(c.source)) end,
  on_end = function(c) T("toxin_end", name(c.unit), "reason", c.reason, "stacks", c.stacks) end,
  tick = function(c) T("toxin_tick", name(c.unit), "stacks", c.stacks, "hp", c.unit.hp) end,
  damage_taken = function(c, damage) T("toxin_taken", damage, "stacks", c.stacks) end,
})
status("GCR_MOMENTUM", { priority = 9, on_start = function(c) T("momentum_start", name(c.unit)) end })
status("GCR_PACT", { priority = 9, on_start = function(c) T("pact_start", name(c.unit), "source", name(c.source)) end })
status("GCR_SONIC_WINDOW", {
  priority = 9,
  on_start = function(c) T("window_start") end,
  on_end = function(c) T("window_end", c.reason) end,
})
status("SC_CLOAKINGEXCEED", { priority = 9, on_start = function(c) T("cloak_start", c.duration) end })

-- A server status's duration, doubled: getstatus shows it.
status("SC_BLESSING", { on_start = function(c) return c.duration * 2 end })

skill("GCR_SHADOW_DANCE", {
  priority = 9,
  on_hit = function(c) T("dance_hit", c.target.id, "connected", c.connected) end,
  on_cast = function(c) T("dance_cast", name(c.caster)) end,
})

-- The harness's own skills (test_skills.yml): the kinds the example has none of.
for _, s in ipairs({ "TST_SPLASH_STRIKE", "TST_GROUND_BURST" }) do
  skill(s, {
    custom = true,
    on_hit = function(c) T("tst_hit", s, c.target.id, "connected", c.connected) end,
  })
end
skill("TST_SELF_BUFF", { custom = true })
skill("TST_SUPPORT_BUFF", {
  custom = true,
  on_cast = function(c) c:apply_status("TST_FLEE", {}, "caster") end,
})

-- A stats hook that must hold while other statuses come and go.
status("TST_FLEE", { duration = 60000, stats = function(c, s) s.flee = s.flee + 20 end })
