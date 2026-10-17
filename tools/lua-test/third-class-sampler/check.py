#!/usr/bin/env python3
"""Reads the map server's log of a third-class-sampler run and checks it.

S| lines come from the scenario, W| lines from the watcher mod (zz-watch).
Prints one line per check and exits 1 if any failed."""
import re
import sys

log = sys.argv[1] if len(sys.argv) > 1 else "map.out"
events = []   # ("S" | "W", [words]) in log order
errors = []
for line in open(log, errors="replace"):
    m = re.search(r"script debug : \S+ \S+ : S\|(.*)", line)
    if m:
        events.append(("S", m.group(1).split()))
        continue
    m = re.search(r"Lua: zz-watch: W\| ?(.*)", line)
    if m:
        events.append(("W", m.group(1).split()))
        continue
    if "[Error]" in line and not re.search(
            r"File not found|Failed to open|Messages file not found|Log configuration file", line):
        errors.append(line.strip())

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))


def s_line(key):
    """The words after the first S| line whose first word is key."""
    for kind, words in events:
        if kind == "S" and words and words[0] == key:
            return words[1:]
    return None


def s_ints(key):
    w = s_line(key)
    try:
        return [int(x) for x in w] if w is not None else None
    except ValueError:
        return None


def window(start, end):
    """The events between the first S| or W| line starting with the words in
    `start` and the next one starting with those in `end` (or the end of the
    log)."""
    out, inside = [], False
    for kind, words in events:
        if not inside and words[:len(start)] == start:
            inside = True
            continue
        if inside and words[:len(end)] == end:
            break
        if inside:
            out.append((kind, words))
    return out


def step(name):
    order = ["gx", "sc", "ab", "rg", "sura", "mech", "wl", "sorc", "gen", "rk", "ra", "mw"]
    i = order.index(name)
    end = ["step", order[i + 1]] if i + 1 < len(order) else ["end"]
    return window(["step", name], end)


def between(a, b):
    return window([a], [b])


def me_lines(evs):
    """The watcher's W| me lines: hp maxhp sp spheres aspd flee2 mods ..."""
    return [w for k, w in evs if k == "W" and w and w[0] == "me"]


def mods_of(me):
    out = {}
    if len(me) > 7 and me[7] != "-":
        for part in me[7].split(","):
            name, _, n = part.partition("=")
            out[name] = int(n or 0)
    return out


def dealt(evs, skill=None):
    """W| dealt lines: dealt skill mob_id target damage hits critical autocast."""
    return [w for k, w in evs if k == "W" and w and w[0] == "dealt" and (skill is None or w[1] == skill)]


def blows(evs, mob_id):
    """Pairs of (what the blow would deal before the statuses' hooks, what it
    dealt) for normal attacks of that monster."""
    out, pending = [], None
    for k, w in evs:
        if k != "W" or not w:
            continue
        if w[0] == "taken" and w[1] == "attack" and w[2] == str(mob_id):
            pending = int(w[3])
        elif w[0] == "hit" and w[1] == "attack" and w[2] == str(mob_id):
            if w[4] == "true" or pending is not None:
                out.append((pending, int(w[3])))
            pending = None
    return out


# ---- Guillotine Cross -----------------------------------------------------
gx = step("gx")
v = s_ints("gx_pd")
check("gx: Hallucination Walk adds 10 perfect dodge (100 tenths)", v and v[1] - v[0] == 100, v)
v = s_ints("gx_aspd")
check("gx: Flurry stacks raise ASPD", v and v[1] > v[0], v)
flurry = max([mods_of(m).get("GCX_FLURRY", 0) for m in me_lines(gx)] or [0])
check("gx: Dark Illusion's blows stack Flurry", flurry >= 3, "max %d" % flurry)
toxin_mob = any(k == "W" and w[0] == "mob" and "GCX_TOXIN" in " ".join(w[7:]) for k, w in gx)
check("gx: Envenom puts Toxin on the dummy", toxin_mob)
on_me = window(["toxin_on_me"], ["client", "use"])
had = any("GCX_TOXIN" in mods_of(m) for m in me_lines(on_me))
after = me_lines(window(["client", "use"], ["gx_cdp_dip"]))
check("gx: Green Potion takes Toxin off", had and after and "GCX_TOXIN" not in mods_of(after[-1]),
      "had %s, after %s" % (had, after[-1][7] if after else None))
craft = [w for k, w in gx if k == "W" and w[0] == "craft"]
check("gx: Create Deadly Poison's chance is capped at base + 1500",
      craft and int(craft[0][3]) == int(craft[0][4]) + 1500, craft[:1])
v = s_ints("gx_cdp_dip")
check("gx: a failed Create Deadly Poison costs HP", v and v[1] < v[0], v)
v = s_ints("gx_cdp_back")
check("gx: ...and the sampler gives it back", v and v[1] >= v[0] and v[2] == 0, v)

# ---- Shadow Chaser --------------------------------------------------------
sc = step("sc")
v = s_ints("sc_reproduce")
check("sc: Reproduce copies the monster's Fire Bolt 3", v == [3], v)
v = s_ints("sc_hidden")
check("sc: Feint Bomb hides the player", v == [1], v)
decoy = [int(w[3]) for k, w in sc if k == "W" and w[0] == "mob" and w[2] == "25300"]
check("sc: Feint Bomb's decoy draws the biter", decoy and min(decoy) < 2000,
      "decoy HP %s" % (decoy[-1:] or None))
v = s_ints("sc_triangle")
check("sc: Triangle Shot's Double Strafe hits two more", v and v[0] > 0 and v[1] > 0, v)

# ---- Arch Bishop ----------------------------------------------------------
ab = step("ab")
v = s_ints("ab_refused")
check("ab: Adoramus without Grace is refused", v == [0], v)
grace = max([mods_of(m).get("ABX_GRACE", 0) for m in me_lines(window(["ab_refused"], ["ab_adoramus"]))] or [0])
check("ab: Heals build Grace", grace >= 2, "max %d" % grace)
v = s_ints("ab_adoramus")
check("ab: Adoramus with Grace hits", v and v[0] > 0, v)
late = me_lines(window(["ab_adoramus"], ["ab_focus_before_step"]))
check("ab: Adoramus spends the Grace", late and "ABX_GRACE" not in mods_of(late[-1]))
focus = max([mods_of(m).get("ABX_FOCUS", 0) for m in me_lines(ab)] or [0])
check("ab: casts from one cell stack Focus", focus >= 3, "max %d" % focus)
moved = me_lines(window(["ab_focus_before_step"], ["ab_focus_after_step"]))
check("ab: a step ends Focus", moved and "ABX_FOCUS" not in mods_of(moved[-1]))

# ---- Royal Guard ----------------------------------------------------------
rg = step("rg")
b = [(x, y) for x, y in blows(between("rg_prestige_on", "rg_prestige_off"), 25995) if x]
check("rg: Prestige: each blow lands 15% lighter",
      len(b) >= 2 and all(abs(y - x * 85 // 100) <= 1 for x, y in b), b)
v = s_ints("rg_inspiration")
sac = dealt(rg, "PA_SACRIFICE")
check("rg: Inspiration: the Sacrifice kill gives its HP back",
      v and sac and v[0] - v[1] <= v[2] * 12 // 100, "hp %s, %d Sacrifice hits" % (v, len(sac)))
v = s_ints("rg_gc_hp")
check("rg: Ray of Genesis: Grand Cross costs no HP and does not hurt", v and v[0] == v[1], v)
field = dealt(window(["rg_gc_hp"], ["rg_field_done"]), "RGX_HOLY_FIELD")
check("rg: Grand Cross leaves the holy field", len(field) >= 2, "%d hits" % len(field))

# ---- Sura -----------------------------------------------------------------
sura = step("sura")
spheres = [int(m[4]) for m in me_lines(window(["step", "sura"], ["sura_snap"])) if int(m[4]) < 5]
check("sura: Energy Gain's blows gather spheres", spheres and max(spheres) >= 1, "max %s" % (max(spheres) if spheres else None))
v = s_ints("sura_snap")
check("sura: Snap hits both dummies on the way, and lands", v and v[0] > 0 and v[1] > 0 and v[2] == 6, v)

# ---- Mechanic -------------------------------------------------------------
v = s_ints("mech_masterwork")
check("mech: Masterwork takes an Oridecon for 8 ATK", v and v[1] - v[0] == 8 and v[2] == 1, v)
v = s_ints("mech_cleave")
check("mech: Axe Training cleaves the dummy beside", v and v[0] > 0, v)

# ---- Warlock --------------------------------------------------------------
v = s_ints("wl_stored")
check("wl: the Fire Bolt is paid for and stored", v and v[0] == 0 and v[1] > 0, v)
v = s_ints("wl_released")
check("wl: Release sets it off", v and v[0] > 0, v)

# ---- Sorcerer -------------------------------------------------------------
sorc = window(["step", "sorc"], ["sorc_done"])
bolts = [w for w in dealt(sorc, "MG_FIREBOLT") if w[7] == "true"]
check("sorc: Spell Fist: 4 blows release 4 Fire Bolts", len(bolts) == 4, "%d bolts" % len(bolts))

# ---- Genetic --------------------------------------------------------------
v = s_ints("gen_splash")
check("gen: Acid Demonstration splashes next door", v and v[0] > 0, v)

# ---- Rune Knight ----------------------------------------------------------
v = s_ints("rk_poison_before")
check("rk: poison starts before Refresh", v == [1], v)
v = s_ints("rk_refresh")
check("rk: Refresh ends it, and a new one does not start", v == [0, 0], v)
b = blows(between("rk_skin_on", "rk_skin_off"), 25996)
check("rk: Stone Hard Skin takes the first blow whole, then breaks",
      len(b) >= 2 and b[0][1] == 0 and any(y > 0 for _, y in b[1:]), b)
v = s_ints("rk_skin_knock")
check("rk: its break knocks the dummy beside 3 cells away", v and v[0] <= -3, v)
v = s_ints("rk_pull")
check("rk: Phantom Thrust pulls the one beside its target in front", v == [1, 0], v)
v = s_ints("rk_windcutter")
check("rk: in Berserk, Bash is refused and Wind Cutter starts Two-Hand Quicken", v == [0, 1], v)

# ---- Ranger ---------------------------------------------------------------
ra = step("ra")
v = s_ints("ra_arrows")
check("ra: Fear Breeze's Double Strafe uses 4 arrows", v == [4], v)
ds = dealt(ra, "AC_DOUBLE")
check("ra: ...and shows 5 hits", ds and ds[0][5] == "5", ds[:1])
ss = dealt(ra, "SN_SHARPSHOOTING")
check("ra: Sharp Shooting out of Camouflage crits", ss and ss[0][6] == "true", ss[:1])
v = s_ints("ra_detonator")
check("ra: Detonator sets off both Claymores", v and v[0] > 0 and v[1] > 0, v)

# ---- Minstrel -------------------------------------------------------------
v = s_ints("mw_echo")
check("mw: Arrow Vulcan echoes inside the field, not outside", v and v[0] > 0 and v[1] == 0, v)

check("the run reached its end", s_line("end") is not None)
check("no errors in the map server's log", not errors, errors[:5])

failed = 0
for name, ok, detail in results:
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, "" if ok else "  [%s]" % (detail,)))
    failed += not ok
print("%d/%d passed" % (len(results) - failed, len(results)))
sys.exit(1 if failed else 0)
