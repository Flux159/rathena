#!/usr/bin/env python3
"""Build npc/re/world_sync/rune_tablet.txt: kRO's Rune Tablet system as an NPC.

The client window (Rune Tablet / Rune Book / Decomposition) needs packets the
server doesn't have, so the same system runs through NPC menus, driven by the
client's own tables (System/Rune, read from ROenglishRE's SystemEN/Rune for the
English names):

  Rune Book      register a rune by handing in its materials
                 (Runetbl_info -> RuneTable_itemList)
  Rune Tablet    a set is the runes in its slots (RuneSettbl_info); its
                 bonuses grow with the runes registered ([2] = 2 runes) and its
                 upgrade level (+1..+15, "For each N Refine Levels"); upgrades
                 cost RuneTable_itemList[RuneSet_UpGradeList[level]] and succeed
                 at GradeTable[table][level] / 100000
  Decomposition  itemDecom_tbl: an item (one, or 30 at once) gives
                 itemDecomType_tbl's materials (min~max at chance / 100000)
  Rewards        RuneSettbl_Reward: activation, per-rune-count and completion
                 reward items, handed out once

GradeTable_Fail is read as the chance added to the next attempt after each
failure (it is 0 exactly where the success chance is already 100%); the
client doesn't say, so this is a stated reading. Set bonuses come from the
English descriptions through tools/regional_items/bonus.py; lines it can't
read are left out and listed. The bonuses are applied by one permanent
bonus_script per character that calls F_RuneTabletBonus.
"""
import argparse
import collections
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(1, os.path.join(HERE, '..', 'regional_items'))
import luadata  # noqa: E402
import bonus  # noqa: E402

TAGS = {17: 'Episode 17', 18: 'Episode 18', 19: 'Episode 19', 20: 'Episode 20', 21: 'Episode 21',
        22: 'Chapter 1', 500: 'Event'}


class SetCtx(bonus.Ctx):
    """'Refine level' in a set description is the set's upgrade level."""

    def refine(self):
        return '.@lv'

    def grade(self):
        return '0'


def text_tables(path, ids):
    """A ROenglishRE text table whose keys are RUNE*IDTBL.<name>: swap in the numbers, then read it."""
    import importlib.util
    spec = importlib.util.spec_from_file_location('ri_luadata', os.path.join(HERE, '..', 'regional_items', 'luadata.py'))
    ri = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ri)
    text = open(path, encoding='utf-8', errors='replace').read()
    text = re.sub(r'\b(RUNE(?:SET|TAG)?IDTBL)\.(\w+)', lambda m: str(ids.get(m.group(1), {}).get(m.group(2), 0)), text)
    with tempfile.NamedTemporaryFile('w', suffix='.lua', delete=False, encoding='utf-8') as t:
        t.write(text)
    try:
        return ri.run_text(t.name, 'utf-8')
    finally:
        os.unlink(t.name)


def lines_of(t):
    return [t[k] for k in sorted(t)] if isinstance(t, dict) else list(t or [])


def esc(s):
    return str(s).replace('\\', '\\\\').replace('"', '\\"')


RACE = {'formless': 'RC_Formless', 'undead': 'RC_Undead', 'brute': 'RC_Brute', 'plant': 'RC_Plant',
        'insect': 'RC_Insect', 'fish': 'RC_Fish', 'demon': 'RC_Demon', 'demi-human': 'RC_DemiHuman',
        'demihuman': 'RC_DemiHuman', 'angel': 'RC_Angel', 'dragon': 'RC_Dragon', 'every': 'RC_All', 'all': 'RC_All'}
SIZE = {'small': 'Size_Small', 'medium': 'Size_Medium', 'large': 'Size_Large', 'every': 'Size_All', 'all': 'Size_All'}
ELE = {'neutral': 'Ele_Neutral', 'water': 'Ele_Water', 'earth': 'Ele_Earth', 'fire': 'Ele_Fire', 'wind': 'Ele_Wind',
       'poison': 'Ele_Poison', 'holy': 'Ele_Holy', 'shadow': 'Ele_Dark', 'dark': 'Ele_Dark', 'ghost': 'Ele_Ghost',
       'undead': 'Ele_Undead', 'every': 'Ele_All', 'all': 'Ele_All'}
CLASS = {'boss': 'Class_Boss', 'normal': 'Class_Normal', 'every': 'Class_All', 'all': 'Class_All'}
REGION = {'devoured geffenia': 'RC2_ENCROACHED_GEPHENIA', 'encroached geffenia': 'RC2_ENCROACHED_GEPHENIA',
          'destroyed valkyrie realm': 'RC2_DESTROYED_VALKYRIE_REALM'}


def words(s):
    return [w.strip().lower() for w in re.split(r',\s*|\s+and\s+', s) if w.strip()]


def rune_effect(sent):
    """The phrasings of the rune set descriptions bonus.py doesn't know. None when not one of them."""
    t = re.sub(r'[^\x20-\x7e]', '', sent).strip().rstrip('.').strip()
    out = []
    # "... +20% Physical Damage against ..." (two sentences without a full stop)
    parts = re.split(r'(?<=%)\s+(?=[A-Z])', t)
    if len(parts) > 1:
        for p in parts:
            r = rune_effect(p)
            if r is None:
                return None
            out += r
        return out
    m = re.fullmatch(r'SP Consumption ([+-]\d+)%', t, re.I)
    if m:
        return ['bonus bUseSPrate,%d;' % int(m.group(1))]
    m = re.fullmatch(r'(Normal )?Physical Damage ([+-]\d+)%', t, re.I)
    if m:
        return ['bonus2 bAddClass,%s,%d;' % ('Class_Normal' if m.group(1) else 'Class_All', int(m.group(2)))]
    m = re.fullmatch(r'Resistance against attacks of every element ([+-]\d+)%', t, re.I)
    if m:
        return ['bonus2 bSubEle,Ele_All,%d;' % int(m.group(1))]
    m = re.fullmatch(r'Physical attacks have a (\d+)% chance to absorb (\d+)% of the damage dealt as (HP|SP)', t, re.I)
    if m:
        return ['bonus2 b%sDrainRate,%d,%d;' % (m.group(3).upper(), int(m.group(1)) * 10, int(m.group(2)))]
    m = re.fullmatch(r'Magical Damage with (.+?) elements? ([+-]\d+)%', t, re.I)
    if m and all(w in ELE for w in words(m.group(1))):
        return ['bonus2 bMagicAtkEle,%s,%d;' % (ELE[w], int(m.group(2))) for w in words(m.group(1))]
    m = re.fullmatch(r'(Physical |Magical )?Damage against monsters in (.+?) ([+-]\d+)%', t, re.I)
    if m and m.group(2).lower() in REGION:
        b = 'bMagicAddRace2' if (m.group(1) or '').lower().startswith('magical') else 'bAddRace2'
        return ['bonus2 %s,%s,%d;' % (b, REGION[m.group(2).lower()], int(m.group(3)))]
    # "Damage against monsters of Plant race +2% and of Insect race +3%"
    m = re.fullmatch(r'(Physical |Magical )?Damage against monsters of (.+)', t, re.I)
    if m:
        magic = (m.group(1) or '').lower().startswith('magical')
        for chunk in re.split(r'\s+and of\s+', m.group(2)):
            mm = re.fullmatch(r'(.+?) (race|size|element|class) ([+-]\d+)%', chunk.strip(), re.I)
            if not mm:
                return None
            kind, val = mm.group(2).lower(), int(mm.group(3))
            table, ph, mg = {'race': (RACE, 'bAddRace', 'bMagicAddRace'), 'size': (SIZE, 'bAddSize', 'bMagicAddSize'),
                             'element': (ELE, 'bAddEle', 'bMagicAddEle'),
                             'class': (CLASS, 'bAddClass', 'bMagicAddClass')}[kind]
            for w in words(mm.group(1)):
                if w not in table:
                    return None
                out.append('bonus2 %s,%s,%d;' % (mg if magic else ph, table[w], val))
        return out
    return None


def translate(desc_lines, ctx, report, where):
    """One [N] block of a set description -> rAthena statements."""
    text = ' '.join(str(x) for x in desc_lines)
    out = []
    for para in re.split(r'_{5,}', text):
        pending = ([], [])
        for sent in re.split(r'(?<=[.:])\s+', para.strip()):
            sent = sent.strip().rstrip(',')
            if not sent:
                continue
            sent = re.sub(r'^For each (\d+) Refine Levels?:?', r'Every \1 refine levels:', sent, flags=re.I)
            sent = re.sub(r'^For each Refine Level:?', 'Every 1 refine levels:', sent, flags=re.I)
            sent = re.sub(r'\bby ([+-]\d)', r'\1', sent)
            m = re.fullmatch(r'Refine Levels? \+(\d+):?', sent, re.I)
            if m:                                   # "+15:" -> from that upgrade level on
                pending = (['.@lv >= %d' % int(m.group(1))], pending[1])
                continue
            mult = pending[1]
            m = re.match(r'(Every \d+ refine levels:)\s*(.+)$', sent, re.I)
            if m:                                   # condition and effect in one sentence
                _, pending, _ = bonus.translate_line(m.group(1), ctx, pending)
                mult, sent = pending[1], m.group(2)
            own = rune_effect(sent)
            if own is None and ',' in sent:          # "P.ATK +2, SP Consumption -5%."
                own = []
                for part in [x for x in re.split(r',\s*', sent.rstrip('.')) if x]:
                    r = rune_effect(part)
                    if r is None:
                        st, _, ok = bonus.translate_line(part + '.', ctx, ([], []))
                        r = st if ok == 'ok' else None
                    if r is None:
                        own = None
                        break
                    own += r
            if own is not None:
                if pending[0]:
                    own = ['if (%s) %s' % (' && '.join(pending[0]), st) for st in own]
                if mult:
                    own = [re.sub(r',(-?\d+);$', lambda x: ',%s*%s;' % (x.group(1), '*'.join(mult)), st) for st in own]
                out += own
                continue
            stmts, pending, status = bonus.translate_line(sent, ctx, pending)
            if status == 'ok':
                out += stmts
            elif status == 'unknown':
                report['set bonus lines not understood (left out)'].append('%s: %s' % (where, sent))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rune_dir', help="the client's SystemEN/Rune folder (ROenglishRE)")
    ap.add_argument('--rathena', default=os.path.join(HERE, '..', '..'))
    ap.add_argument('--x', type=int, default=140)
    ap.add_argument('--y', type=int, default=190)
    a = ap.parse_args()
    root = os.path.abspath(a.rathena)
    R = a.rune_dir
    G = luadata.run([os.path.join(R, f + '.lub') for f in
                     ('runesystemid', 'rune_info', 'runeset_info', 'runeset_reward', 'runeSystem_table', 'itemDecom')],
                    'cp949')
    ids = {k: G.get(k) or {} for k in ('RUNEIDTBL', 'RUNESETIDTBL', 'RUNETAGIDTBL')}
    rdesc = text_tables(os.path.join(R, 'desc.lub'), ids).get('Runetbl_desc') or {}
    sdesc = text_tables(os.path.join(R, 'set_desc.lub'), ids).get('RuneSettbl_desc') or {}
    items = {}
    for f in os.listdir(os.path.join(root, 'db', 're')):
        if f.startswith('item_db') and f.endswith('.yml'):
            for i, n in re.findall(r'^  - Id: (\d+)\n    AegisName: \S+\n    Name: (.+)$',
                                   open(os.path.join(root, 'db', 're', f), encoding='utf-8').read(), re.M):
                items.setdefault(int(i), n.strip().strip('"'))
    skills = {}
    for i, n, d in re.findall(r'^  - Id: (\d+)\n    Name: (\S+)\n    Description: (.+)$',
                              open(os.path.join(root, 'db', 're', 'skill_db.yml'), encoding='utf-8').read(), re.M):
        skills[d.strip().lower()] = n
    ctx = SetCtx(skills)
    report = collections.defaultdict(list)
    itemlist = G.get('RuneTable_itemList') or {}

    def mats(idx):
        rows = lines_of(itemlist.get(idx))
        out = []
        for r in rows:
            iid, qty = r.get(1), r.get(2)
            if not iid:
                continue
            if iid not in items:
                report['materials the item db lacks'].append(str(iid))
                return None
            out.append('%d,%d' % (iid, qty))
        return ';'.join(out)

    # runes
    runes = []
    for tag in sorted(G.get('Runetbl_info') or {}):
        for rid, r in sorted(G['Runetbl_info'][tag].items()):
            name = ((rdesc.get(tag) or {}).get(rid) or {}).get('Rune_DisplayName') or r.get('Rune_Res')
            m = mats(r.get('Rune_ActiveList'))
            if m is None:
                report['runes left out (a material the item db lacks)'].append(name)
                continue
            runes.append((rid, tag, name, m))
    rune_ids = {r[0] for r in runes}

    # sets
    sets, bonus_code = [], []
    rewards = G.get('RuneSettbl_Reward') or {}
    for tag in sorted(G.get('RuneSettbl_info') or {}):
        for sid, s in sorted(G['RuneSettbl_info'][tag].items()):
            d = (sdesc.get(tag) or {}).get(sid) or {}
            name = d.get('RuneSetDisplayName') or s.get('RuneSetRes')
            slots = [v for v in lines_of(s.get('RuneSet_SlotList')) if v]
            if not slots or any(v not in rune_ids for v in slots):
                report['sets left out (a rune is missing)'].append(name)
                continue
            ups = [mats(i) for i in lines_of(s.get('RuneSet_UpGradeList'))]
            if any(u is None for u in ups):
                report['sets left out (an upgrade material the item db lacks)'].append(name)
                continue
            t, tf = s.get('RuneSet_UpGrade_Percentage_table'), s.get('RuneSet_UpGrade_Percentage_table_Fail')
            rate = [str((G['GradeTable'].get(t) or {}).get(l, 0)) for l in range(1, 16)]
            fail = [str((G['GradeTable_Fail'].get(tf) or {}).get(l, 0)) for l in range(1, 16)]
            rw = [v for v in lines_of((rewards.get(tag) or {}).get(sid))] + [0] * 7
            rw = [v if v in items else 0 for v in rw[:7]]
            desc = d.get('RuneSetDescription') or {}
            text_lines, code = [], []
            for n in sorted(desc):
                block = lines_of(desc[n]) if isinstance(desc[n], dict) else list(desc[n])
                text_lines.append('^0000FF%d runes:^000000 %s' % (n, ' '.join(str(x) for x in block if not str(x).startswith('___'))))
                st = translate(block, ctx, report, name)
                if st:
                    code.append('\t\tif (.@c >= %d) {\n%s\n\t\t}' % (n, '\n'.join('\t\t\t' + x.replace('\n', '\n\t\t\t') for x in st)))
            sets.append((sid, tag, name, ','.join(map(str, slots)), '|'.join(ups), ','.join(rate), ','.join(fail),
                         ','.join(map(str, rw)), '|'.join(text_lines)))
            if code:
                bonus_code.append('\tcase %d:\n%s\n\t\tbreak;' % (sid, '\n'.join(code)))

    # decomposition
    dtypes = G.get('itemDecomType_tbl') or {}
    nums = G.get('itemDecomItemNum_tbl') or {1: 1, 2: 30}
    dec = []
    for iid, r in sorted((G.get('itemDecom_tbl') or {}).items()):
        if iid not in items:
            continue
        outs = []
        for k in (1, 2):
            rows = [x for x in lines_of(dtypes.get(r.get(k))) if x.get(1) in items]
            outs.append('|'.join('%d,%d,%d,%d' % (x[1], x[2], x[3], x[4]) for x in rows))
        if outs[0] or outs[1]:
            dec.append((iid, outs[0], outs[1]))

    write(root, a, runes, sets, dec, bonus_code, nums)
    print('runes %d, sets %d (with bonuses %d), decomposable items %d' % (len(runes), len(sets), len(bonus_code), len(dec)))
    for k, v in report.items():
        print('  %s: %d' % (k, len(v)))
        for x in sorted(set(v))[:200]:
            print('      ' + x)


def arr(name, values, quote):
    """setarray in chunks (a script line has a 128-argument limit)."""
    out = []
    for i in range(0, len(values), 100):
        chunk = values[i:i + 100]
        vals = ','.join(('"%s"' % esc(v)) if quote else str(v) for v in chunk)
        out.append('\tsetarray %s[%d],%s;' % (name, i, vals))
    return '\n'.join(out)


def write(root, a, runes, sets, dec, bonus_code, nums):
    p = os.path.join(root, 'npc', 're', 'world_sync', 'rune_tablet.txt')
    data = '\n'.join([
        arr('.r_id', [r[0] for r in runes], False), arr('.r_tag', [r[1] for r in runes], False),
        arr('.r_name$', [r[2] for r in runes], True), arr('.r_mat$', [r[3] for r in runes], True),
        arr('.s_id', [s[0] for s in sets], False), arr('.s_tag', [s[1] for s in sets], False),
        arr('.s_name$', [s[2] for s in sets], True), arr('.s_slot$', [s[3] for s in sets], True),
        arr('.s_up$', [s[4] for s in sets], True), arr('.s_rate$', [s[5] for s in sets], True),
        arr('.s_fail$', [s[6] for s in sets], True), arr('.s_rw$', [s[7] for s in sets], True),
        arr('.s_desc$', [s[8] for s in sets], True),
        arr('.d_id', [d[0] for d in dec], False), arr('.d_one$', [d[1] for d in dec], True),
        arr('.d_many$', [d[2] for d in dec], True),
        '\tsetarray .tag[0],%s;' % ','.join(str(t) for t in sorted(TAGS)),
        '\tsetarray .tag_name$[0],%s;' % ','.join('"%s"' % TAGS[t] for t in sorted(TAGS)),
        '\t.many = %d;' % (nums.get(2) or 30),
    ])
    body = open(os.path.join(HERE, 'rune_tablet.tmpl.txt'), encoding='utf-8').read()
    body = body.replace('%X%', str(a.x)).replace('%Y%', str(a.y)).replace('%DATA%', data) \
               .replace('%BONUSES%', '\n'.join(bonus_code))
    open(p, 'w', encoding='utf-8', newline='\n').write(body)


if __name__ == '__main__':
    main()
