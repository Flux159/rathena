#!/usr/bin/env python3
"""Reads the map server's log of a custom-skills run and checks what it shows.

S| lines come from the scenario NPC, T| from zz-observe, P| from zz-first and
V| from zz-probe. ERA=prere checks pre-renewal's numbers. Prints one line per
check and exits 1 if any failed."""
import os
import re
import sys

log = open(sys.argv[1], errors="replace").read().splitlines()
S, T, P, V = {}, [], [], []
for line in log:
    m = re.search(r"S\|(\S+) ?(.*)$", line)
    if m and "script debug" in line:
        S.setdefault(m.group(1), []).append(m.group(2))
    m = re.search(r"Lua: zz-observe: T\| (.*)$", line)
    if m:
        T.append(m.group(1).split())
    m = re.search(r"Lua: zz-first: P\| (.*)$", line)
    if m:
        P.append(m.group(1).split())
    m = re.search(r"Lua: zz-probe: V\| (.*)$", line)
    if m:
        V.append(m.group(1).split())

failures = []
passed = 0


def check(name, ok, detail=""):
    global passed
    if ok:
        passed += 1
        print("  ok   %s" % name)
    else:
        failures.append(name)
        print("  FAIL %s %s" % (name, detail))


def s(key, i=0):
    v = S.get(key)
    return v[i] if v and len(v) > i else None


def ts(kind):
    return [t for t in T if t[0] == kind]


print("removal and refund")
check("Dark Illusion is gone", s("darkillusion_lv") == "0", s("darkillusion_lv"))
check("Hallucination Walk is gone", s("hallucinationwalk_lv") == "0", s("hallucinationwalk_lv"))
check("8 points back, 2 spent on skill-ups", s("skillpoint") == "16", s("skillpoint"))
check("a new skill learned from the client", s("contact_poison_lv") == "1", s("contact_poison_lv"))
check("Phantom Menace learned without Dark Illusion", s("phantom_menace_lv") == "1", s("phantom_menace_lv"))

print("passives")
before, after = int(s("flee2_before")), int(s("flee2_after"))
check("bonus: +5 and +90 Perfect Dodge (two mods)", after == before + 50 + 900, "%d -> %d" % (before, after))
check("grant_skill: Heal 3", s("heal_lv") == "3", s("heal_lv"))
check("on_attack applied Toxin", any(t[1:3] == ["Test", "Dummy"] for t in ts("toxin_start")))
check("on_skill saw the skills used", {t[1] for t in ts("on_skill")} >= {"GCR_VENOM_BURST", "AS_SONICBLOW", "GC_CROSSIMPACT", "GCR_TOXIC_CLOUD"}, ts("on_skill"))

print("Venom Burst")
vr = ts("venom_ratio")
check("ratio chain", vr and int(vr[0][1]) == 100 + 300 + 75 * int(vr[0][3]) and int(vr[0][3]) > 0, vr)
# The first end after the cast: Contact Poison's stacks may have decayed and
# come back during the attacks before it.
cast = next((i for i, t in enumerate(T) if t[0] == "venom_cast"), len(T))
ends = [t for t in T[cast:] if t[0] == "toxin_end" and t[1:3] == ["Test", "Dummy"]]
check("on_cast used up the Toxin", ends and ends[0][4] == "removed", ends[:1])

print("Cross Impact")
seq = [t[0] + (":" + t[2] if t[0] == "ci_can_cast" else "") for t in T if t[0] in ("ci_can_cast", "ci_cast", "window_start", "window_end")]
check("refused, window, cast, closed", seq[:5] == ["ci_can_cast:false", "window_start", "ci_can_cast:true", "ci_cast", "window_end"], seq)
# Cross Impact costs 40 SP in renewal, 25 in pre-renewal.
stock_sp = 25 if os.environ.get("ERA") == "prere" else 40
check("half SP", s("crossimpact_sp") == str(stock_sp // 2), s("crossimpact_sp"))

print("Toxic Cloud and Toxin")
hits = [t for t in ts("cloud_hit") if t[1:3] == ["Test", "Dummy"]]
check("the cloud hits each second, as Poison", len(hits) >= 5 and all(t[t.index("element") + 1] == "5" for t in hits), len(hits))
ticks = [t for t in ts("toxin_tick") if t[1:3] == ["Test", "Dummy"]]
ok = True
for a, b in zip(ticks, ticks[1:]):
    pass
check("tick hooks ran", len(ticks) >= 5, len(ticks))
pre = [p for p in P if p[0] == "toxin_pre"]
post = [t for t in ts("toxin_taken")]
pairs = list(zip(pre, post))
mult_ok = pairs and all(
    (int(b[1]) == int(a[1]) * (100 + 2 * int(a[3])) // 100) if a[5] == "true" else (b[1] == a[1])
    for a, b in pairs
)
check("damage_taken: +2% a stack from the poisoner", mult_ok, pairs[:3])
check("decay and expiry", any(t[4] == "expired" for t in ts("toxin_end")), ts("toxin_end"))

print("Executioner")
epre = [p for p in P if p[0] == "exec_pre"]
epost = [t for t in ts("exec_dealt")]
ep = list(zip(epre, epost))
low = [(a, b) for a, b in ep if int(a[3]) * 100 < int(a[5]) * 35]
high = [(a, b) for a, b in ep if int(a[3]) * 100 >= int(a[5]) * 35]
check("+25% under 35% HP", low and all(int(b[1]) == int(a[1]) * 125 // 100 for a, b in low), low)
check("unchanged above", high and all(b[1] == a[1] for a, b in high), high[:2])
check("on_kill", any(t[1:3] == ["Test", "Weakling"] for t in ts("exec_kill")))
check("give_item: 2 Red Potions", s("red_potions") == "2", s("red_potions"))
check("a death ends Toxin", any(t[1:3] == ["Test", "Weakling"] and t[4] == "death" for t in ts("toxin_end")))

print("Shadow Instinct, Shadow Pact, statuses")
check("perfect dodges seen in on_hit_taken", len(ts("perfect_dodge")) >= 1)
check("Momentum started", any(t[1] == "Tester" for t in ts("momentum_start")))
check("Pact: +15 HIT from the stats hook", int(s("hit_pact")) == int(s("hit_before")) + 15, (s("hit_before"), s("hit_pact")))
check("Cloaking Exceed: 10 s -> 15 s", ["cloak_start", "15000"] in ts("cloak_start"), ts("cloak_start"))
check("Blessing doubled", int(s("blessing_left") or 0) >= 19000, s("blessing_left"))

print("Shadow Dance")
cx, cy = map(int, (s("dance_caster") or "0 0").split())
mobs = {}
for v in S.get("dance_mob", []):
    i, x, y = map(int, v.split())
    mobs[i] = max(abs(x - cx), abs(y - cy))
hit = {int(t[1]) for t in ts("dance_hit")}
connected = len([t for t in ts("dance_hit") if t[3] == "true"])
near = {i for i, d in mobs.items() if d <= 2}
check("hits every enemy within 2 cells, and only those", near and hit == near, (mobs, hit))
# +3 CRIT a stack; a katar doubles the critical the status window shows.
check("Momentum: +3 CRIT for each one hit", s("dance_crit") == str(2 * 3 * min(5, connected)), (s("dance_crit"), connected))
check("the skill was used (on_cast, on_skill)", ts("dance_cast") and any(t[1] == "GCR_SHADOW_DANCE" for t in ts("on_skill")))

print("other kinds of custom skill")
km = {}
for v in S.get("kinds_mob", []):
    i, x, y = map(int, v.split())
    km[i] = (x, y)
center = km.get(int(s("kinds_center") or 0))
around = {i for i, (x, y) in km.items() if center and max(abs(x - center[0]), abs(y - center[1])) <= 1}
# Only the hits on these three: the probe step casts the skill on others later.
splash = [int(t[2]) for t in ts("tst_hit") if t[1] == "TST_SPLASH_STRIKE" and int(t[2]) in km]
ground = [int(t[2]) for t in ts("tst_hit") if t[1] == "TST_GROUND_BURST"]
check("Attack with a SplashArea: the target and what is next to it", around and len(around) < len(km) and set(splash) == around, (km, splash))
check("Ground without a Unit: its area, once each", around and sorted(ground) == sorted(around), (km, ground))
check("Self and Support start their Status", s("kinds_status") == "1 1", s("kinds_status"))
f0, f1, f2, f3 = map(int, (s("stats_hold") or "0 0 0 0").split())
check("a stats hook adds its FLEE, and keeps it through another status", f1 == f0 + 20 and f2 > f1 and f3 == f1, (f0, f1, f2, f3))

print("boss")
boss = [int(t[t.index("stacks") + 1]) for t in ts("toxin_tick") if t[1:3] == ["Test", "Boss"]]
check("a boss holds at most 3 stacks", boss and max(boss) == 3, boss)

print("on_death")
check("survived at 1 HP", s("hp_after_death") == "1", s("hp_after_death"))
check("still alive a second later", s("alive_after_death") == "1", s("alive_after_death"))

print("refund and reset")
gain, expected = (s("reset_gain") or "0 -1").split()
check("a reset after the refund gives nothing back twice", gain == expected, (gain, expected))


def vs(kind):
    return [v for v in V if v[0] == kind]


def v1(kind):
    got = vs(kind)
    return got[0][1:] if got else None


print("queries")
check("skill_info: Fire Bolt 5", v1("skill_info") == ["5", "magic", "attack", "10"], v1("skill_info"))
check("units_around: the two within 3 cells", v1("around") == ["2"], v1("around"))
check("units_in_line: the one on the line", v1("line") == ["1"], v1("line"))
me = v1("me") or []
check("unit fields: katar, char_id, aspd", me[:2] == ["16", "150000"] and me[3] == "1250", me)
check("unit:equipment", v1("weapon") == ["1250", "0", "0"], v1("weapon"))
check("now()", v1("now") == ["true"], v1("now"))

print("actions")
check("knockback: 2 cells", s("probe_push") == "5", s("probe_push"))
check("spend: 100 HP, 10 SP", s("probe_spend") == "100 10", s("probe_spend"))
check("give_item 3, take_item 2", s("probe_potions") == "1", s("probe_potions"))
check("mount: falcon", s("probe_falcon") == "1", s("probe_falcon"))
check("copy_skill: Cold Bolt 3", s("probe_coldbolt") == "3" and v1("copied") == ["MG_COLDBOLT", "3"], (s("probe_coldbolt"), v1("copied")))
check("spheres: 3", v1("spheres") == ["3"], v1("spheres"))
check("status with vals: Blessing 10, Triple Attack +50%", v1("blessing") == ["10", "50"], v1("blessing"))
check("place_unit: a Fire Wall of 3 cells", v1("firewall") == ["1", "3"], v1("firewall"))
check("summon: one Poring of the player's", v1("summoned") == ["1"], v1("summoned"))
check("status_val, end_unit, mount off, spheres -1", v1("after") == ["5", "0", "false", "2"], v1("after"))
check("expire: the Poring left", v1("summoned_after") == ["0"], v1("summoned_after"))
check("end_status", v1("ended") == ["true"], v1("ended"))
check("slide: 2 cells west", s("probe_slide") == "-2 0", s("probe_slide"))
check("unit:statuses lists a mod's status", v1("statuses_mod") == ["true", "1"], v1("statuses_mod"))
# c:attack counts its hit as one target; at power 50 the hooks see half of
# what an unscaled c:attack deals, and a damage they fix stays fixed.
attacks = [d for d in vs("dealt") if "targets" in d and d[d.index("targets") + 1] == "1" and "damage" in d]
half = [int(d[d.index("damage") + 1]) for d in attacks if d[1] == "25990"]
full = [int(d[d.index("damage") + 1]) for d in attacks if d[1] == "25991"]
check("c:attack power 50: hooks see the hit halved; a fixed 1000 stays 1000",
      s("probe_power") == "1000" and half and full and 0.4 <= half[0] / full[0] <= 0.6, (s("probe_power"), half[:1], full[:1]))

print("battle config, element, size and DEF tables, cooldowns, cost")
# What the server's own tables say, for this era: the shared file, then the era's.
DB = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), "..", "db")
ERA_DIR = "pre-re" if os.environ.get("ERA") == "prere" else "re"


def attr_table(path):
    table, level, atk = {}, None, None
    for line in open(path):
        m = re.match(r"  - Level: (\d+)", line)
        if m:
            level = int(m.group(1)); continue
        m = re.match(r"    (\w+):\s*$", line)
        if m:
            atk = m.group(1); continue
        m = re.match(r"      (\w+): (-?\d+)", line)
        if m and level is not None and atk is not None:
            table[(level, atk, m.group(1))] = int(m.group(2))
    return table


def size_table(paths):
    table, weapon = {}, None
    for path in paths:
        for line in open(path):
            m = re.match(r"  - Weapon: (\w+)", line)
            if m:
                weapon = m.group(1); table.setdefault(weapon, {"Small": 100, "Medium": 100, "Large": 100}); continue
            m = re.match(r"    (Small|Medium|Large): (\d+)", line)
            if m and weapon:
                table[weapon][m.group(1)] = int(m.group(2))
    return table


attr = attr_table(os.path.join(DB, ERA_DIR, "attr_fix.yml"))
size = size_table([os.path.join(DB, "size_fix.yml"), os.path.join(DB, ERA_DIR, "size_fix.yml")])
check("battle_config reads the server's value; 0 for a name it lacks",
      v1("battle_config") == [s("probe3_maxparam"), "0"], (v1("battle_config"), s("probe3_maxparam")))
want = [str(attr[(1, "Fire", "Earth")]), str(attr[(2, "Water", "Fire")]), str(attr[(1, "Holy", "Undead")])]
check("element_rate: the element table", v1("element_rate") == want, (v1("element_rate"), want))
check("element_rate on a unit; unit.element_level", v1("element_rate_unit") == [str(attr[(1, "Fire", "Neutral")]), "0", "1"], v1("element_rate_unit"))
want = [str(size["Knuckle"]["Large"]), str(size["Dagger"]["Large"]), str(size["2hSword"]["Medium"]), str(size["Fist"]["Large"])]
check("size_rate: the size table, with the era's own", v1("size_rate") == want, (v1("size_rate"), want))
dr = [float(x) for x in (v1("def_rate") or ["0", "0", "0"])]
want = [100.0, 90.0, 82.0] if ERA_DIR == "re" else [100.0, 50.0, 0.0]
check("def_rate: the era's hard DEF formula", dr == want, (dr, want))
left = int((v1("cooldown_left") or ["-1"])[0])
check("c:cooldown starts a cooldown that blocks the skill",
      s("probe3_heal_blocked") == "0" and 3000 <= left <= 5000, (s("probe3_heal_blocked"), left))
check("c:cooldown 0 ends it; the skill works again",
      v1("cooldown_cleared") == ["0"] and s("probe3_heal_after") not in (None, "0"), (v1("cooldown_cleared"), s("probe3_heal_after")))
check("cost sees the target: 1 SP on the dummy instead of 50",
      s("probe3_cost") == "1" and v1("cost_target") == ["25990"], (s("probe3_cost"), v1("cost_target")))
cells = [v[1:] for v in vs("cost_cell")]
check("cost sees a ground skill's cell", cells and cells[-1] == (s("probe3_cell") or "").split(), (cells, s("probe3_cell")))

print("a ground skill's power, range hooks, statuses kept over a logout")
plain, half, double = v1("tick_units") or ["0", "0", "0"]
ticks = {}
for v in vs("tick_hit"):
    ticks.setdefault(v[1], []).append(int(v[2]))


def mean(xs):
    return sum(xs) / len(xs) if xs else 0


p0, p50, p200 = mean(ticks.get(plain, [])), mean(ticks.get(half, [])), mean(ticks.get(double, []))
check("c:place_unit power 50: every hit of the unit deals half",
      p0 > 0 and len(ticks.get(half, [])) >= 3 and 0.4 <= p50 / p0 <= 0.6, (p0, p50, {k: len(x) for k, x in ticks.items()}))
check("c:cast power 200: what the cast placed deals double, hit after hit",
      p0 > 0 and len(ticks.get(double, [])) >= 3 and 1.7 <= p200 / p0 <= 2.3, (p0, p200))
check("range hooks: the skill's own +1, a status's +4 (unit:skill_range)",
      v1("range") == ["4"] and v1("range_long") == ["8"], (v1("range"), v1("range_long")))
reach = (s("probe4_reach") or "").split()
check("a use reaches as far as the hooks say, from where the caster stands",
      len(reach) == 4 and reach[0] == "0" and int(reach[1]) > 0 and reach[2:] == ["0", "0"], reach)
kept = [v[1:] for v in vs("kept")]
before, after = (kept + [None, None])[:2]
check("persist: back after a relog with its 3 stacks; a status without it is not",
      before and after and before[0] == "3" and after[0] == "3" and before[2] == "true" and after[2] == "false", kept)
check("persist: its time stood still while offline, and on_start ran once",
      before and after and 0 <= int(before[1]) - int(after[1]) <= 4000 and after[3] == "1", kept)
check("persist: its stats hook is back (+11 FLEE a stack)", s("probe4_kept_flee") == "33", s("probe4_kept_flee"))

print("hooks")
st = (s("probe_stats") or "0 0 0").split()
check("a passive's stats hook: +7 HIT, ASPD 120, +1000 Max HP", st == ["7", "120", "1000"], st)
check("a status's stats hook: attack speed (ASPD 120 -> 125)", s("probe_haste") == "125", s("probe_haste"))
check("a status's cost: half SP", s("probe_discount") == "5", s("probe_discount"))
check("a status's can_cast refuses", s("probe_locked") == "0", s("probe_locked"))
auto = vs("auto")
check("a status's critical: every normal attack a critical", auto and all(a[1] == "true" for a in auto), auto[:3])
check("a status's on_skill", any(w[1] == "TST_SPLASH_STRIKE" for w in vs("watch_skill")), vs("watch_skill")[:3])
check("before_cast, before the hit", [v[0] for v in V if v[0] in ("before_cast", "tst_strike")][:2] == ["before_cast", "tst_strike"], vs("before_cast"))
dealt = [d for d in vs("dealt") if d[1] == "25990"]
check("hits: 3, seen by damage hooks", dealt and dealt[0][3] == "3", dealt[:1])
check("targets: the two a split area skill hit", dealt and "targets" in dealt[0] and dealt[0][dealt[0].index("targets") + 1] == "2", dealt[:1])
check("ground_units_around: anyone's, and only enemies'", v1("ground_around") == ["1", "0"], v1("ground_around"))
check("teleport: the monster went elsewhere", s("probe_teleport") == "1", s("probe_teleport"))
check("critical forced on a skill", vs("tst_strike") and vs("tst_strike")[0][2] == "true" and dealt and dealt[0][5] == "true", (vs("tst_strike")[:1], dealt[:1]))
check("on_kill: the whole blow and what was left over", ["25991", "10000", "6000", "TST_SPLASH_STRIKE"] in [w[1:] for w in vs("watch_kill")], vs("watch_kill"))
check("a monster's status: on_death and survive", s("probe_second_life") == "1" and vs("second_life"), (s("probe_second_life"), vs("second_life")))
check("chance: the rate as given (out of 10000)", v1("curse_rate") == ["2500", "SC_CURSE"], v1("curse_rate"))
check("chance: 0 stops a status", v1("stun") == ["false"], v1("stun"))
check("status(\"*\"): every server status, with by_skill", ["SC_BLESSING", "TST_PROBE", "false"] in [a[1:] for a in vs("any_start")], vs("any_start"))
check("heal: the hook acts", v1("heal") == ["true"] and v1("overflow") == ["Tester"], (v1("heal"), v1("overflow")))
check("on_use: the item's and the passive's", v1("used") == ["Red_Potion", "0", "Tester"] and v1("passive_use") == ["Red_Potion"], (v1("used"), v1("passive_use")))
check("craft: the passive's rate", v1("craft") == ["Sword", "BS_SWORD", "true"], v1("craft"))
check("on_craft: a random option on the forged Sword", s("probe_sword") == "1" and v1("crafted") == ["true", "true", "1", "50"], (s("probe_sword"), v1("on_craft"), v1("crafted")))

check("before_cast false: no Angelus, the 7 SP paid", s("probe_veto") == "0 7" and v1("vetoed") == ["Tester"], (s("probe_veto"), v1("vetoed")))
check("summon neutral: a dummy of nobody's", v1("neutral") == ["1", "0"], v1("neutral"))
check("itemskill: Soul Strike 3, which the player does not have, once", s("probe_soulstrike_lv") == "0" and v1("itemskill_hit") == ["3", "Tester"], (s("probe_soulstrike_lv"), v1("itemskill_hit")))
check("a mod status's hook calls c:status and c:hurt beside c.status_name and c.damage", s("probe_endure") == "1" and v1("watch_actions") == ["TST_WATCH", "number"], (s("probe_endure"), v1("watch_actions")))
check("unit:ground_units: x and y on each group", v1("firewall_xy") == ["true", "true"], v1("firewall_xy"))
check("before_cast once per use, though Counter Slash re-enters", len(vs("cs_before")) == 1, vs("cs_before"))
floor = (v1("amotion_min") or ["0"])[0]
check("stats: amotion_min, the server's ASPD limit", floor == ("100" if os.environ.get("ERA") == "prere" else "70"), floor)
check("a server status's on_end sees its values", v1("blessing_end") == ["5", "SC_BLESSING"], v1("blessing_end"))
check("skill_info: Mammonite 10 costs 1000 zeny", v1("zeny_cost") == ["1000"], v1("zeny_cost"))

errors = [l for l in log if "failed and is now off" in l or "Lua:" in l and "Error" in l]
check("no Lua errors", not errors, errors[:3])

print("\n%d passed, %d failed" % (passed, len(failures)))
sys.exit(1 if failures else 0)
