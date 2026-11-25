"""Wayfarer Data Builder: quest data for Wayfarer's Journal, made from the game's own files.

Reads an OpenMW profile (openmw.cfg: the data folders and the content list), then the content files themselves
(.esm/.esp/.omwaddon), and works out for each journal quest and stage what moves it on: who to talk to about what,
what to kill, what to get, where to go. Writes the questData folder in the same format as Quest Guider Lite's
Quest Data Builder (quests.yaml, questObjects.yaml, info.yaml), the parts Wayfarer's Journal reads; the journal
converts it in game.

Usage: wayfarer_data_builder [--config openmw.cfg] [--out folder]
"""

import argparse
import json
import os
import re
import struct
import sys
import time

VERSION = '0.2'

ITEM_TYPES = {b'ALCH', b'APPA', b'ARMO', b'BOOK', b'CLOT', b'INGR', b'LIGH', b'LOCK', b'MISC', b'PROB', b'REPA',
              b'WEAP'}
OBJECT_TYPES = ITEM_TYPES | {b'NPC_', b'CREA', b'CONT', b'ACTI', b'DOOR'}
HOLDER_TYPES = {b'NPC_', b'CREA', b'CONT'}
FIRST_PASS = OBJECT_TYPES | {b'DIAL', b'INFO', b'SCPT', b'TES3'}

OPS = {'==': 48, '=': 48, '!=': 49, '>': 50, '>=': 51, '<': 52, '<=': 53}


def log(*a):
    print(*a, flush=True)


# --------------------------------------------------------------------------------------------------------------------
# The profile

def documents_folder():
    """Windows' Documents folder, wherever it has been moved (OneDrive and so on)."""
    if os.name == 'nt':
        try:
            import ctypes
            buf = ctypes.create_unicode_buffer(260)
            if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buf) == 0:
                return buf.value
        except Exception:
            pass
    return os.path.join(os.path.expanduser('~'), 'Documents')


def default_config():
    home = os.path.expanduser('~')
    for p in (os.path.join(documents_folder(), 'My Games', 'OpenMW', 'openmw.cfg'),
              os.path.join(home, 'Documents', 'My Games', 'OpenMW', 'openmw.cfg'),
              os.path.join(home, '.config', 'openmw', 'openmw.cfg')):
        if os.path.isfile(p):
            return p
    return None


def read_config(path, remaps):
    data, content = [], []

    def unquote(v):
        v = v.strip()
        if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
            v = v[1:-1].replace('&&', '&').replace('&"', '"')
        return v

    def remap(p):
        p = p.replace('\\', '/')
        for src, dst in remaps:
            if p.lower().startswith(src.lower()):
                return dst + p[len(src):]
        return p

    with open(path, encoding='utf-8-sig', errors='replace') as f:
        for line in f:
            line = line.strip()
            if line.startswith('data='):
                data.append(remap(unquote(line[5:])))
            elif line.startswith('content='):
                content.append(unquote(line[8:]))
    return data, content


def find_files(data_dirs, names):
    """Content file name -> path, the last data folder that has it winning (as in OpenMW)."""
    wanted = {n.lower() for n in names}
    found = {}
    for d in data_dirs:
        try:
            entries = os.listdir(d)
        except OSError:
            continue
        for e in entries:
            if e.lower() in wanted:
                found[e.lower()] = os.path.join(d, e)
    return found


# --------------------------------------------------------------------------------------------------------------------
# Reading the files

def zstr(b):
    i = b.find(b'\0')
    if i >= 0:
        b = b[:i]
    return b.decode('cp1252', errors='replace')


def subrecords(data):
    i, n = 0, len(data)
    while i + 8 <= n:
        name = data[i:i + 4]
        size = struct.unpack_from('<I', data, i + 4)[0]
        yield name, data[i + 8:i + 8 + size]
        i += 8 + size


def records(path, wanted):
    with open(path, 'rb') as f:
        while True:
            head = f.read(16)
            if len(head) < 16:
                return
            name = head[:4]
            size, _, flags = struct.unpack('<III', head[4:])
            if name in wanted:
                yield name, flags, f.read(size)
            else:
                f.seek(size, 1)


class World:
    def __init__(self):
        self.files = []           # lowercase names, in load order
        self.dials = {}           # dial id (lower) -> {type, name, infos: {info id: info}}
        self.scripts = {}         # script id (lower) -> text
        self.objects = {}         # object id (lower) -> {type, name, script}
        self.inventory = {}       # holder id (lower) -> {item id (lower): count}
        self.refs = {}            # (file index, ref index) -> (object id, cell, pos) or None (deleted)

    # first pass: everything but the cells
    def read_records(self, index, path):
        masters = []
        current_dial = None
        for rtype, flags, data in records(path, FIRST_PASS):
            deleted = bool(flags & 0x20)
            if rtype == b'TES3':
                for sname, sdata in subrecords(data):
                    if sname == b'MAST':
                        masters.append(zstr(sdata).lower())
                continue
            sub = list(subrecords(data))
            if rtype == b'DIAL':
                did, dtype = None, None
                for sname, sdata in sub:
                    if sname == b'NAME':
                        did = zstr(sdata)
                    elif sname == b'DATA' and sdata:
                        dtype = sdata[0]
                    elif sname == b'DELE':
                        deleted = True
                if did is None:
                    current_dial = None
                    continue
                key = did.lower()
                d = self.dials.get(key)
                if d is None:
                    d = {'type': dtype, 'name': did, 'infos': {}}
                    self.dials[key] = d
                elif dtype is not None:
                    d['type'] = dtype
                current_dial = d
                continue
            if rtype == b'INFO':
                if current_dial is None:
                    continue
                info = {'conds': []}
                cond = None
                for sname, sdata in sub:
                    if sname == b'INAM':
                        info['id'] = zstr(sdata)
                    elif sname == b'DATA' and len(sdata) >= 12:
                        info['index'] = struct.unpack_from('<i', sdata, 4)[0]
                    elif sname == b'ONAM':
                        info['actor'] = zstr(sdata).lower()
                    elif sname == b'FNAM':
                        info['faction'] = zstr(sdata).lower()
                    elif sname == b'ANAM':
                        info['cell'] = zstr(sdata)
                    elif sname == b'NAME':
                        info['text'] = zstr(sdata)
                    elif sname == b'BNAM':
                        info['result'] = zstr(sdata)
                    elif sname == b'SCVR':
                        s = zstr(sdata)
                        cond = {'kind': s[1:2], 'func': s[2:4], 'op': s[4:5], 'name': s[5:]}
                        info['conds'].append(cond)
                    elif sname == b'INTV' and cond is not None and len(sdata) >= 4:
                        cond['value'] = struct.unpack_from('<i', sdata)[0]
                    elif sname == b'FLTV' and cond is not None and len(sdata) >= 4:
                        cond['value'] = struct.unpack_from('<f', sdata)[0]
                    elif sname == b'QSTN':
                        info['named'] = True
                    elif sname == b'QSTF':
                        info['finished'] = True
                    elif sname == b'DELE':
                        deleted = True
                iid = info.get('id')
                if not iid:
                    continue
                if deleted:
                    current_dial['infos'].pop(iid, None)
                else:
                    current_dial['infos'][iid] = info
                continue
            if rtype == b'SCPT':
                sid, text = None, ''
                for sname, sdata in sub:
                    if sname == b'SCHD':
                        sid = zstr(sdata[:32])
                    elif sname == b'SCTX':
                        text = sdata.decode('cp1252', errors='replace')
                if sid:
                    if deleted:
                        self.scripts.pop(sid.lower(), None)
                    else:
                        self.scripts[sid.lower()] = text
                continue
            # objects
            oid, name, script, inv = None, '', None, None
            for sname, sdata in sub:
                if sname == b'NAME':
                    oid = zstr(sdata)
                elif sname == b'FNAM':
                    name = zstr(sdata)
                elif sname == b'SCRI':
                    script = zstr(sdata).lower()
                elif sname == b'NPCO' and len(sdata) >= 36:
                    if inv is None:
                        inv = {}
                    count = struct.unpack_from('<i', sdata)[0]
                    item = zstr(sdata[4:36]).lower()
                    inv[item] = inv.get(item, 0) + abs(count)
            if not oid:
                continue
            key = oid.lower()
            if deleted:
                self.objects.pop(key, None)
                self.inventory.pop(key, None)
                continue
            self.objects[key] = {'type': rtype.decode(), 'name': name, 'script': script}
            if rtype in HOLDER_TYPES:
                if inv:
                    self.inventory[key] = inv
                else:
                    self.inventory.pop(key, None)
        return masters

    # second pass: the placed objects in the cells, only for the ids wanted
    def read_cells(self, index, path, masters, wanted):
        mast_index = [self.files.index(m) if m in self.files else -1 for m in masters]
        for rtype, flags, data in records(path, {b'CELL'}):
            cell_name, interior, grid = '', False, None
            ref_key, ref_obj, ref_pos, ref_deleted = None, None, None, False
            in_refs = False

            def flush():
                if ref_key is None:
                    return
                if ref_deleted:
                    if ref_key in self.refs:
                        self.refs[ref_key] = None
                    return
                if ref_obj is not None and ref_obj in wanted:
                    cell = cell_name if interior else grid
                    self.refs[ref_key] = (ref_obj, cell, ref_pos)
                elif ref_key in self.refs:
                    # overridden by another object, or one we don't need
                    self.refs[ref_key] = None

            for sname, sdata in subrecords(data):
                if not in_refs:
                    if sname == b'NAME':
                        cell_name = zstr(sdata)
                    elif sname == b'DATA' and len(sdata) >= 12:
                        cflags, gx, gy = struct.unpack_from('<Iii', sdata)
                        interior = bool(cflags & 1)
                        grid = (gx, gy)
                    elif sname == b'FRMR':
                        in_refs = True
                if in_refs:
                    if sname == b'FRMR':
                        flush()
                        raw = struct.unpack_from('<I', sdata)[0]
                        high, low = raw >> 24, raw & 0xFFFFFF
                        if high == 0:
                            f = index
                        else:
                            f = mast_index[high - 1] if high - 1 < len(mast_index) else -1
                        ref_key, ref_obj, ref_pos, ref_deleted = (f, low), None, None, False
                    elif sname == b'NAME':
                        ref_obj = zstr(sdata).lower()
                    elif sname == b'DATA' and len(sdata) >= 12:
                        ref_pos = struct.unpack_from('<fff', sdata)
                    elif sname == b'DELE':
                        ref_deleted = True
            flush()


# --------------------------------------------------------------------------------------------------------------------
# Working out the quests

def strip_comment(line):
    out, q = [], False
    for ch in line:
        if ch == '"':
            q = not q
        if ch == ';' and not q:
            break
        out.append(ch)
    return ''.join(out)


JOURNAL_RE = re.compile(r'(?:^|[\s>])(?:journal|setjournalindex)\s*,?\s*(?:"([^"]+)"|([^\s,"]+))\s*,?\s*(-?\d+)',
                        re.I)
ID = r'(?:"([^"]+)"|([\w\'\.\-]+))'
CMP = r'\s*(==|!=|>=|<=|>|<|=)\s*(-?[\d\.]+)'
COND_RES = [
    ('item', re.compile(r'(?:' + ID + r'\s*->\s*)?getitemcount\s*,?\s*' + ID + CMP, re.I)),
    ('dead', re.compile(r'getdeadcount\s*,?\s*' + ID + CMP, re.I)),
    ('cell', re.compile(r'getpccell\s*,?\s*"([^"]+)"(?:' + CMP + r')?', re.I)),
    ('journal', re.compile(r'getjournalindex\s*,?\s*' + ID + CMP, re.I)),
    ('dist', re.compile(r'(?:' + ID + r'\s*->\s*)?getdistance\s*,?\s*' + ID + CMP, re.I)),
    ('activate', re.compile(r'\bonactivate\b', re.I)),
    ('death', re.compile(r'\bondeath\b', re.I)),
]


def pick(a, b):
    return (a or b or '').strip()


def parse_conditions(text, owner):
    """Requirements from an if-condition of a script: [{type, variable, operator, value, object}]."""
    reqs = []
    for kind, rx in COND_RES:
        for m in rx.finditer(text):
            g = m.groups()
            if kind == 'item':
                subj = pick(g[0], g[1]).lower()
                item = pick(g[2], g[3]).lower()
                if subj in ('', 'player') and (subj == 'player' or owner is None):
                    reqs.append({'type': 'IX', 'variable': item, 'operator': OPS[g[4]], 'value': float(g[5]),
                                 'object': 'player'})
            elif kind == 'dead':
                reqs.append({'type': 'DX', 'variable': pick(g[0], g[1]).lower(), 'operator': OPS[g[2]],
                             'value': float(g[3])})
            elif kind == 'cell':
                op = OPS.get(g[1] or '==', 48)
                val = float(g[2]) if g[2] else 1.0
                if (op == 48 and val == 1) or (op == 49 and val == 0) or (op == 50 and val == 0):
                    reqs.append({'type': 'PCL', 'variable': g[0], 'operator': 48, 'value': 1.0})
            elif kind == 'journal':
                reqs.append({'type': 'JX', 'variable': pick(g[0], g[1]).lower(), 'operator': OPS[g[2]],
                             'value': float(g[3]), 'object': 'player'})
            elif kind == 'dist':
                subj = pick(g[0], g[1]).lower()
                arg = pick(g[2], g[3]).lower()
                op = OPS[g[4]]
                if op not in (52, 53):
                    continue
                target = None
                if subj in ('', ) and arg == 'player':
                    target = owner
                elif subj == 'player':
                    target = arg
                elif arg == 'player':
                    target = subj
                if target:
                    reqs.append({'type': 'DIS', 'variable': target, 'operator': op, 'value': float(g[5])})
            elif kind == 'activate' and owner:
                reqs.append({'type': 'DIS', 'variable': owner, 'operator': 52, 'value': 200.0, 'activate': True})
            elif kind == 'death' and owner:
                reqs.append({'type': 'DX', 'variable': owner, 'operator': 51, 'value': 1.0})
    return reqs


def script_sources(text, owner):
    """(quest, stage, requirements) for each Journal / SetJournalIndex in a script, with the conditions of the ifs
    around it."""
    out = []
    stack = []
    for raw in text.splitlines():
        line = strip_comment(raw).strip()
        if not line:
            continue
        low = line.lower()
        if low.startswith('if') and (len(low) == 2 or not low[2].isalnum()):
            stack.append(line[2:])
            continue
        if low.startswith('elseif') and (len(low) == 6 or not low[6].isalnum()):
            if stack:
                stack[-1] = line[6:]
            continue
        if low == 'else' or low.startswith('else ') or low.startswith('else;'):
            if stack:
                stack[-1] = ''
            continue
        if low.startswith('endif'):
            if stack:
                stack.pop()
            continue
        for m in JOURNAL_RE.finditer(line):
            quest = pick(m.group(1), m.group(2)).lower()
            stage = int(m.group(3))
            reqs = []
            for c in stack:
                reqs.extend(parse_conditions(c, owner))
            out.append((quest, stage, reqs))
    return out


def info_requirements(world, dial, info):
    """A dialogue response's requirements: who says it, about what, and its conditions."""
    reqs = []
    topic = '#dia: ' + dial['name']  # the topic as the game writes it ("Galasa Uvayn")
    reqs.append({'type': 'DIA', 'operator': 48, 'variable': topic})
    for c in info['conds']:
        kind, name, op = c['kind'], c['name'].lower(), c.get('op') or '0'
        opn = ord(op) if '0' <= op <= '5' else 48
        val = float(c.get('value', 0))
        if kind == '4':
            reqs.append({'type': 'JX', 'operator': opn, 'value': val, 'variable': name, 'object': 'player'})
        elif kind == '5':
            reqs.append({'type': 'IX', 'operator': opn, 'value': val, 'variable': name, 'object': 'player'})
        elif kind == '6':
            reqs.append({'type': 'DX', 'operator': opn, 'value': val, 'variable': name})
    if info.get('actor'):
        reqs.append({'type': 'ACT', 'operator': 48, 'variable': topic, 'object': info['actor']})
    return reqs


def result_stages(result):
    for m in JOURNAL_RE.finditer(result or ''):
        yield pick(m.group(1), m.group(2)).lower(), int(m.group(3))


def holds(cond_reqs, quest, stage):
    """Do the JX conditions on `quest` hold when it is at `stage`? None when there are none."""
    seen = False
    for r in cond_reqs:
        if r.get('type') != 'JX' or r.get('variable') != quest:
            continue
        seen = True
        v, op = r['value'], r['operator']
        ok = {48: stage == v, 49: stage != v, 50: stage > v, 51: stage >= v, 52: stage < v, 53: stage <= v}.get(op, True)
        if not ok:
            return False
    return True if seen else None


def build(world):
    quests = {}
    for did, d in world.dials.items():
        if d['type'] != 4:
            continue
        stages = {}
        name = None
        for info in d['infos'].values():
            idx = info.get('index')
            if idx is None:
                continue
            st = stages.setdefault(idx, {'requirements': []})
            if info.get('finished'):
                st['finished'] = True
            if info.get('named') and info.get('text'):
                name = info['text']
        if stages:
            q = {'stages': stages}
            if name:
                q['name'] = name
            quests[did] = q

    def add_source(quest, stage, reqs):
        q = quests.get(quest)
        if not q:
            return
        st = q['stages'].get(stage)
        if st is None:
            return
        if len(st['requirements']) < 12:
            st['requirements'].append(reqs)

    # dialogue
    for did, d in world.dials.items():
        if d['type'] == 4:
            continue
        for info in d['infos'].values():
            if not info.get('result'):
                continue
            for quest, stage in result_stages(info['result']):
                add_source(quest, stage, info_requirements(world, d, info))

    # scripts, with the objects they're on
    owners = {}
    for oid, o in world.objects.items():
        if o['script']:
            owners.setdefault(o['script'], []).append(oid)
    for sid, text in world.scripts.items():
        if 'journal' not in text.lower():
            continue
        objs = owners.get(sid) or [None]
        for owner in objs[:2]:
            for quest, stage, reqs in script_sources(text, owner):
                if owner and not any(r['type'] in ('DIS', 'DX', 'IX', 'PCL') for r in reqs):
                    # something about the object itself (picking it up, reading it, being near it)
                    reqs = reqs + [{'type': 'DIS', 'variable': owner, 'operator': 52, 'value': 300.0}]
                reqs = [{'type': 'SCR', 'operator': 48, 'variable': sid, 'script': sid}] + reqs
                add_source(quest, stage, reqs)

    # the stages that can follow each stage
    for qid, q in quests.items():
        idxs = sorted(q['stages'])
        for i, s in enumerate(idxs):
            st = q['stages'][s]
            nxt = []
            for t in idxs:
                if t <= s:
                    continue
                for block in q['stages'][t]['requirements']:
                    if holds(block, qid, s):
                        nxt.append(t)
                        break
            if nxt:
                st['next'] = nxt
            higher = [t for t in idxs if t > s]
            if higher and not st.get('finished'):
                st['nextIndex'] = min(nxt) if nxt else higher[0]
    return quests


def needed_ids(world, quests):
    ids = set()
    for q in quests.values():
        for st in q['stages'].values():
            for block in st['requirements']:
                for r in block:
                    t = r.get('type')
                    if t == 'ACT' and r.get('object'):
                        ids.add(r['object'])
                    elif t in ('DX', 'IX', 'DIS') and r.get('variable'):
                        ids.add(r['variable'])
    return ids


def holders_of(world, items):
    links = {}
    for holder, inv in world.inventory.items():
        for item in inv:
            if item in items:
                links.setdefault(item, []).append(holder)
    return links


def write_output(world, quests, out_dir, files):
    os.makedirs(out_dir, exist_ok=True)
    qout = {}
    for qid, q in quests.items():
        e = {}
        if q.get('name'):
            e['name'] = q['name']
        for idx, st in sorted(q['stages'].items()):
            s = {}
            if st['requirements']:
                s['requirements'] = st['requirements']
            if st.get('next'):
                s['next'] = st['next']
            if st.get('nextIndex') is not None:
                s['nextIndex'] = st['nextIndex']
            if st.get('finished'):
                s['finished'] = True
            e[str(idx)] = s
        qout[qid] = e

    ids = needed_ids(world, quests)
    items = {i for i in ids if i in world.objects and world.objects[i]['type'] in
             {t.decode() for t in ITEM_TYPES}}
    links = holders_of(world, items)
    placed = {}
    for ref in world.refs.values():
        if ref is None:
            continue
        oid, cell, pos = ref
        lst = placed.setdefault(oid, [])
        if len(lst) < 8:
            p = {'pos': [round(pos[0], 2), round(pos[1], 2), round(pos[2], 2)]}
            if isinstance(cell, str):
                p['name'] = cell
            elif cell is not None:
                p['grid'] = [cell[0], cell[1]]
            lst.append(p)
    oout = {}
    for oid in ids | {h for hs in links.values() for h in hs}:
        e = {}
        if oid in placed:
            e['positions'] = placed[oid]
        if oid in links:
            e['links'] = [[h, 1.0] for h in links[oid][:6]]
        if e:
            oout[oid] = e

    def dump(name, obj):
        with open(os.path.join(out_dir, name), 'w', encoding='utf-8') as f:
            json.dump(obj, f, ensure_ascii=False, separators=(',', ':'))

    dump('quests.yaml', qout)
    dump('questObjects.yaml', oout)
    dump('info.yaml', {'version': 8, 'time': int(time.time()), 'format': 'yaml', 'generator':
                       'Wayfarer Data Builder ' + VERSION, 'files': files})
    return len(qout), len(oout)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Quest data for Wayfarer\'s Journal.')
    ap.add_argument('--config', help='openmw.cfg of the profile (default: the one in Documents/My Games/OpenMW)')
    here = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.getcwd()
    ap.add_argument('--out', default=os.path.join(here, 'questData'),
                    help='the folder to write (default: questData next to the program)')
    ap.add_argument('--remap', action='append', default=[], help='FROM=TO: read data folders under FROM at TO')
    ap.add_argument('--no-pause', action='store_true', help='don\'t wait for Enter at the end')
    args = ap.parse_args(argv)

    cfg = args.config or default_config()
    if not cfg or not os.path.isfile(cfg):
        log('Could not find openmw.cfg; give it with --config.')
        return 1
    remaps = [tuple(r.split('=', 1)) for r in args.remap if '=' in r]
    log(f'Wayfarer Data Builder {VERSION}')
    log(f'Profile: {cfg}')
    data_dirs, content = read_config(cfg, remaps)
    plugins = [c for c in content if c.lower().endswith(('.esm', '.esp', '.omwaddon', '.omwgame'))]
    paths = find_files(data_dirs, plugins)
    world = World()
    order, missing = [], []
    for c in plugins:
        p = paths.get(c.lower())
        if p:
            order.append((c.lower(), p))
        else:
            missing.append(c)
    if missing:
        # a folder Mod Organizer left out of openmw.cfg: look beside the data folders it did list
        parents = sorted({os.path.dirname(d.rstrip('/\\')) for d in data_dirs if d})
        siblings = []
        for par in parents:
            try:
                siblings.extend(os.path.join(par, e) for e in os.listdir(par))
            except OSError:
                pass
        extra = find_files([d for d in siblings if os.path.isdir(d)], missing)
        still = []
        for c in missing:
            p = extra.get(c.lower())
            if p:
                log(f'Found {c} outside the listed data folders: {p}')
            else:
                still.append(c)
        missing = still
        order = [(c.lower(), paths.get(c.lower()) or extra.get(c.lower())) for c in plugins
                 if paths.get(c.lower()) or extra.get(c.lower())]
    world.files = [c for c, _ in order]
    if missing:
        log(f'{len(missing)} content files not found (skipped): ' + ', '.join(missing[:10])
            + (' ...' if len(missing) > 10 else ''))
    t0 = time.time()
    masters = {}
    for i, (c, p) in enumerate(order):
        masters[c] = world.read_records(i, p)
    log(f'Read {len(order)} content files in {time.time() - t0:.0f} s: {len(world.dials)} dialogue topics, '
        f'{len(world.scripts)} scripts, {len(world.objects)} objects')
    quests = build(world)
    wanted = needed_ids(world, quests)
    items = {i for i in wanted if i in world.objects}
    for hs in holders_of(world, items).values():
        wanted.update(hs)
    t1 = time.time()
    for i, (c, p) in enumerate(order):
        world.read_cells(i, p, masters[c], wanted)
    log(f'Placed objects found in {time.time() - t1:.0f} s')
    nq, no = write_output(world, quests, args.out, world.files)
    log(f'Wrote {nq} quests and {no} objects to {os.path.abspath(args.out)}')
    return 0


if __name__ == '__main__':
    code = 1
    try:
        code = main()
    except Exception as e:  # keep the window open to show what went wrong
        import traceback
        traceback.print_exc()
    if getattr(sys, 'frozen', False) and sys.stdin and sys.stdin.isatty() and '--no-pause' not in sys.argv:
        input('Press Enter to close.')
    sys.exit(code)
