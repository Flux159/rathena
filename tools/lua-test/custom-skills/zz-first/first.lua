-- zz-first: a third mod, first in the chains (priority 0), so the test sees
-- what the example's hooks are given as well as what they return.
local function P(...)
  local parts = { "P|" }
  for _, v in ipairs({ ... }) do parts[#parts + 1] = tostring(v) end
  log(table.concat(parts, " "))
end

skill("GCR_EXECUTIONER", {
  priority = 0,
  damage_dealt = function(c, damage)
    if c.target.kind == "mob" and c.target.mob_id == 25991 then
      P("exec_pre", damage, "hp", c.target.hp, "maxhp", c.target.maxhp)
    end
  end,
})

status("GCR_TOXIN", {
  priority = 0,
  damage_taken = function(c, damage)
    P("toxin_pre", damage, "stacks", c.stacks, "match", c.source ~= nil and c.caster.id == c.source.id)
  end,
})
