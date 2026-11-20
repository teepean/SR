#!/usr/bin/env python3
"""Translate the SR configuration (SCI files) of one STEEL.EXE build to another.

Every loc_XXXXX in the files (also inside replacement text and comments, and the sp_jt_XXXXX
table labels) is mapped with addrmap.Mapper. For instruction_replacements.sci the replaced bytes
must be the same in both builds. Unmapped addresses are reported and the line is kept with a
"; UNMAPPED" marker so that the build fails visibly.

usage: port_sci.py OLD.EXE NEW.EXE OLD_SCI_DIR NEW_SCI_DIR [OLD_AIL.txt NEW_AIL.txt]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addrmap  # noqa: E402

old_exe, new_exe, src, dst = sys.argv[1:5]
m = addrmap.Mapper(old_exe, new_exe)
cache = {}
problems = []

# Miles AIL API functions are look-alike wrappers: map them by name (tools/ailfind.py output)
ail_old = {}
ail_new = {}
if len(sys.argv) > 6:
    for line in open(sys.argv[5]):
        a, n = line.split()[:2]
        ail_old.setdefault(int(a, 16), n)
    for line in open(sys.argv[6]):
        a, n = line.split()[:2]
        ail_new.setdefault(n, int(a, 16))
for a, n in ail_old.items():
    if n in ail_new:
        cache[a] = (ail_new[n], 'AIL name ' + n)


def map_addr(a):
    if a in cache:
        return cache[a]
    if a == m.a.data_base:
        r = (m.b.data_base, 'object base')
    elif a >= m.a.data_base:
        r = m.data(a)
    else:
        r = map_code(a)
    cache[a] = r
    return r


def by_neighbours(a, span=0x800):
    """repeated code (e.g. the CLIB fill blocks): use the shift of the uniquely mapped addresses
    around a, if those before and after agree and the bytes at the shifted address match"""
    deltas = []
    for p in list(range(a - span, a, 0x20)) + list(range(a + 0x20, a + span, 0x20)):
        n, _ = m.code(p)
        if n is not None:
            deltas.append((abs(p - a), n - p, p < a))
    before = sorted(d for d in deltas if d[2])
    after = sorted(d for d in deltas if not d[2])
    if not before or not after or before[0][1] != after[0][1]:
        return None, 'no unique match (neighbours disagree)'
    d = before[0][1]
    oa, ob = a - m.a.code_base, a + d - m.b.code_base
    for k in range(24):
        if not (m.a.mask[oa + k] or m.b.mask[ob + k] or m.a.code[oa + k] == m.b.code[ob + k]):
            return None, 'neighbour shift %+x but bytes differ' % d
    return a + d, 'neighbours %+x' % d


def map_code(a):
    n, how = m.code(a)
    if n is None:
        n, how = by_neighbours(a)
    return n, how


def subst(text, where):
    def rep(mo):
        a = int(mo.group(2), 16)
        n, how = map_addr(a)
        if n is None:
            problems.append('%s: %s%X %s' % (where, mo.group(1), a, how))
            return mo.group(0)
        return '%s%X' % (mo.group(1), n)
    return re.sub(r'(loc_|sp_jt_)([0-9A-Fa-f]{5,6})\b', rep, text)


os.makedirs(dst, exist_ok=True)
for name in sorted(os.listdir(src)):
    if not name.endswith('.sci'):
        continue
    out = []
    for ln, line in enumerate(open(os.path.join(src, name)).read().split('\n'), 1):
        where = '%s:%d' % (name, ln)
        new = subst(line, where)
        if name == 'instruction_replacements.sci' and line.startswith('loc_'):
            a = int(line[4:line.index(',')], 16)
            size = int(line.split(',')[1])
            n, _ = map_addr(a)
            if n is not None:
                oa, ob = a - m.a.code_base, n - m.b.code_base
                same = all(m.a.mask[oa + k] or m.b.mask[ob + k] or m.a.code[oa + k] == m.b.code[ob + k]
                           for k in range(size))
                if not same:
                    problems.append('%s: bytes at %X differ from %X' % (where, n, a))
        if problems and problems[-1].startswith(where) and not new.endswith('; UNMAPPED'):
            new += ' ; UNMAPPED'
        out.append(new)
    open(os.path.join(dst, name), 'w').write('\n'.join(out))

# order check: mapped code addresses must increase with the old ones
pairs = sorted((a, r[0], r[1]) for a, r in cache.items() if r[0] is not None and a < m.a.data_base)
for (a0, n0, h0), (a1, n1, h1) in zip(pairs, pairs[1:]):
    if n1 <= n0:
        problems.append('order: %X->%X (%s) then %X->%X (%s)' % (a0, n0, h0, a1, n1, h1))
open(os.path.join(dst, 'addrmap.txt'), 'w').write(''.join('%X %s %s\n' % (a, '%X' % r[0] if r[0] is not None else '-', r[1]) for a, r in sorted(cache.items())))
print('%d addresses mapped, %d problems' % (len(cache), len(problems)))
for p in problems:
    print('  ' + p)
