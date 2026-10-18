#!/usr/bin/env python3
"""Build db/re/mob_db_world.yml (and the world-sync block of
db/re/mob_skill_db.txt) for the monsters the official clients' navigation
lists and the monster db lacks.

Input: one Divine Pride monster record per file (api/database/Monster/{id},
server=iRO), as saved by a downloader that keeps the API key outside this
repository; and the clients' navi_mob tables for the English names. Divine
Pride shows the attack and magic attack ranges, not rAthena's Attack/Attack2,
so they are solved from renewal's monster formulas:
    ATK  = Str + Level + Attack  * [0.8, 1.2]
    MATK = Int + Level + Attack2 * [0.7, 1.3]
WalkSpeed and the attack timings come from the formulas fitted against
monsters rAthena already has (see CALIBRATION). Drops whose item the item db
doesn't know are left out and listed in the report.

Rerun generate.py afterwards: the new monsters then get their spawns.
"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import luadata  # noqa: E402

RACES = {'formless': 'Formless', 'undead': 'Undead', 'brute': 'Brute', 'plant': 'Plant', 'insect': 'Insect',
         'fish': 'Fish', 'demon': 'Demon', 'demihuman': 'Demihuman', 'angel': 'Angel', 'dragon': 'Dragon'}
RACE_IDS = ['Formless', 'Undead', 'Brute', 'Plant', 'Insect', 'Fish', 'Demon', 'Demihuman', 'Angel', 'Dragon']
ELEMENTS = {'neutral': 'Neutral', 'water': 'Water', 'earth': 'Earth', 'fire': 'Fire', 'wind': 'Wind',
            'poison': 'Poison', 'holy': 'Holy', 'shadow': 'Dark', 'dark': 'Dark', 'ghost': 'Ghost',
            'undead': 'Undead'}
ELEMENT_IDS = ['Neutral', 'Water', 'Earth', 'Fire', 'Wind', 'Poison', 'Holy', 'Dark', 'Ghost', 'Undead']
SIZES = {'small': 'Small', 'medium': 'Medium', 'large': 'Large'}
SIZE_IDS = ['Small', 'Medium', 'Large']
AI_CODES = {'01', '02', '03', '04', '05', '06', '07', '08', '09', '10', '11', '12', '13', '17', '19', '20',
            '21', '24', '25', '26', '27'}
STATES = {'idle': 'idle', 'idling': 'idle', 'walking': 'walk', 'moving': 'walk', 'attacking': 'attack', 'chasing': 'chase', 'following': 'follow',
          'dead': 'dead', 'angry': 'angry', 'masterattacked': 'angry', 'attacked': 'angry',
          'looting': 'loot', 'any': 'anytarget', 'anytarget': 'anytarget',
          'unknown': 'attack'}   # Divine Pride doesn't know the state: nearly all are attack skills

# Fitted against the calibration monsters (Divine Pride record vs rAthena entry); see fit_report().
# Divine Pride's speed is cells per second and attackSpeed is 1000 / AttackMotion; AttackDelay and
# DamageMotion aren't shown, so they use the median ratio to AttackMotion of the calibration monsters.
CALIBRATION = {
    'walk': lambda speed: rnd(1000.0 / speed) if speed else 200,
    'delay_per_motion': 0.68,       # median of the 29 newer calibration monsters (Id 20000+)
    'damage_motion': 480,           # median of the calibration monsters
}


def rnd(x):
    return int(x + 0.5)


def key(s):
    return re.sub(r'[^a-z]', '', str(s or '').lower())


def item_names(root):
    out = {}
    for f in glob.glob(os.path.join(root, 'db', 're', 'item_db*.yml')):
        for i, a in re.findall(r'^  - Id: (\d+)\n    AegisName: (\S+)', open(f, encoding='utf-8').read(), re.M):
            out.setdefault(int(i), a)
    return out


def mob_db(root):
    out = {}
    for f in glob.glob(os.path.join(root, 'db', 're', 'mob_db*.yml')):
        if f.endswith('mob_db_world.yml'):      # ours: rebuilt from scratch
            continue
        for e in re.split(r'\n(?=  - Id: )', open(f, encoding='utf-8').read()):
            m = re.match(r'  - Id: (\d+)\n    AegisName: (\S+)', e)
            if m:
                out[int(m.group(1))] = (m.group(2), e)
    return out


def navi_mobs(paths):
    """id -> (name, aegis, level, element, element level, race, size), iRO first."""
    out = {}
    for p, enc in paths:
        for r in luadata.ordered_lines(luadata.run([p], enc).get('Navi_Mob') or {}):
            if not isinstance(r, dict) or not r.get(4):
                continue
            mid = r[4] & 0xFFFF
            if mid in out:
                continue
            v = r.get(8, 0)
            ele = (v >> 16) & 0xFF
            out[mid] = (str(r.get(5, '')), str(r.get(6, '')), r.get(7), ele % 20, ele // 20,
                        (v >> 8) & 0xFF, v & 0xFF)
    return out


def english_names(paths):
    """Korean navigation name -> English, from ROenglishRE's SystemEN/Navi_Data.lub (text Lua)."""
    out = {}
    for p in paths:
        text = open(p, 'rb').read().decode('cp949', errors='replace')
        for ko, en in re.findall(r'\[\s*"([^"]+)"\s*\]\s*=\s*"([^"]*)"', text):
            if en and ascii_name(en):
                out.setdefault(ko.strip(), en.strip())
    return out


def aegis_name(aegis):
    """A readable name from an AegisName: family prefixes off, words capitalized."""
    a = re.sub(r'^(EP\d+(_\d+)?|CH\d+|CP_ABI|ABI|ILL|MD|G|E|QE|MQ|NC|HEROS|BL)_', '', aegis)
    return ' '.join(w.capitalize() for w in a.split('_') if w)


def ascii_name(s):
    return s if s and all(32 <= ord(c) < 127 for c in s) else ''


def yq(s):
    """A YAML scalar: plain when it starts with a letter or digit and holds nothing special."""
    s = str(s)
    if re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9 ()\-.,!?&/+]*', s) and not s.endswith(' ') \
            and s.lower() not in ('yes', 'no', 'true', 'false', 'null', 'on', 'off'):
        return s
    return '"%s"' % s.replace('\\', '\\\\').replace('"', '\\"')


def mid_range(s):
    m = re.match(r'\s*(\d+)\s*-\s*(\d+)', re.sub(r'[.,]', '', str(s or '')))
    return (int(m.group(1)) + int(m.group(2))) / 2.0 if m else None


def attack_motion(d):
    a = d.get('attackSpeed')
    return rnd(1000.0 / a) if a else 600


FILL = ('str', 'agi', 'vit', 'int', 'dex', 'luk', 'def', 'mDef', 'res', 'mRes', 'attackRange',
        'magicAttackRange', 'health', 'baseExperience', 'jobExperience', 'speed', 'attackSpeed', 'range')


def family(d):
    return str(d.get('spriteName') or '').upper().split('_')[0]


def complete(d):
    return all(d.get(k) for k in ('str', 'health', 'attackRange', 'baseExperience', 'speed', 'attackSpeed'))


def fill_gaps(d, pool):
    """Copy what Divine Pride doesn't show from the nearest-level complete record of the same family
    (EP21_, CH1_, ...), else of any family. Returns (donor AegisName, fields copied)."""
    gaps = [k for k in FILL if d.get(k) in (None, '', 0) and k not in ('res', 'mRes')]
    if not gaps or not pool:
        return None, []
    lv = int(d.get('level') or 1)
    same = [p for p in pool if family(p) == family(d)] or pool
    donor = min(same, key=lambda p: abs(int(p.get('level') or 1) - lv))
    got = [k for k in gaps if donor.get(k)]
    for k in got:
        d[k] = donor[k]
    return str(donor.get('spriteName') or donor.get('id')).upper(), got


def convert(mid, d, nv, items, report, pool=()):
    donor, filled = fill_gaps(d, pool)
    n = nv.get(mid)
    name = (n and (ascii_name(n[0]) or EN_NAMES.get(n[0].strip()))) or ascii_name(d.get('name')) \
        or EN_NAMES.get(str(d.get('name') or '').strip()) or ''
    aegis = (d.get('spriteName') or (n and n[1]) or '').upper()
    if not aegis:
        report['no AegisName (skipped)'].append(mid)
        return None, []
    if not name:
        name = aegis_name(aegis)
    if len(name) > 23:                        # rAthena's NAME_LENGTH - 1
        small = ('of', 'in', 'the', 'a', 'an', 'and', 'on', 'at', 'to')
        # drop leading words first (they're usually adjectives: "Awakened Yordos Executor"), so
        # variants stay apart; else cut the end at a word
        short = name
        while len(short) > 23 and ' ' in short and short.split(' ', 2)[1].lower() not in small:
            short = short.split(' ', 1)[1]
        if len(short) > 23:
            short = name[:24].rsplit(' ', 1)[0] if ' ' in name[:24] else name[:23]
            while short.rsplit(' ', 1)[-1].lower() in small and ' ' in short:
                short = short.rsplit(' ', 1)[0]
        short = short[:1].upper() + short[1:]
        report['names shortened to 23 characters'].append('%d %s -> %s' % (mid, name, short))
        name = short
        report['no English name (AegisName used)'].append('%d %s' % (mid, aegis))
    lv = int(d.get('level') or (n and n[2]) or 1)
    st = {k: int(d.get(k) or 1) for k in ('str', 'agi', 'vit', 'int', 'dex', 'luk')}
    atk = mid_range(d.get('attackRange'))
    matk = mid_range(d.get('magicAttackRange'))
    race = RACES.get(key(d.get('race'))) or (n and RACE_IDS[n[5]] if n and n[5] < 10 else 'Formless')
    ele_s = re.sub(r'\s*\d+$', '', str(d.get('element') or ''))
    element = ELEMENTS.get(key(ele_s)) or (ELEMENT_IDS[n[3]] if n and n[3] < 10 else 'Neutral')
    ele_lv = int(d.get('elementLevel') or (n and n[4]) or 1)
    size = SIZES.get(key(d.get('size'))) or (SIZE_IDS[n[6]] if n and n[6] < 3 else 'Medium')
    ai = ''
    for f in d.get('aiFlags') or []:
        m = re.match(r'MONSTER_TYPE_(\d+)', str(f))
        if m:
            ai = m.group(1).zfill(2)
    mvp = bool(d.get('mvpDrops')) or str(d.get('type', '')).lower() == 'mvp'
    boss = mvp or str(d.get('type', '')).lower() == 'boss'
    amotion = attack_motion(d)

    L = ['  - Id: %d' % mid, '    AegisName: %s' % aegis, '    Name: %s' % yq(name)]
    if filled:
        L.append('    # Divine Pride shows no %s; copied from %s' % (', '.join(filled), donor))
        report['monsters with values copied from a similar monster'].append('%d %s <- %s' % (mid, aegis, donor))
    L += [
         '    Level: %d' % lv, '    Hp: %d' % int(d.get('health') or 1)]
    for f, k in (('BaseExp', 'baseExperience'), ('JobExp', 'jobExperience')):
        if d.get(k):
            L.append('    %s: %d' % (f, int(d[k])))
    if mvp and d.get('baseExperience'):
        L.append('    MvpExp: %d' % (int(d['baseExperience']) // 2))
    if atk is not None:
        L.append('    Attack: %d' % max(0, rnd(atk - st['str'] - lv)))
    if matk is not None:
        L.append('    Attack2: %d' % max(0, rnd(matk - st['int'] - lv)))
    for f, k in (('Defense', 'def'), ('MagicDefense', 'mDef'), ('Resistance', 'res'),
                 ('MagicResistance', 'mRes')):
        if d.get(k):
            L.append('    %s: %d' % (f, int(d[k])))
    for k in ('str', 'agi', 'vit', 'int', 'dex', 'luk'):
        L.append('    %s: %d' % (k.capitalize(), st[k]))
    L += ['    AttackRange: %d' % int(d.get('range') or 1), '    SkillRange: 10', '    ChaseRange: 12',
          '    Size: %s' % size, '    Race: %s' % race, '    Element: %s' % element,
          '    ElementLevel: %d' % max(1, min(4, ele_lv)),
          '    WalkSpeed: %d' % max(20, min(1000, CALIBRATION['walk'](d.get('speed')))),
          '    AttackDelay: %d' % rnd(amotion * CALIBRATION['delay_per_motion']),
          '    AttackMotion: %d' % amotion, '    DamageMotion: %d' % CALIBRATION['damage_motion']]
    if not ai:
        # Divine Pride shows no AI for most new monsters: rAthena's own (Id 20000+) are mostly 04
        # (aggressive), bosses 21, plants (a few HP) 06
        ai = '06' if int(d.get('health') or 0) <= 20 else '21' if boss else '04'
    if ai in AI_CODES and ai != '06':           # 06 is the default
        L.append('    Ai: %s' % ai)
    if boss:
        L.append('    Class: Boss')

    def drops(rows, label):
        out = []
        for r in rows or []:
            a = items.get(int(r.get('itemId') or 0))
            if not a:
                report['drops whose item the item db lacks (left out)'].append('%d item %s' % (mid, r.get('itemId')))
                continue
            rate = max(1, min(10000, rnd(float(r.get('probability') or 0) * 10000)))
            e = ['      - Item: %s' % a, '        Rate: %d' % rate]
            if r.get('isStealProtected'):
                e.append('        StealProtected: true')
            out += e
        return ['    %s:' % label] + out if out else []
    L += drops(d.get('mvpDrops'), 'MvpDrops')
    L += drops(d.get('drops'), 'Drops')
    return '\n'.join(L), skills(mid, aegis, d, report)


SKILL_NAMES = {}
EN_NAMES = {}


def skill_names(root):
    text = open(os.path.join(root, 'db', 're', 'skill_db.yml'), encoding='utf-8').read()
    return {int(i): n for i, n in re.findall(r'^  - Id: (\d+)\n    Name: (\S+)', text, re.M)}


def skills(mid, aegis, d, report):
    rows, seen = [], set()
    for s in d.get('skills') or []:
        s = dict(s, skillName=SKILL_NAMES.get(int(s.get('skillId') or 0)) or s.get('skillName'))
        if int(s.get('skillId') or 0) not in SKILL_NAMES:
            report['skill rows whose skill the skill db lacks (left out)'].append('%d %s' % (mid, s.get('skillId')))
            continue
        state = STATES.get(key(s.get('state')))
        if not state or not s.get('skillId'):
            report['skill rows with a state rAthena lacks (left out)'].append('%d %s %s' % (mid, s.get('skillName'), s.get('state')))
            continue
        cond, cval = condition(s.get('condition'))
        if cond is None:
            report['skill rows with an unread condition (left out)'].append('%d %s "%s"' % (mid, s.get('skillName'), s.get('condition')))
            continue
        sig = (state, s['skillId'], s.get('skillLevel'), cond, cval)
        if sig in seen:
            continue
        seen.add(sig)
        target = 'friend' if cond.startswith('friend') else 'self' if is_self(s.get('skillName')) else 'target'
        rows.append('%d,%s@%s,%s,%d,%d,%d,%d,%d,%s,%s,%s,%s,,,,,,,' % (
            mid, aegis, s.get('skillName'), state, s['skillId'], int(s.get('skillLevel') or 1),
            max(1, rnd(float(s.get('probabilityPercent') or 0) * 100)), int(s.get('castTime') or 0),
            int(s.get('cooldownTime') or 0), 'yes' if s.get('interruptable') else 'no', target, cond, cval))
    return rows


SELF_SKILLS = re.compile(r'^(NPC_(POWERUP|AGIUP|DEFENDER|BARRIER|INVINCIBLE|STONESKIN|ANTIMAGIC|SPEEDUP|'
                         r'REBIRTH|SUMMONSLAVE|SUMMONMONSTER|CALLSLAVE|RELIEVE_ON|RELIEVE_OFF|EMOTION|'
                         r'TRANSFORMATION|METAMORPHOSIS|HALLUCINATION.*|INVISIBLE|RUN|CHANGE.*|'
                         r'MAXPAIN|ALLHEAL|MAGICMIRROR|SELFDESTRUCTION|KEEPING|INVINCIBLEOFF)|'
                         r'AL_HEAL|AL_TELEPORT|AS_CLOAKING|MO_EXPLOSIONSPIRITS|CR_REFLECTSHIELD|'
                         r'MG_SIGHT|KN_TWOHANDQUICKEN|LK_CONCENTRATION|AL_INCAGI|PR_KYRIE|ST_PRESERVE|'
                         r'.*_REFLECT.*|SM_ENDURE|BS_ADRENALINE|CR_AUTOGUARD|HW_MAGICPOWER)$')


def is_self(name):
    return bool(SELF_SKILLS.match(str(name or '')))


# Divine Pride's condition codes (Aegis names) -> rAthena condition and its usual value
COND_CODES = {
    'slavenum': ('slavelt', '1'), 'comradehp': ('friendhpltmaxrate', '60'),
    'comradestatus': ('friendstatuson', 'anybad'), 'rangeattacked': ('longrangeattacked', '0'),
    'magiclocked': ('casttargeted', '0'), 'rudeattacked': ('rudeattacked', '0'),
    'skillused': ('skillused', '0'), 'castattacked': ('casttargeted', '0'),
    'attackedbymany': ('attackpcge', '2'), 'closedattacked': ('closedattacked', '0'),
    'longrangeattacked': ('longrangeattacked', '0'), 'masterattacked': ('masterattacked', '0'),
    'masterhp': ('masterhpltmaxrate', '50'), 'skilluse': ('skillused', '0'), '': ('always', '0'),
    'groundattackcheck': ('groundattacked', '0'), 'onspawn': ('onspawn', '0'), 'alchemist': ('alchemist', '0'),
    'groundattacked': ('groundattacked', '0'), 'damagedgt': ('damagedgt', '1000'),
    'mystatuson': ('mystatuson', 'anybad'), 'mystatusoff': ('mystatusoff', 'anybad'),
}


def condition(text):
    """Divine Pride's condition text -> (rAthena condition, value); None when unread."""
    t = str(text or '').strip().lower()
    if not t or t == '0':
        return 'always', '0'
    if re.sub(r'[^a-z]', '', t) in COND_CODES:
        return COND_CODES[re.sub(r'[^a-z]', '', t)]
    m = re.search(r'health drops below (\d+)\s*%', t)
    if m:
        return 'myhpltmaxrate', m.group(1)
    if "can't reach" in t:
        return 'rudeattacked', '0'
    m = re.search(r'nearby players are at least (\d+)', t)
    if m:
        return 'attackpcge', m.group(1)
    m = re.match(r'(?:when )?(?:my )?hp\s*(?:is\s*)?(?:<=?|below|under|less than)\s*(\d+)\s*%', t)
    if m:
        return 'myhpltmaxrate', m.group(1)
    m = re.match(r'(?:when )?(?:my )?hp\s*(?:is\s*)?(?:>=?|above|over)\s*(\d+)\s*%', t)
    if m:
        return 'myhpinrate', m.group(1)
    if 'slave' in t and re.search(r'(no|0|less|<)', t):
        m = re.search(r'(\d+)', t)
        return 'slavelt', m.group(1) if m else '1'
    if 'cast' in t or 'skill used' in t:
        return 'skillused', '0'
    if 'rude' in t:
        return 'rudeattacked', '0'
    if 'closed' in t or 'surrounded' in t:
        m = re.search(r'(\d+)', t)
        return 'attackpcge', m.group(1) if m else '2'
    if 'long range' in t:
        return 'longrangeattacked', '0'
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('records', help='folder with the Divine Pride monster records ({id}.json)')
    ap.add_argument('--ids', required=True, help='JSON list (or {"missing": [...]}) of the monster ids to add')
    ap.add_argument('--rathena', default=os.path.join(HERE, '..', '..'))
    ap.add_argument('--iro-navi', help='navi_mob_us.lub')
    ap.add_argument('--kro-navi', help='navi_mob_krpri.lub')
    ap.add_argument('--navi-en', action='append', default=[],
                    help="ROenglishRE's SystemEN/Navi_Data.lub (English for kRO's navigation names)")
    ap.add_argument('--report')
    a = ap.parse_args()
    root = os.path.abspath(a.rathena)
    ids = json.load(open(a.ids))
    ids = ids['missing'] if isinstance(ids, dict) else ids
    nv = navi_mobs([(p, e) for p, e in ((a.iro_navi, 'latin-1'), (a.kro_navi, 'cp949')) if p])
    items = item_names(root)
    SKILL_NAMES.update(skill_names(root))
    EN_NAMES.update(english_names(a.navi_en))
    have = mob_db(root)
    report = {k: [] for k in ('no record', 'already in the monster db', 'no AegisName (skipped)',
                              'AegisName already used (skipped)', 'no English name (AegisName used)',
                              'monsters with values copied from a similar monster',
                              'names shortened to 23 characters',
                              'skill rows whose skill the skill db lacks (left out)',
                              'drops whose item the item db lacks (left out)',
                              'skill rows with a state rAthena lacks (left out)',
                              'skill rows with an unread condition (left out)')}
    used = {aegis for aegis, _ in have.values()}
    ours = os.path.join(root, 'db', 're', 'mob_db_world.yml')
    entries, skill_rows = [], []
    pool = [json.load(open(p)) for p in glob.glob(os.path.join(a.records, '*.json'))]
    pool = [d for d in pool if complete(d)]
    for mid in sorted(set(ids)):
        if mid in have:
            report['already in the monster db'].append(mid)
            continue
        p = os.path.join(a.records, '%d.json' % mid)
        if not os.path.exists(p):
            report['no record'].append(mid)
            continue
        e, sk = convert(mid, json.load(open(p)), nv, items, report, pool)
        if not e:
            continue
        aegis = re.search(r'AegisName: (\S+)', e).group(1)
        if aegis in used:
            report['AegisName already used (skipped)'].append('%d %s' % (mid, aegis))
            continue
        used.add(aegis)
        entries.append(e)
        skill_rows += sk

    head = open(os.path.join(root, 'db', 're', 'mob_db.yml'), encoding='utf-8').read()
    version = re.search(r'Header:\n  Type: MOB_DB\n  Version: (\d+)', head).group(1)
    head = head[:head.index('Header:')]
    with open(ours, 'w', encoding='utf-8', newline='\n') as o:
        o.write(head.split('###########################################################################\n')[0])
        o.write('###########################################################################\n'
                '# World Sync Monster Database\n'
                '###########################################################################\n'
                '#\n'
                '# GENERATED by tools/world_sync/divine_pride.py. Do not edit by hand.\n'
                '#\n'
                '# Monsters the official clients place on their maps that the monster db\n'
                '# lacked, from Divine Pride\'s iRO records and the clients\' names.\n'
                '#\n'
                '###########################################################################\n'
                '\nHeader:\n  Type: MOB_DB\n  Version: %s\n\nBody:\n' % version)
        o.write('\n'.join(entries) + '\n')

    sk_path = os.path.join(root, 'db', 're', 'mob_skill_db.txt')
    text = open(sk_path, encoding='utf-8', errors='surrogateescape').read()
    mark = '\n// World sync (GENERATED by tools/world_sync/divine_pride.py, Divine Pride iRO records)\n'
    text = text.split(mark)[0].rstrip('\n') + '\n'
    if skill_rows:
        text += mark + '\n'.join(skill_rows) + '\n'
    open(sk_path, 'w', encoding='utf-8', errors='surrogateescape', newline='\n').write(text)

    print('monsters %d, skill rows %d' % (len(entries), len(skill_rows)))
    for k, v in report.items():
        if v:
            print('  %s: %d' % (k, len(v)))
    if a.report:
        json.dump(report, open(a.report, 'w'), indent=1)


if __name__ == '__main__':
    main()
