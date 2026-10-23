#!/usr/bin/env python3
"""Build db/re/item_db_regional.yml from official iRO / kRO client item tables.

The clients know thousands of items that rAthena's item_db doesn't: iRO's own
event and collaboration gear (the *_US items), and kRO's newest releases.
Without an item_db entry the server treats them as nonexistent: @item refuses
them and no script can hand them out.

This reads each client's itemInfo table (and ItemMoveInfoV5.txt for trade
rules), skips every Id rAthena already has, and writes an entry for the rest:
type, slots, location, weight, attack / defense, level, jobs, view and trade
flags from the description's fields, and a script translated from its effect
lines (see bonus.py). Set bonuses go to item_combos_regional.yml, box contents
become getitem scripts. Lines it can't translate stay in the output as YAML
comments, next to the item, so they can be finished by hand.

Run it again after an upstream merge: Ids upstream has added in the meantime
are skipped, so their official entries take over.

Example:
  tools/regional_items/generate.py \\
    --iro "iRO/System/iteminfo.lub" --iro "iRO/System/iteminfo_sak.lub" \\
    --iro-moveinfo iro/data/itemmoveinfov5.txt --iro-skills iro/skillinfoz \\
    --kro "kRO/data/luafiles514/lua files/datainfo/iteminfo.lub" \\
    --kro "kRO/System/itemInfo_true.lub" \\
    --kro-moveinfo kro/data/itemmoveinfov5.txt --kro-skills kro/skillinfoz
"""
import argparse
import collections
import difflib
import itertools
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bonus as B          # noqa: E402
import luadata             # noqa: E402

DEBUG = {int(x) for x in os.environ.get('REGIONAL_DEBUG', '').split(',') if x}
COLOR = re.compile(r'\^[0-9a-fA-F]{6}')
NAVI = re.compile(r'<NAVI>(.*?)<INFO>.*?</INFO></NAVI>', re.I)
TAGS = re.compile(r'</?(?:ITEM|ITEMLINK|INFO|URL|NAVI|TIPBOX)[^>]*>', re.I)

FIELD_KEYS = [
    'Type', 'Class', 'Item Class', 'Category', 'Series', 'Equipment Type', 'Defense', 'Def', 'DEF', 'Location', 'Position',
    'Equip Position', 'Compound on', 'Compound On', 'Weight', 'Required Level', 'Requires Level',
    'Require Level', 'Required Lv', 'Required LVL', 'Req LvL', 'Req Lv', 'Required Base Level', 'Attack Strength', 'Req. Level', 'Base Level', 'Level Requirement', 'Jobs', 'Job',
    'Usable By', 'Usable by', 'Equipped by', 'Equipable Jobs', 'Applicable Job', 'Attack', 'ATK',
    'MATK', 'Magic Attack', 'Weapon Level', 'Armor Level', 'Property', 'Element', 'Slot', 'Box Weight',
    '계열', '방어', '위치', '무게', '요구 레벨', '요구레벨', '장착', '공격', '마법공격', '마법 공격',
    '무기레벨', '무기 레벨', '방어구 레벨', '방어구레벨', '속성', '장착 위치', '착용 위치',
]
FIELD_RE = re.compile(r'(?:(?<=\s)|^)(%s)\s*:\s*' % '|'.join(
    re.escape(k) for k in sorted(FIELD_KEYS, key=len, reverse=True)), re.I)
FIELD_CANON = {k.lower(): k for k in FIELD_KEYS}

WEAPON_WORDS = [
    (r'two[- ]?handed sword|2h sword|양손 ?검', '2hSword'),
    (r'two[- ]?handed spear|2h spear|양손 ?창', '2hSpear'),
    (r'two[- ]?handed axe|2h axe|양손 ?도끼', '2hAxe'),
    (r'two[- ]?handed staff|2h staff|양손 ?지팡이', '2hStaff'),
    (r'one[- ]?handed sword|1h sword|^sword$|한손 ?검|^검$', '1hSword'),
    (r'one[- ]?handed spear|1h spear|^spear$|한손 ?창|^창$', '1hSpear'),
    (r'one[- ]?handed axe|1h axe|^axe$|한손 ?도끼|^도끼$', '1hAxe'),
    (r'one[- ]?handed staff|^staff$|^rod$|한손 ?지팡이|^지팡이$|^staves$', 'Staff'),
    (r'dagger|단검', 'Dagger'),
    (r'two[- ]?handed mace|양손 ?둔기|^mace$|둔기|메이스|^club$', 'Mace'),
    (r'^bow$|^활$', 'Bow'),
    (r'knuckle|claw|fist|너클|클로', 'Knuckle'),
    (r'instrument|musical|악기', 'Musical'),
    (r'^whip$|채찍', 'Whip'),
    (r'^book$|^책$', 'Book'),
    (r'katar|카타르', 'Katar'),
    (r'revolver|pistol|권총|리볼버', 'Revolver'),
    (r'rifle|라이플', 'Rifle'),
    (r'gatling|개틀링', 'Gatling'),
    (r'shotgun|샷건', 'Shotgun'),
    (r'grenade|그레네이드', 'Grenade'),
    (r'huuma|풍마', 'Huuma'),
]
AMMO_WORDS = [(r'arrow|화살', 'Arrow'), (r'bullet|탄환|불릿', 'Bullet'), (r'sphere|스피어', 'Grenade'),
              (r'shuriken|수리검', 'Shuriken'), (r'kunai|쿠나이', 'Kunai'), (r'cannon ?ball|캐논볼', 'CannonBall'),
              (r'throw', 'ThrowWeapon')]

SHADOW_LOC = {'weapon': 'Shadow_Weapon', '웨폰': 'Shadow_Weapon', 'armor': 'Shadow_Armor', '아머': 'Shadow_Armor',
              'shield': 'Shadow_Shield', '실드': 'Shadow_Shield', 'shoes': 'Shadow_Shoes', '슈즈': 'Shadow_Shoes',
              'earring': 'Shadow_Right_Accessory', '이어링': 'Shadow_Right_Accessory',
              'pendant': 'Shadow_Left_Accessory', '펜던트': 'Shadow_Left_Accessory'}
HEAD_LOC = {'upper': 'Head_Top', 'top': 'Head_Top', '상단': 'Head_Top', 'middle': 'Head_Mid', 'mid': 'Head_Mid',
            '중단': 'Head_Mid', 'lower': 'Head_Low', 'bottom': 'Head_Low', 'low': 'Head_Low', '하단': 'Head_Low'}
CARD_LOC = {'weapon': 'Right_Hand', '무기': 'Right_Hand', 'armor': 'Armor', '갑옷': 'Armor', 'shield': 'Left_Hand',
            '방패': 'Left_Hand', 'garment': 'Garment', '걸칠것': 'Garment', 'shoes': 'Shoes', 'footgear': 'Shoes',
            '신발': 'Shoes', 'headgear': 'Head_Top', '투구': 'Head_Top', 'accessory': 'Both_Accessory',
            '액세서리': 'Both_Accessory', '액세서리(오른쪽)': 'Right_Accessory', '액세서리(왼쪽)': 'Left_Accessory'}
SLOT_EQI = {'Head_Top': 'EQI_HEAD_TOP', 'Head_Mid': 'EQI_HEAD_MID', 'Head_Low': 'EQI_HEAD_LOW',
            'Armor': 'EQI_ARMOR', 'Right_Hand': 'EQI_HAND_R', 'Both_Hand': 'EQI_HAND_R', 'Left_Hand': 'EQI_HAND_L',
            'Garment': 'EQI_GARMENT', 'Shoes': 'EQI_SHOES', 'Right_Accessory': 'EQI_ACC_R',
            'Left_Accessory': 'EQI_ACC_L', 'Both_Accessory': 'EQI_ACC_R', 'Shadow_Weapon': 'EQI_SHADOW_WEAPON',
            'Shadow_Armor': 'EQI_SHADOW_ARMOR', 'Shadow_Shield': 'EQI_SHADOW_SHIELD',
            'Shadow_Shoes': 'EQI_SHADOW_SHOES', 'Shadow_Right_Accessory': 'EQI_SHADOW_ACC_R',
            'Shadow_Left_Accessory': 'EQI_SHADOW_ACC_L'}
ELE_FIELD = {'neutral': 'Ele_Neutral', 'water': 'Ele_Water', 'earth': 'Ele_Earth', 'fire': 'Ele_Fire',
             'wind': 'Ele_Wind', 'poison': 'Ele_Poison', 'holy': 'Ele_Holy', 'shadow': 'Ele_Dark',
             'dark': 'Ele_Dark', 'ghost': 'Ele_Ghost', 'undead': 'Ele_Undead', '수': 'Ele_Water',
             '지': 'Ele_Earth', '화': 'Ele_Fire', '풍': 'Ele_Wind', '독': 'Ele_Poison', '성': 'Ele_Holy',
             '암': 'Ele_Dark', '염': 'Ele_Ghost', '념': 'Ele_Ghost', '불사': 'Ele_Undead'}

# --- jobs: description wording -> rAthena Jobs / Classes ---------------------------
FAMILY = {
    'novice': ['Novice', 'SuperNovice'], 'swordman': ['Swordman', 'Knight', 'Crusader'],
    'mage': ['Mage', 'Wizard', 'Sage'], 'archer': ['Archer', 'Hunter', 'BardDancer'],
    'acolyte': ['Acolyte', 'Priest', 'Monk'], 'merchant': ['Merchant', 'Blacksmith', 'Alchemist'],
    'thief': ['Thief', 'Assassin', 'Rogue'], 'taekwon': ['Taekwon', 'StarGladiator', 'SoulLinker'],
    'ninja': ['Ninja', 'KagerouOboro'], 'gunslinger': ['Gunslinger', 'Rebellion'],
    'summoner': ['Summoner', 'Spirit_Handler'], 'druid': ['Druid', 'Karnos'],
}
# name -> (jobs, class restriction or None)
JOBNAMES = {}
for names, jobs, cls in [
    ('novice|노비스', ['Novice'], None), ('super novice|슈퍼 ?노비스', ['SuperNovice'], None),
    ('expanded super novice', ['SuperNovice'], 'All_Third'), ('hyper novice|하이퍼 ?노비스', ['SuperNovice'], 'Fourth'),
    ('swordman|swordsman|검사', ['Swordman'], None), ('mage|마법사', ['Mage'], None), ('archer|궁수', ['Archer'], None),
    ('acolyte|복사', ['Acolyte'], None), ('merchant|상인', ['Merchant'], None), ('thief|도둑', ['Thief'], None),
    ('knight|기사', ['Knight'], None), ('lord knight|로드 ?나이트', ['Knight'], 'All_Upper'),
    ('rune knight|룬 ?나이트', ['Knight'], 'All_Third'), ('dragon knight|드래곤 ?나이트', ['Knight'], 'Fourth'),
    ('crusader|크루세이더', ['Crusader'], None), ('paladin|팔라딘', ['Crusader'], 'All_Upper'),
    ('royal guard|로열 ?가드', ['Crusader'], 'All_Third'), ('imperial guard|임페리얼 ?가드', ['Crusader'], 'Fourth'),
    ('wizard|위저드', ['Wizard'], None), ('high wizard|하이 ?위저드', ['Wizard'], 'All_Upper'),
    ('warlock|워록', ['Wizard'], 'All_Third'), ('arch mage|archmage|아크 ?메이지', ['Wizard'], 'Fourth'),
    ('sage|세이지', ['Sage'], None), ('professor|프로페서', ['Sage'], 'All_Upper'),
    ('sorcerer|소서러', ['Sage'], 'All_Third'), ('elemental master|엘레멘탈 ?마스터', ['Sage'], 'Fourth'),
    ('hunter|헌터', ['Hunter'], None), ('sniper|스나이퍼', ['Hunter'], 'All_Upper'),
    ('ranger|레인저', ['Hunter'], 'All_Third'), ('wind hawk|windhawk|윈드 ?호크', ['Hunter'], 'Fourth'),
    ('bard|바드', ['BardDancer'], None), ('dancer|댄서', ['BardDancer'], None),
    ('clown|minstrel|gypsy|클라운|집시', ['BardDancer'], 'All_Upper'),
    ('maestro|wanderer|민스트럴|원더러', ['BardDancer'], 'All_Third'),
    ('troubadour|trouvere|troubadour&trouvere|트루바두르|트루베르', ['BardDancer'], 'Fourth'),
    ('priest|프리스트', ['Priest'], None), ('high priest|하이 ?프리스트', ['Priest'], 'All_Upper'),
    ('arch bishop|archbishop|아크 ?비숍', ['Priest'], 'All_Third'), ('cardinal|카디날', ['Priest'], 'Fourth'),
    ('monk|몽크', ['Monk'], None), ('champion|챔피언', ['Monk'], 'All_Upper'), ('sura|수라', ['Monk'], 'All_Third'),
    ('inquisitor|인퀴지터', ['Monk'], 'Fourth'),
    ('blacksmith|블랙스미스', ['Blacksmith'], None), ('whitesmith|mastersmith|화이트스미스', ['Blacksmith'], 'All_Upper'),
    ('mechanic|미케닉', ['Blacksmith'], 'All_Third'), ('meister|마이스터', ['Blacksmith'], 'Fourth'),
    ('alchemist|알케미스트', ['Alchemist'], None), ('creator|biochemist|크리에이터', ['Alchemist'], 'All_Upper'),
    ('genetic|geneticist|제네릭', ['Alchemist'], 'All_Third'), ('biolo|바이올로', ['Alchemist'], 'Fourth'),
    ('assassin|어세신', ['Assassin'], None), ('assassin cross|어세신 ?크로스', ['Assassin'], 'All_Upper'),
    ('guillotine cross|길로틴 ?크로스', ['Assassin'], 'All_Third'), ('shadow cross|쉐도우 ?크로스', ['Assassin'], 'Fourth'),
    ('rogue|로그', ['Rogue'], None), ('stalker|스토커', ['Rogue'], 'All_Upper'),
    ('shadow chaser|쉐도우 ?체이서', ['Rogue'], 'All_Third'), ('abyss chaser|어비스 ?체이서', ['Rogue'], 'Fourth'),
    ('taekwon|태권소년|태권소녀|태권', ['Taekwon'], None),
    ('star gladiator|taekwon master|권성', ['StarGladiator'], None), ('star emperor|성제', ['StarGladiator'], 'All_Third'),
    ('sky emperor|천제', ['StarGladiator'], 'Fourth'),
    ('soul linker|소울 ?링커', ['SoulLinker'], None), ('soul reaper|소울 ?리퍼', ['SoulLinker'], 'All_Third'),
    ('soul ascetic|혼령사', ['SoulLinker'], 'Fourth'),
    ('gunslinger|건슬링거', ['Gunslinger'], None), ('rebellion|rebel|리벨리온', ['Rebellion'], None),
    ('night watch|나이트 ?워치', ['Rebellion'], 'Third'),  # rAthena's Night Watch is a third class
    ('ninja|닌자', ['Ninja'], None), ('kagerou|oboro|카게로우|오보로', ['KagerouOboro'], None),
    ('shinkiro|shiranui|신키로|시라누이', ['KagerouOboro'], 'Third'),  # and so are Shinkiro / Shiranui
    ('summoner|doram|도람', ['Summoner'], None), ('spirit handler|영도사', ['Spirit_Handler'], None),
    ('druid|드루이드', ['Druid'], None), ('karnos|카르노스', ['Karnos'], None), ('alitea|알리테아', ['Karnos'], 'Fourth'),  # gear counts Alitea as fourth (pc_isItemClass)
]:
    for n in names.split('|'):
        for v in ({n.replace(' ?', ' '), n.replace(' ?', '')} if ' ?' in n else {n}):
            JOBNAMES[v] = (jobs, cls)
FAMILY_KO = {'검사': 'swordman', '마법사': 'mage', '궁수': 'archer', '복사': 'acolyte', '상인': 'merchant',
             '도둑': 'thief', '노비스': 'novice', '태권': 'taekwon', '닌자': 'ninja', '건슬링거': 'gunslinger',
             '도람': 'summoner', '드루이드': 'druid'}


def parse_jobs(text):
    """'Swordman classes, Rune Knight' -> ({job: True}, {class: True}) or None if unread."""
    if not text:
        return None
    t = text.lower().strip(' .,;')
    t = re.sub(r'\s+', ' ', t)
    if re.fullmatch(r'(?:all|every|any)(?: jobs?| class(?:es)?| characters?)?|전 ?직업|모든 직업|all jobs\.?', t):
        return {}, {}
    m = re.fullmatch(r'(?:all|every)(?: jobs?| class(?:es)?)? except (.+)', t)
    base_all, negate = False, []
    if m:
        base_all = True
        t = m.group(1)
    m = re.fullmatch(r'(3rd|third|4th|fourth|transcendent|rebirth)(?: jobs?| class(?:es)?)?(?: only)?', t)
    if m:
        c = {'3rd': 'All_Third', 'third': 'All_Third', '4th': 'Fourth', 'fourth': 'Fourth'}.get(m.group(1), 'All_Upper')
        return {}, {c: True}
    jobs, classes, any_unrestricted = [], set(), False
    for tok in re.split(r'\s*(?:,|/|;|\band\b|&(?!trouvere)|및|·)\s*', t):
        tok = tok.strip()
        if not tok:
            continue
        family = bool(re.search(r'\b(?:classes|class|jobs|line|series)\b|계열|직업군', tok))
        name = re.sub(r'\b(?:classes|class|jobs|job|only|line|series|types?)\b|계열|직업군|직업|만', '', tok).strip()
        name = re.sub(r'^(?:the|all)\s+', '', name)
        ko_family = FAMILY_KO.get(name)
        fam = name if name in FAMILY else ko_family
        if family and fam:
            jobs += FAMILY[fam]
            any_unrestricted = True
        elif name in JOBNAMES:
            j, c = JOBNAMES[name]
            jobs += j
            if c:
                classes.add(c)
            else:
                any_unrestricted = True
        elif fam:
            jobs += FAMILY[fam]
            any_unrestricted = True
        else:
            return None
    if base_all:
        return {'All': True, **{j: False for j in jobs}}, {}
    cls = {} if any_unrestricted or not classes else {c: True for c in sorted(classes)}
    return {j: True for j in dict.fromkeys(jobs)}, cls


# --- input -----------------------------------------------------------------------

def clean(s):
    s = COLOR.sub('', s or '')
    s = NAVI.sub(r'\1', s)
    s = TAGS.sub('', s)
    return s.replace('\r', '')


def desc_lines(entry):
    """Description as a list of lines; separators become None."""
    out = []
    for raw in luadata.ordered_lines(entry.get('identifiedDescriptionName')):
        if not isinstance(raw, str):
            continue
        for part in clean(raw).split('\n'):
            p = part.replace('\t', ' ').strip()
            if not p or re.fullmatch(r'[-_=─━.·\s]+', p):
                out.append(None)
            else:
                out.append(p)
    return out


def split_fields(lines):
    fields, rest = {}, []
    for l in lines:
        if l is None:
            rest.append(None)
            continue
        hits = list(FIELD_RE.finditer(l))
        if not hits:
            rest.append(l)
            continue
        pre = l[:hits[0].start()].strip()
        if pre:
            # "Type : Armor" is a field line; "When ... Defense: 5" is not
            if len(pre) > 25 or not re.fullmatch(r'[^\d:]{0,25}', pre):
                rest.append(l)
                continue
        for k, h in enumerate(hits):
            end = hits[k + 1].start() if k + 1 < len(hits) else len(l)
            fields.setdefault(FIELD_CANON.get(h.group(1).lower(), h.group(1)), l[h.end():end].strip(' ,'))
        if pre:
            fields.setdefault('_pre', pre)
    return fields, rest


def first(f, *keys):
    for k in keys:
        if f.get(k):
            return f[k]
    return None


def to_int(s):
    if s is None:
        return None
    m = re.search(r'-?\d+', s.replace(',', ''))
    return int(m.group()) if m else None


def load_moveinfo(path, enc):
    rules, aegis = {}, {}
    if not path:
        return rules, aegis
    with open(path, 'rb') as fh:
        for line in fh.read().decode(enc, 'replace').splitlines():
            m = re.match(r'\s*(\d+)\s+((?:\d\s+){7}\d)\s*(?://\s*(.*))?$', line)
            if not m:
                continue
            i = int(m.group(1))
            rules[i] = [int(x) for x in m.group(2).split()]
            c = (m.group(3) or '').strip()
            if re.fullmatch(r'[A-Za-z0-9_]+', c):
                aegis[i] = c
    return rules, aegis


def load_skills(d, enc):
    if not d:
        return {}
    G = luadata.run([os.path.join(d, 'skillid.lub'), os.path.join(d, 'skillinfolist.lub')], enc)
    inv = {v: k for k, v in (G.get('SKID') or {}).items()}
    out = {}
    for k, v in (G.get('SKILL_INFO_LIST') or {}).items():
        if isinstance(v, dict) and v.get('SkillName') and k in inv:
            out.setdefault(v['SkillName'].strip(), inv[k])
    return out


def load_rathena(root):
    items, by_name = {}, collections.defaultdict(list)
    for part in ('usable', 'equip', 'etc'):
        path = os.path.join(root, 'db', 're', 'item_db_%s.yml' % part)
        cur = None
        for line in open(path, encoding='utf-8'):
            m = re.match(r'  - Id: (\d+)', line)
            if m:
                cur = {'Id': int(m.group(1))}
                items[cur['Id']] = cur
                continue
            m = re.match(r'    (AegisName|Name|Type|SubType|Range|WeaponLevel): "?(.*?)"?\s*(?:#.*)?$', line)
            if m and cur is not None:
                cur[m.group(1)] = m.group(2)
            m = re.match(r'      (Right_Hand|Both_Hand): true', line)
            if m and cur is not None:
                cur['Hand'] = m.group(1)
            if line.startswith('    Locations:') and cur is not None:
                cur['_loc'] = True
            elif cur is not None and cur.get('_loc') is True:
                m = re.match(r'      (\w+): true', line)
                cur['_loc'] = m.group(1) if m else None
    for it in items.values():
        if it.get('Name'):
            by_name[name_key(it['Name'])].append(it['Id'])
    skills, skill_desc = set(), {}
    cur = None
    for line in open(os.path.join(root, 'db', 're', 'skill_db.yml'), encoding='utf-8'):
        m = re.match(r'    Name: (\S+)', line)
        if m:
            cur = m.group(1)
            skills.add(cur)
        m = re.match(r'    Description: "?(.*?)"?\s*$', line)
        if m and cur:
            prev = skill_desc.get(m.group(1))
            # the first skill with a name is the base one (SM_MAGNUM before
            # MS_MAGNUM, NW_THE_VIGILANTE_AT_NIGHT before its _GUN_ variants)
            if not prev or (prev.startswith(('NPC_', 'ALL_')) and not cur.startswith(('NPC_', 'ALL_'))):
                skill_desc[m.group(1)] = cur
    return items, by_name, skills, skill_desc


def name_key(s):
    s = clean(s)
    s = re.sub(r'\s*\[\d\]\s*$', '', s.strip())
    return re.sub(r'\s+', ' ', s).strip().lower()


# --- inference ---------------------------------------------------------------------

def weapon_subtype(word):
    w = word.strip().lower()
    for pat, sub in WEAPON_WORDS:
        if re.search(pat, w):
            return sub
    return None


def infer(item_id, entry, f, name, defaults):
    """-> dict with Type, SubType, Locations (list), or None for 'unknown'."""
    tword = (first(f, 'Type', 'Class', 'Item Class', 'Category', 'Series', 'Equipment Type', '계열') or '').strip()
    if tword.lower() in ('weapon', '무기', 'armor', 'equipment', 'equip') and f.get('Class') and f.get('Type'):
        if parse_jobs(f['Class']) is None:      # "Type: Weapon / Class: Dagger"
            tword = f['Class']
    tword = re.split(r'\s+(?:def(?:ense)?|atk|attack|방어|공격)\b', tword, flags=re.I)[0].strip()
    tl = tword.lower()
    loc = (first(f, 'Location', 'Position', 'Equip Position', '위치', '장착 위치', '착용 위치') or '').lower()
    loc = re.split(r'\s+(?:weight|무게)\b', loc)[0].strip()
    un = clean(entry.get('unidentifiedDisplayName') or '').strip()
    unl = un.lower()
    cls = entry.get('ClassNum') or 0
    nl = name.lower()

    def head_locs(prefix=''):
        locs = []
        for word, l in HEAD_LOC.items():
            if re.search(r'(?:^|[\s,/&-])%s(?:$|[\s,/&-])' % re.escape(word), ' %s ' % loc):
                locs.append(prefix + l)
        return list(dict.fromkeys(locs))

    if re.search(r'monster egg|cute pet|pet egg', tl):
        return {'Type': 'PetEgg', 'Locations': None}
    if re.search(r'pet (?:equipment|accessory|armor)|펫 장비|펫 액세서리', tl + ' ' + unl):
        return {'Type': 'PetArmor', 'Locations': None}
    # boxes that describe the equipment they hold
    if not cls and re.search(r'\b(?:box|package|crate|chest|pack|bundle|eggs?|balloon|bag|pouch|capsule)(?:\s*\(\d+\))?(?:\s+[ivx]+)?$', nl) \
            and not re.search(r'pet egg|monster egg', nl) \
            or re.search(r'(?:상자|패키지|꾸러미|교환권|알)(?:\s*\(\d+\))?(?:\s*[ivxⅠ-Ⅹ]+)?$', name):
        return {'Type': 'Usable', 'Locations': None}
    # cards and enchants
    if tl in ('card', '카드') or 'card' == tl or (4000 <= item_id < 5000) or 27000 <= item_id < 28000 \
            or 300000 <= item_id < 310000 and (tl in ('card', '카드') or nl.endswith(' card') or name.endswith('카드')):
        target = (first(f, 'Compound on', 'Compound On', '장착') or loc or '').lower().strip()
        l = None
        for word, slot in CARD_LOC.items():
            if word in target.replace(' ', '') or word in target:
                l = slot
                break
        if not l:
            return None
        return {'Type': 'Card', 'Locations': [l]}
    if 310000 <= item_id < 320000 or 29000 <= item_id < 30000 or tl in ('enchant', 'enchantment', '인챈트'):
        return {'Type': 'Card', 'SubType': 'Enchant', 'Locations': None}
    # shadow gear
    if 'shadow' in tl or '쉐도우' in tl or 'shadow' in unl and cls == 0 and 'equip' in unl \
            or re.search(r'(?:웨폰|아머|실드|슈즈|이어링|펜던트) 쉐도우$', un):
        for word, l in SHADOW_LOC.items():
            if word in loc or word in unl or un.endswith(word + ' 쉐도우'):
                return {'Type': 'ShadowGear', 'Locations': [l]}
        return None
    # costumes
    if 'costume' in tl or '의상' in tl or ('costume' in unl and cls) or (nl.startswith('costume') and cls):
        if 'garment' in loc or 'robe' in loc or '걸칠것' in loc or 'garment' in tl:
            return {'Type': 'Armor', 'Locations': ['Costume_Garment']}
        locs = head_locs('Costume_')
        return {'Type': 'Armor', 'Locations': locs} if locs else None
    # weapons
    sub = weapon_subtype(tword) if tword else None
    if not sub and cls and not tword:
        sub = weapon_subtype(un)
    if sub:
        hand = defaults.get(sub, {}).get('Hand', 'Right_Hand')
        return {'Type': 'Weapon', 'SubType': sub, 'Locations': [hand]}
    for pat, sub in AMMO_WORDS:
        if re.search(pat, tl) and tl:
            return {'Type': 'Ammo', 'SubType': sub, 'Locations': ['Ammo']}
    # armor pieces
    if re.search(r'head ?gear|helm|hat|cap|투구|모자', tl) or (not tl and re.search(r'\b(?:head ?gear|helm|cap|hat)\b|투구|모자|리본|머리띠', unl) and cls):
        locs = head_locs() or (['Head_Top'] if not loc else None)
        return {'Type': 'Armor', 'Locations': locs} if locs else None
    if not tl and re.search(r'^(?:upper|middle|lower) headgear$', unl):
        return {'Type': 'Armor', 'Locations': [HEAD_LOC[unl.split()[0]]]}
    if re.fullmatch(r'armou?r|body armou?r|갑옷', tl) or (not tl and unl in ('armor', '갑옷')):
        return {'Type': 'Armor', 'Locations': ['Armor']}
    if re.search(r'garment|robe|manteau|muffler|cape|outer|걸칠것|망토|머플러|로브', tl) or (not tl and re.search(r'^(?:garment|manteau|muffler|robe|걸칠것)$', unl)):
        return {'Type': 'Armor', 'Locations': ['Garment']}
    if re.search(r'shoes|footgear|boots|신발', tl) or (not tl and re.search(r'^(?:shoes|boots|footgear|신발)$', unl)):
        return {'Type': 'Armor', 'Locations': ['Shoes']}
    if re.search(r'shield|guard|방패', tl) or (not tl and re.search(r'^(?:shield|guard|buckler|방패)$', unl)):
        return {'Type': 'Armor', 'Locations': ['Left_Hand']}
    if re.search(r'accessory|액세서리|악세서리', tl) or (not tl and re.search(r'\b(?:accessory|ring)\b|액세서리|악세서리|반지', unl)):
        src = tl + ' ' + unl + ' ' + loc
        if 'right' in src or '오른' in src:
            return {'Type': 'Armor', 'Locations': ['Right_Accessory']}
        if 'left' in src or '왼' in src:
            return {'Type': 'Armor', 'Locations': ['Left_Accessory']}
        return {'Type': 'Armor', 'Locations': ['Both_Accessory']}
    if re.search(r'monster egg|cute pet|pet egg|알$', tl):
        return {'Type': 'PetEgg', 'Locations': None}
    if re.search(r'pet (?:equipment|accessory|armor)|펫 장비', tl):
        return {'Type': 'PetArmor', 'Locations': None}
    if cls and not tl:
        return None     # has a sprite but nothing says where it goes
    if re.search(r'usable|item giver|consumable|stat boost|scroll|box|package|restorative|recovery|special|소비|회복|상자|패키지|주문서', tl):
        return {'Type': 'Usable', 'Locations': None}
    return {'Type': 'Etc', 'Locations': None}


# --- boxes -------------------------------------------------------------------------

BOX_HEAD = re.compile(r'(?:contains?(?: the following| one of the following|:)|when opened,? (?:gives|obtains?|you (?:can )?(?:get|receive|obtain))|'
                      r'obtains? (?:all )?(?:of )?the following|(?:open|opening)(?: it| this)? to (?:receive|get|obtain)|'
                      r'(?:receive|get|obtain)s? (?:all )?(?:of )?the following|the following items?\s*:|open(?:ing)? (?:it|this|the box)[^,]* (?:gives|grants|obtains?)|'
                      r'(?:gives?|grants?|obtain|get) (?:you )?(?:randomly )?(?:one|1) of the following|randomly gives?|(?:one|1) of the following|'
                      r'you can (?:get|obtain|receive) (?:one of )?the following|following items? (?:can be|will be) obtained|'
                      r'다음 (?:아이템|중)|아래 (?:아이템|중)|중 하나를? (?:획득|얻)|획득할 수 있(?:다|는))', re.I)
NOTE = re.compile(r'^(?:\^?[0-9a-f]{0,6})?(?:account[- ]?bound|character[- ]?bound|cannot be|can not be|can\'t be|'
                  r'this (?:item|box)|item will be|items? (?:can|will)|.*will be (?:removed|deleted)|.*expires?\b|'
                  r'.*(?:event|promotion) (?:ends|is over|period)|duration|rental|note\b|\(.*\)$|'
                  r'.*거래|.*귀속|.*삭제|.*기간)', re.I)
RANDOM = re.compile(r'one of|random|chance|중 하나|랜덤|확률', re.I)


def _box_row(l2):
    """One line of a box's list -> (name, qty, weight). qty may be 'rand(a,b)'."""
    s = re.sub(r'^(?:[-•*·]|\d{1,3}[.)])\s*', '', l2).strip(' .,;')
    wm = re.search(r'\s*[:\-–(]?\s*(\d+(?:[.,]\d+)?)\s*%\s*\)?$', s)
    weight = None
    if wm:
        weight = float(wm.group(1).replace(',', '.'))
        s = s[:wm.start()].strip(' :-–')
    s = re.sub(r'\s*\((?:enchantable|random[^)]*|account[- ]bound|tradable)\)\s*$', '', s, flags=re.I)
    rm = re.search(r'\s+(\d+)\s*[-~]\s*(\d+)\s*(?:cnt|ea|pcs|x)?$', s)
    if rm:
        return s[:rm.start()].strip(), 'rand(%s,%s)' % (rm.group(1), rm.group(2)), weight
    pm = re.fullmatch(r'\((\d+)\)\s*(.+)', s)
    if pm:
        s = '%s x %s' % (pm.group(2), pm.group(1))
    m = re.fullmatch(r'(\d+)\s*(?:[xX×]|cnt|ea\b|pcs)\s*(.+)', s) or re.fullmatch(r'(\d+)\s+([A-Za-z\[].+)', s) \
        or re.fullmatch(r'(.+?)\s*[xX×]\s*(\d+)', s) or re.fullmatch(r'(.+?)\s+(\d+)\s*(?:ea|pcs|cnt|개)', s) \
        or re.fullmatch(r'(.+?)\s*\((\d+)\)', s)
    if m:
        a, b = m.groups()
        name, qty = (b, int(a)) if a.isdigit() else (a, int(b))
    else:
        name, qty = s, 1
    return name.strip(), qty, weight


def parse_box(lines):
    """-> [(random?, [(name, qty, weight)])], one per list in the description, or None."""
    for l in lines:
        if l is None:
            continue
        m = re.search(r'(?:box|package|crate|chest|pack)(?: that)? (?:contains?|containing|with)(?: with)? (\d+)\s*(?:x\s*)?(?:(?:boxes|units|pieces|pcs|ea)\b)?\s*(?:of )?(.+?)\.?$', l, re.I)
        if m and not re.search(r'\band\b|&|,', m.group(2)):
            return [(False, [(re.sub(r'^(?:the )?', '', m.group(2)), int(m.group(1)), None)])]
        m = re.search(r'(?:when opened,? )?gives? (?:you )?(?:one|1|an?) (.+?)\.?$', l, re.I)
        if m and not BOX_HEAD.search(l) and re.match(r'when opened', l, re.I):
            return [(False, [(m.group(1), 1, None)])]
    sections, cur = [], None
    for l in lines + [None]:
        if l is not None and BOX_HEAD.search(l):
            if cur and cur[1]:
                sections.append(cur)
            cur = (bool(RANDOM.search(l)), [])
            continue
        if cur is None:
            continue
        if l is None:
            if cur[1]:
                sections.append(cur)
                cur = None
            continue
        if re.match(r'(?:weight|type|class|무게|계열)\s*:', l, re.I) or (cur[1] and NOTE.search(l)):
            if cur[1]:
                sections.append(cur)
            cur = None
            continue
        if NOTE.search(l) or re.fullmatch(r'\[[^\]]*\]:?|[^:]{0,40}:', l.strip()):
            continue    # a note or a sub-heading before the list starts
        cur[1].append(_box_row(l))
    return sections or None


def box_section(i, name, rnd, got, resolve, aegis_of, unknown):
    """Script for one list of a box: everything, one at random, or a weighted
    draw. None when too much of it can't be found."""
    rows = [(resolve(nm), q, nm, wt) for nm, q, wt in got]
    rows = [r for r in rows if r[0] != i]
    if not rows:
        return None
    # a 100% row next to odds is a bonus every opening gives
    sure = [r for r in rows if r[3] and r[3] >= 99.5 and len(rows) > 1]
    pool = [r for r in rows if r not in sure]
    missing = [r[2] for r in rows if not r[0]]
    weighted = len(pool) > 1 and all(r[3] for r in pool)
    if not weighted and rnd and len(pool) > 4 and not any(r[3] for r in pool) \
            and re.search(r'overseas care|\bocp\b', name, re.I):
        pool = ocp_odds(pool)
        weighted = True
        unknown.append('odds: the box lists none; using the layout of the OCPs that do '
                       '(0.34%, 4x 1.01%, 3x 2.73%, 3x 6.83%, Party Hard Pack 9.56%, consumables share the rest)')
    if weighted:
        have = sum(r[3] for r in pool if r[0])
        ok = have >= 0.7 * sum(r[3] for r in pool) and all(r[0] for r in sure)
    else:
        ok = sum(1 for r in rows if r[0]) >= 0.7 * len(rows) and (rnd or not missing)
    if missing:
        unknown.append('box contents not found in the item db%s: %s' % (
            ' (left out of the draw)' if ok else '', ' | '.join(missing)))
    if not ok:
        return None
    out = []
    get = lambda x, q, nm: 'getitem %d,%s;    // %s' % (x, q, aegis_of.get(x, nm))
    for x, q, nm, wt in sure:
        out.append(get(x, q, nm))
    pool = [r for r in pool if r[0]]
    if weighted and len(pool) > 1:
        total = sum(r[3] for r in pool)
        out.append('.@r = rand(%d);' % int(round(total * 100)))
        acc = 0
        for k, (x, q, nm, wt) in enumerate(pool):
            acc += int(round(wt * 100))
            if k == len(pool) - 1:
                out.append('else ' + get(x, q, nm))
            else:
                out.append('%s (.@r < %d) %s' % ('if' if k == 0 else 'else if', acc, get(x, q, nm)))
    elif rnd and len(pool) > 1:
        out.append('switch (rand(%d)) {' % len(pool))
        for k, (x, q, nm, wt) in enumerate(pool):
            out.append('case %d: %s break;' % (k, get(x, q, nm)))
        out.append('}')
    else:
        for x, q, nm, wt in pool:
            out.append(get(x, q, nm))
    return out


OCP_TIERS = [0.34, 1.01, 1.01, 1.01, 1.01, 2.73, 2.73, 2.73, 6.83, 6.83, 6.83]


def ocp_odds(pool):
    """Odds for an Overseas Care Package that lists none, from the layout every
    later OCP prints: equipment from rarest to commonest, Party Hard Pack at
    9.56%, and the consumable stacks sharing what is left."""
    stacks = [r for r in pool if not isinstance(r[1], int) or r[1] > 1 or re.search(r'party hard pack', r[2], re.I)]
    gear = [r for r in pool if r not in stacks]
    out = {}
    for k, r in enumerate(gear):
        tier = OCP_TIERS[round(k * (len(OCP_TIERS) - 1) / max(len(gear) - 1, 1))]
        out[id(r)] = tier
    php = [r for r in stacks if re.search(r'party hard pack', r[2], re.I)]
    for r in php:
        out[id(r)] = 9.56
    rest = [r for r in stacks if r not in php]
    left = max(100 - sum(out.values()), 0)
    for r in rest:
        out[id(r)] = round(left / len(rest), 2)
    return [(r[0], r[1], r[2], out[id(r)]) for r in pool]


# --- main --------------------------------------------------------------------------

def yaml_str(s):
    if re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9 _\'().&+,\-]*', s) and not s.endswith(' ') \
            and s.lower() not in ('true', 'false', 'yes', 'no', 'null') and not re.fullmatch(r'[\d.]+', s):
        return s
    return '"%s"' % s.replace('\\', '\\\\').replace('"', '\\"')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rathena', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    ap.add_argument('--iro', action='append', default=[], help='iRO itemInfo .lub, most authoritative first')
    ap.add_argument('--kro', action='append', default=[], help='kRO itemInfo .lub, most authoritative first')
    ap.add_argument('--en', action='append', default=[],
                    help='an English item table covering several servers (ROenglishRE itemInfo.lua, text or bytecode); '
                         'its entries\' Server field names the region. Read after --iro and before --kro')
    ap.add_argument('--iro-moveinfo')
    ap.add_argument('--kro-moveinfo')
    ap.add_argument('--iro-skills', help='folder with iRO skillid.lub and skillinfolist.lub')
    ap.add_argument('--kro-skills', help='folder with kRO skillid.lub and skillinfolist.lub')
    ap.add_argument('--names', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kro_names_en.txt'),
                    help='English names for kRO-only items: "<id>\\t<name>" per line')
    ap.add_argument('--out', default='db/re/item_db_regional.yml')
    ap.add_argument('--out-combos', default='db/re/item_combos_regional.yml')
    ap.add_argument('--report', help='write a per-item translation report (markdown)')
    ap.add_argument('--list-kro-only', help='write the kRO-only Ids and Korean names to translate, then stop')
    a = ap.parse_args()
    root = os.path.abspath(a.rathena)

    existing, ra_by_name, ra_skills, ra_skill_desc = load_rathena(root)
    aegis_used = {it['AegisName'].lower() for it in existing.values() if it.get('AegisName')}

    region_tables = []
    for region, paths, enc in (('iRO', a.iro, 'latin-1'), ('EN', a.en, 'latin-1'), ('kRO', a.kro, 'cp949')):
        for p in paths:
            G = luadata.load_any(p, enc)
            region_tables.append((region, G.get('tbl') or {}))
    entries = {}         # id -> (region, entry)
    for region, tbl in region_tables:
        for k, v in tbl.items():
            if isinstance(k, int) and isinstance(v, dict) and k not in entries \
                    and clean(v.get('identifiedDisplayName') or '').strip():
                r = region
                if region == 'EN':
                    r = re.sub(r'[^A-Za-z]', '', str(v.get('Server') or '')) or 'EN'
                    r = 'EN' if r.lower() == 'unknown' else r
                entries[k] = (r, v)
    iro_ids = {k for region, tbl in region_tables if region == 'iRO' for k in tbl}

    moves = {}
    move_aegis = {}
    for region, path, enc in (('kRO', a.kro_moveinfo, 'cp949'), ('iRO', a.iro_moveinfo, 'latin-1')):
        r, ae = load_moveinfo(path, enc)
        moves.update(r)
        move_aegis.update(ae)

    # skill names -> AegisName
    skills = {}
    for desc, nm in ra_skill_desc.items():
        prev = skills.get(desc.lower())
        if prev and not prev.startswith(('NPC_', 'ALL_', 'GD_', 'MER_', 'MH_', 'EL_', 'HLIF_', 'HAMI_', 'HVAN_', 'HFLI_')) \
                and nm.startswith(('NPC_', 'ALL_', 'GD_', 'MER_', 'MH_', 'EL_', 'HLIF_', 'HAMI_', 'HVAN_', 'HFLI_')):
            continue
        skills[desc.lower()] = nm
    for d, enc in ((a.kro_skills, 'cp949'), (a.iro_skills, 'latin-1')):
        for sname, const in load_skills(d, enc).items():
            if const in ra_skills and not const.startswith('NPC_'):
                skills.setdefault(sname.lower(), const)
                skills.setdefault(re.sub(r'\s*\(.*?\)\s*', '', sname).strip().lower(), const)

    names_en = {}
    if a.names and os.path.exists(a.names):
        for line in open(a.names, encoding='utf-8'):
            m = re.match(r'(\d+)\t(.+)$', line.rstrip('\n'))
            if m:
                names_en[int(m.group(1))] = m.group(2).strip()

    candidates = sorted(i for i, (r, v) in entries.items()
                        if i not in existing and clean(v.get('identifiedDisplayName') or '').strip())

    if a.list_kro_only:
        with open(a.list_kro_only, 'w', encoding='utf-8') as out:
            for i in candidates:
                region, v = entries[i]
                if region == 'kRO':
                    out.write('%d\t%s\t%s\n' % (i, clean(v['identifiedDisplayName']).strip(),
                                                clean(v.get('unidentifiedDisplayName') or '').strip()))
        return

    # item names -> ids, for set members and box contents
    def display_name(i):
        region, v = entries[i]
        nm = clean(v['identifiedDisplayName']).strip()
        if re.search(r'[\uac00-\ud7a3]', nm) or region == 'kRO' and i in names_en:
            return names_en.get(i)
        return nm

    by_name = collections.defaultdict(list)
    wanted = set(existing) | set(candidates)
    for region, tbl in region_tables:
        for i, v in tbl.items():
            if i in wanted and isinstance(v, dict):
                by_name[name_key(clean(v.get('identifiedDisplayName') or ''))].append(i)
    for i, (region, v) in entries.items():
        if i in existing or i in candidates:
            if region == 'kRO' and i in names_en:
                by_name[name_key(names_en[i])].append(i)
    for k, ids in ra_by_name.items():
        by_name[k].extend(ids)

    def loose(k):
        k = re.sub(r'^costume\s*:\s*', 'costume ', k)
        k = re.sub(r'\s*\[\d\]', '', k)
        return re.sub(r'[^a-z0-9가-힣]+', '', k)

    known = set(existing) | set(candidates)
    by_loose = collections.defaultdict(list)
    for key, ids in by_name.items():
        by_loose[loose(key)].extend(ids)
    for x, it in existing.items():          # AegisNames too: Bloody_Dead_Branch
        if it.get('AegisName'):
            by_loose[loose(it['AegisName'].lower())].append(x)
    loose_keys = [k for k in by_loose if len(k) >= 8]

    def words(k):
        k = re.sub(r'\s*\[\d\]|\bcostume\s*:?|[^a-z0-9 ]+', ' ', k)
        return ' '.join(sorted(w for w in k.split() if w not in ('of', 'the', 's')))

    by_words = collections.defaultdict(list)
    for key, ids in by_name.items():
        by_words[words(key)].extend(ids)

    def pick(ids):
        ids = [x for x in dict.fromkeys(ids) if x in known]
        ids.sort(key=lambda x: (x not in existing, x))
        return ids[0] if ids else None

    def resolve(nm):
        k0 = name_key(nm)
        x = pick(by_name.get(k0, [])) or pick(by_loose.get(loose(k0), []))
        if x:
            return x
        k = re.sub(r'^(?:\d+\s*(?:x|ea|cnt|pcs)\s+)|(?:\s+x\s*\d+|\s+\d+\s*(?:cnt|ea|pcs))$', '', k0).strip()
        k2 = re.sub(r'\s*\(\s*\d+\s*(?:days?|hours?|일)\s*\)\s*', '', k)
        for key in (k, k2, re.sub(r'^\[event\]\s*', '', k), re.sub(r's$', '', k)):
            x = pick(by_name.get(key, [])) or pick(by_loose.get(loose(key), []))
            if x:
                return x
        wk = words(k)
        if len(wk.split()) >= 2:
            x = pick(by_words.get(wk, [])) or pick(by_words.get(words(re.sub(r'(?:e?s)\b', '', k)), []))
            if x:
                return x
        lk = loose(k)
        if len(lk) >= 10:      # a typo in the box text ("Miracle Elixr")
            digits = re.findall(r'\d+', lk)
            for c in difflib.get_close_matches(lk, loose_keys, n=3, cutoff=0.92):
                if re.findall(r'\d+', c) == digits:
                    return pick(by_loose[c])
        return None

    # weapon defaults from rAthena's own entries
    per = collections.defaultdict(lambda: collections.Counter())
    hands = collections.defaultdict(lambda: collections.Counter())
    lv = collections.defaultdict(lambda: collections.Counter())
    for it in existing.values():
        if it.get('Type') == 'Weapon' and it.get('SubType'):
            per[it['SubType']][it.get('Range', '1')] += 1
            hands[it['SubType']][it.get('Hand', 'Right_Hand')] += 1
            if it.get('WeaponLevel'):
                lv[it['SubType']][it['WeaponLevel']] += 1
    defaults = {s: {'Range': int(per[s].most_common(1)[0][0]), 'Hand': hands[s].most_common(1)[0][0]} for s in per}

    out, combos, report = [], [], []
    stats = collections.Counter()
    aegis_of = {i: existing[i]['AegisName'] for i in existing if existing[i].get('AegisName')}
    info = {}

    # pass 1: names, aegis, base fields
    for i in candidates:
        region, v = entries[i]
        name = display_name(i)
        if not name:
            stats['skipped: no English name'] += 1
            continue
        lines = desc_lines(v)
        f, rest = split_fields(lines)
        kind = infer(i, v, f, name, defaults)
        ae = move_aegis.get(i)
        if not ae:
            res = clean(v.get('identifiedResourceName') or '')
            ae = res if re.fullmatch(r'[A-Za-z][A-Za-z0-9_]{2,}', res) else '%s_%d' % (region, i)
        if ae.lower() in aegis_used:
            ae = '%s_%d' % (ae, i)
        aegis_used.add(ae.lower())
        aegis_of[i] = ae
        info[i] = dict(region=region, v=v, name=name, f=f, rest=rest, kind=kind)

    item_slot = {}
    for it in existing.values():
        if it.get('_loc') and it.get('Name'):
            item_slot.setdefault(name_key(it['Name']), SLOT_EQI.get(it['_loc']))
    for i, d in info.items():
        if d['kind'] and d['kind'].get('Locations'):
            item_slot[name_key(d['name'])] = SLOT_EQI.get(d['kind']['Locations'][0])

    # pass 2: entries
    for i, d in info.items():
        region, v, name, f, rest, kind = d['region'], d['v'], d['name'], d['f'], d['rest'], d['kind']
        unknown = []
        e = collections.OrderedDict()
        e['Id'] = i
        e['AegisName'] = aegis_of[i]
        e['Name'] = name if len(name) <= 49 else name.replace('(Account Bound) ', '').replace('Package', 'Pkg')[:49].rstrip()
        if kind is None:
            stats['etc: equipment slot not readable'] += 1
            unknown.append('equipment type/slot not readable: %s' % ', '.join('%s=%s' % kv for kv in f.items() if not kv[0].startswith('_')))
            kind = {'Type': 'Etc', 'Locations': None}
        typ = kind['Type']
        e['Type'] = typ
        if kind.get('SubType'):
            e['SubType'] = kind['SubType']
        w = to_int(first(f, 'Weight', '무게', 'Box Weight'))
        if w:
            e['Weight'] = w * 10
        locs = kind.get('Locations') or []
        equip = typ in ('Armor', 'Weapon', 'ShadowGear')
        if typ == 'Weapon':
            atk = to_int(first(f, 'Attack', 'Attack Strength', 'ATK', '공격'))
            if atk is None:
                pre = f.get('Type') or f.get('계열') or ''
                m = re.search(r'(?:atk|attack|공격)\s*:?\s*(\d+)', pre, re.I)
                atk = int(m.group(1)) if m else None
            if atk:
                e['Attack'] = atk
            matk = to_int(first(f, 'MATK', 'Magic Attack', '마법공격', '마법 공격'))
            if matk:
                e['MagicAttack'] = matk
            e['Range'] = defaults.get(kind['SubType'], {}).get('Range', 1)
        if typ in ('Armor', 'ShadowGear'):
            dv = to_int(first(f, 'Defense', 'Def', 'DEF', '방어'))
            if dv is None:
                pre = first(f, 'Type', 'Class', '계열') or ''
                m = re.search(r'(?:def(?:ense)?|방어)\s*:?\s*(\d+)', pre, re.I)
                dv = int(m.group(1)) if m else None
            if dv:
                e['Defense'] = dv
        slots = v.get('slotCount') or 0
        if slots and (equip or typ == 'Card'):
            e['Slots'] = slots
        jt = first(f, 'Jobs', 'Job', 'Usable By', 'Usable by', 'Equipped by', 'Equipable Jobs', 'Applicable Job')
        if not jt and typ != 'Card':
            jt = f.get('장착')
        if not jt and (f.get('Class') and f.get('Type')):
            jt = f.get('Class')    # "Type: Headgear ... Class: All Jobs"
        if equip and jt:
            pj = parse_jobs(jt)
            if pj is None:
                unknown.append('jobs not readable: %s' % jt)
            else:
                jobs, classes = pj
                if jobs:
                    e['Jobs'] = jobs
                if classes:
                    e['Classes'] = classes
        if typ == 'Weapon' and kind.get('SubType') in ('Whip', 'Musical'):
            e['Gender'] = 'Female' if kind['SubType'] == 'Whip' else 'Male'
        if locs:
            e['Locations'] = {l: True for l in locs}
        if typ == 'Weapon':
            wl = to_int(first(f, 'Weapon Level', '무기레벨', '무기 레벨'))
            e['WeaponLevel'] = min(wl, 5) if wl else 4
        # renewal's item_db warns about any armor without one, costumes included
        if typ == 'Armor':
            al = to_int(first(f, 'Armor Level', '방어구 레벨', '방어구레벨'))
            e['ArmorLevel'] = min(al, 2) if al else 1
        if equip:
            req = to_int(first(f, 'Required Level', 'Requires Level', 'Require Level', 'Required Lv', 'Required LVL', 'Req LvL', 'Req Lv', 'Required Base Level',
                               'Req. Level', 'Base Level', 'Level Requirement', '요구 레벨', '요구레벨'))
            if req and req > 1:
                e['EquipLevelMin'] = req
        text = ' '.join(l for l in rest if l)
        if equip:
            costume = any(l.startswith('Costume') for l in locs)
            no_refine = re.search(r'cannot be (?:refined|upgraded)|can ?not be refined|unrefinable|refine(?:ment)? (?:is )?(?:not )?(?:impossible|unavailable)|제련 ?불가', text, re.I)
            refine_text = re.search(r'refine|제련', text, re.I)
            accessory = any('Accessory' in l for l in locs)
            if not costume and not no_refine and (not accessory or refine_text):
                e['Refineable'] = True
            if re.search(r'\[grade|grade [dcba]\b|grade bonus|등급', text, re.I):
                e['Gradable'] = True
        if equip and v.get('ClassNum') and typ != 'ShadowGear':
            e['View'] = v['ClassNum']
        elif typ == 'Armor' and 'Left_Hand' in locs:
            # a shield needs a view id for skills that check for one; 1 is Guard,
            # what the item_db would default it to with a warning
            e['View'] = 1
        rule = moves.get(i)
        if rule:
            flags = ['NoDrop', 'NoTrade', 'NoStorage', 'NoCart', 'NoSell', 'NoMail', 'NoAuction', 'NoGuildStorage']
            tr = {flags[k]: True for k in range(8) if rule[k] == 0}
            if tr:
                e['Trade'] = tr

        # script
        script, combo_list = [], []
        is_box = False
        if typ in ('Usable', 'Etc') and not equip:
            sections = parse_box(rest)
            if sections:
                is_box = True
                part_scripts, all_ok = [], True
                for rnd, got in sections:
                    part = box_section(i, name, rnd, got, resolve, aegis_of, unknown)
                    if part is None:
                        all_ok = False
                        break
                    part_scripts.append(part)
                if all_ok:
                    for part in part_scripts:
                        script.extend(part)
        if typ in ('Armor', 'Weapon', 'ShadowGear', 'Card'):
            ele = first(f, 'Property', 'Element', '속성')
            if ele and typ == 'Weapon':
                ek = ele.strip().lower().replace('속성', '').replace(' property', '')
                if ek in ELE_FIELD and ek != 'neutral':
                    script.append('bonus bAtkEle,%s;' % ELE_FIELD[ek])
            self_eqi = SLOT_EQI.get(locs[0]) if locs else None
            ctx = B.Ctx(skills, locs, False, self_eqi, item_slot)
            pending = ([], [])
            mode, members, alt_next = 'main', [], False
            set_effects = []
            members_ok = True

            def close_set():
                if members or set_effects:
                    combo_list.append((list(members), list(set_effects), members_ok))

            for l in rest:
                if l is None:
                    pending = ([], [])
                    if mode == 'members' and members:
                        mode = 'set'
                    continue
                low = l.lower()
                if re.fullmatch(r'\[?(?:set bonus|set effect|combo bonus|set)\]?\s*:?|세트 효과\s*:?|\[세트 효과\]', low):
                    if mode in ('members', 'set'):
                        close_set()
                    mode, members, set_effects, members_ok = 'members', [[i]], [], True
                    pending = ([], [])
                    continue
                if re.fullmatch(r'\[(?:grade bonus|bonus by grade|grade bonus effects?|.*grade bonus|등급별 추가 옵션|무기 등급별 추가 옵션|추가 효과)\]', low):
                    continue
                if re.fullmatch(r'-{2,}|_+', l):
                    if mode in ('members', 'set'):
                        close_set()
                        mode = 'main'
                    continue
                m = re.match(r'(?:when |if )?(?:equipped|worn|equip(?:ped)?) (?:together )?with (.+?)\s*[,:]\s*(.*)$', l, re.I) \
                    or re.match(r'(.+?)(?:와|과) 함께 (?:장착|착용) 시\s*,?\s*(.*)$', l)
                if m:
                    names = [n for n in re.split(r'\s*(?:,|\band\b|및)\s*', m.group(1)) if n.strip()]
                    ids = [resolve(n) for n in names]
                    if mode in ('members', 'set'):
                        close_set()
                    if all(ids):
                        mode, members, set_effects, members_ok = 'set', [[i]] + [[x] for x in ids], [], True
                    else:
                        mode, members, set_effects, members_ok = 'set', [[i]], [], False
                        unknown.append('set members not found: %s' % m.group(1))
                    pending = ([], [])
                    l = m.group(2).strip()
                    if not l:
                        continue
                    low = l.lower()
                if mode == 'members':
                    if low in ('or', '또는'):
                        alt_next = True
                        continue
                    x = resolve(l)
                    if x is not None:
                        if x != i:
                            if alt_next and len(members) > 1:
                                members[-1].append(x)
                            else:
                                members.append([x])
                        alt_next = False
                        continue
                    if re.fullmatch(r'.*\[\d\]\s*', l) or (not B.looks_like_effect(l) and len(l) < 60):
                        members_ok = False
                        unknown.append('set member not found: %s' % l)
                        continue
                    mode = 'set'
                if re.fullmatch(r'\[[^\]]*\]|\(.*\)|[^+%:]*!', l) and not re.match(r'\[grade', low):
                    continue
                c = ctx if mode == 'main' else B.Ctx(skills, locs, True, self_eqi, item_slot)
                stmts, newp, st = B.translate_line(l, c, pending)
                if i in DEBUG:
                    print('%d %-5s %-4s %r -> %s' % (i, mode, st, l, stmts), file=sys.stderr)
                if st == 'cond':
                    pending = newp
                elif st == 'ok':
                    (script if mode == 'main' else set_effects).extend(stmts)
                elif st == 'unknown' and B.looks_like_effect(l):
                    unknown.append(l if mode == 'main' else '[set] ' + l)
            if mode in ('members', 'set'):
                close_set()

            for mem, eff, ok in combo_list:
                if not ok or len(mem) < 2 or not eff:
                    continue
                for combo in itertools.product(*mem):
                    if len(set(combo)) == len(combo):
                        combos.append((sorted(combo), eff, i))

        if is_box or typ == 'Usable':
            e['Type'] = 'Usable'
            if not script:
                # consumable tab, but not used up until its effect is known
                e['Flags'] = {'NoConsume': True}
                script.append('dispbottom "%s";' % (
                    "This box's contents aren't available on this server yet." if is_box or re.search(r'box|egg|package|crate|chest|pack\b', name, re.I)
                    else "This item's effect isn't available on this server yet."))
                stats['usable with no readable effect (NoConsume)'] += 1
        if script:
            e['Script'] = script
        e['_unknown'] = unknown
        out.append(e)
        stats['entries'] += 1
        stats['region ' + region] += 1
        stats['type ' + e['Type']] += 1
        if equip or typ == 'Card':
            stats['equip/card'] += 1
            if unknown:
                stats['equip/card with untranslated lines'] += 1
        stats['untranslated lines'] += len(unknown)
        report.append((i, name, region, e['Type'], unknown))

    write_items(os.path.join(root, a.out), out)
    write_combos(os.path.join(root, a.out_combos), combos, aegis_of)
    if a.report:
        with open(a.report, 'w', encoding='utf-8') as r:
            r.write('# Regional items: translation report\n\n')
            for k, n in sorted(stats.items()):
                r.write('- %s: %d\n' % (k, n))
            r.write('\n')
            for i, name, region, typ, unk in report:
                if unk:
                    r.write('## %d %s (%s, %s)\n' % (i, name, region, typ))
                    for u in unk:
                        r.write('- %s\n' % u)
                    r.write('\n')
    for k, n in sorted(stats.items()):
        print('%-50s %d' % (k, n))
    print('combos %d' % len(combos))


HEADER = """# This file is a part of rAthena.
#   Copyright(C) 2026 rAthena Development Team
#   https://rathena.org - https://github.com/rathena
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
#
###########################################################################
# %s
###########################################################################
#
# GENERATED by tools/regional_items/generate.py from official client item
# tables and ROenglishRE. Do not edit by hand: fix the generator (or its
# name list) and run it again.
#
# Items the clients know but the main item_db doesn't. Lines of a
# description the generator couldn't turn into script are kept as
# "# <Id> untranslated:" comments above the item.
#
###########################################################################
"""


def write_items(path, items):
    with open(path, 'w', encoding='utf-8', newline='\n') as o:
        o.write(HEADER % 'Regional Item Database')
        o.write('\nHeader:\n  Type: ITEM_DB\n  Version: 3\n\nBody:\n')
        for e in items:
            for u in e['_unknown']:
                o.write('  # %d untranslated: %s\n' % (e['Id'], u.replace('\n', ' ')))
            o.write('  - Id: %d\n' % e['Id'])
            for k, val in e.items():
                if k in ('Id', '_unknown'):
                    continue
                if isinstance(val, dict):
                    o.write('    %s:\n' % k)
                    for kk, vv in val.items():
                        o.write('      %s: %s\n' % (kk, 'true' if vv is True else 'false' if vv is False else vv))
                elif k == 'Script':
                    o.write('    Script: |\n')
                    for s in val:
                        for sl in s.split('\n'):
                            o.write('      %s\n' % sl)
                elif isinstance(val, bool):
                    o.write('    %s: %s\n' % (k, 'true' if val else 'false'))
                elif isinstance(val, int):
                    o.write('    %s: %d\n' % (k, val))
                else:
                    o.write('    %s: %s\n' % (k, yaml_str(val)))


def write_combos(path, combos, aegis_of):
    seen = set()
    with open(path, 'w', encoding='utf-8', newline='\n') as o:
        o.write(HEADER % 'Regional Item Combo Database')
        o.write('\nHeader:\n  Type: COMBO_DB\n  Version: 1\n\nBody:\n')
        for ids, eff, owner in combos:
            key = (tuple(ids), tuple(eff))
            if key in seen:
                continue
            seen.add(key)
            o.write('  - Combos:\n      - Combo:\n')
            for x in ids:
                o.write('          - %s    # %d\n' % (aegis_of[x], x))
            o.write('    Script: |\n')
            for s in eff:
                for sl in s.split('\n'):
                    o.write('      %s\n' % sl)


if __name__ == '__main__':
    main()
