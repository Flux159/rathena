-- zz-watch: watches the third-class-sampler from the outside, for the test.
-- Every line it logs starts with W| for the test to read.
local function W(...)
  local parts = { "W|" }
  for _, v in ipairs({ ... }) do parts[#parts + 1] = tostring(v) end
  log(table.concat(parts, " "))
end

local function mods(u)
  local out = {}
  for _, s in ipairs(u:statuses()) do
    if s.mod then out[#out + 1] = s.name .. "=" .. s.stacks end
  end
  table.sort(out)
  return #out > 0 and table.concat(out, ",") or "-"
end

-- The watcher's own passive: every second, what the player and the monsters
-- around carry; every blow the player deals; every craft.
skill("TST_EYE", {
  priority = 10,

  tick = function(c)
    local p = c.caster
    W("me", p.hp, p.maxhp, p.sp, p.spheres, p.aspd, p.flee2, mods(p), p.reproduced_skill, p.reproduced_skill_lv, p.hidden)
    for _, u in ipairs(units_around(p, 12, { kind = "mob", hidden = true })) do
      W("mob", u.id, u.mob_id, u.hp, u.x, u.y, u.master_id, mods(u))
    end
    local g = ground_units_around(p, 12)
    local names = {}
    for _, unit in ipairs(g) do names[#names + 1] = unit.skill .. "@" .. unit.x .. "," .. unit.y end
    W("ground", #g, table.concat(names, " "))
  end,

  damage_dealt = function(c, damage)
    W("dealt", c.attack_skill ~= "" and c.attack_skill or "attack", c.target.mob_id or 0, c.target.id, damage, c.hits or 0, c.critical, c.autocast)
  end,

  -- What a blow would deal before the statuses' damage_taken hooks (a
  -- passive's run first), and what it dealt once they had their say.
  damage_taken = function(c, damage)
    W("taken", c.attack_skill ~= "" and c.attack_skill or "attack", c.caster and c.caster.mob_id or 0, damage)
  end,

  on_hit_taken = function(c)
    W("hit", c.attack_skill ~= "" and c.attack_skill or "attack", c.caster and c.caster.mob_id or 0, c.damage, c.connected)
  end,

})

-- A status's craft hook runs after the passives': it sees the chance as the
-- sampler left it, then forces a failure, for the sampler to give the HP back.
status("TST_CRAFT_WATCH", {
  duration = 60000,
  craft = function(c, rate)
    W("craft", c.craft_skill, c.item_name, rate, c.base)
    return 0
  end,
})

-- Probes for what the test cannot do from a script.
local home = nil
skill("TST_PROBE2", {
  custom = true,
  on_cast = function(c)
    local lv = c.skill_lv
    if lv == 1 then
      c:apply_status("GCX_TOXIN", { stacks = 2 }, "caster")
      W("toxin_on_me")
    elseif lv == 2 then
      -- A clean slate between classes: no mod statuses but the craft watch,
      -- no ground skills.
      for _, s in ipairs(c.caster:statuses()) do
        if s.mod and s.name ~= "TST_CRAFT_WATCH" then
          c:remove_status(s.name, "caster")
        end
      end
      for _, g in ipairs(c.caster:ground_units()) do
        c:end_unit(g.group)
      end
    elseif lv == 3 then
      c:spheres(5, "caster")
    elseif lv == 4 then
      c:apply_status("TST_CRAFT_WATCH", {}, "caster")
    elseif lv == 5 then
      c:slide("caster", c.caster.x, c.caster.y + 1)
    elseif lv == 6 then
      if home then c:slide("caster", home.x, home.y) end
    elseif lv == 8 then
      home = { x = c.caster.x, y = c.caster.y }
    elseif lv == 7 then
      c:slide("caster", c.caster.x, c.caster.y - 1)
    end
  end,
})
