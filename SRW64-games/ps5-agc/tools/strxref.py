#! /usr/bin/env python3
# strxref.py <string>... - code (lea rip) and data (relative relocation) references to strings in the PS5 eboot
import bisect, re, struct, sys, os
E = os.environ['PS5_EBOOT']
d = open(E, 'rb').read()
code = d[0x4000:0x4000 + 0x17e8d0c]
fs = sorted(int(l.split()[0], 16) for l in open(os.environ['IDA_FUNCS']))
phoff, = struct.unpack_from('<Q', d, 0x20); phnum, = struct.unpack_from('<H', d, 0x38)
loads = []; dyn = None
for i in range(phnum):
    t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', d, phoff + 56 * i)
    if t == 1: loads.append((va, off, fsz))
    if t == 2: dyn = (va, off, fsz)
def fo(va):
    for v, o, sz in loads:
        if v <= va < v + sz: return o + va - v
tags = {}
for i in range(0, dyn[2], 16):
    tag, val = struct.unpack_from('<qQ', d, dyn[1] + i); tags.setdefault(tag, val)
rel = {}
for i in range(0, tags[8], 24):
    r_offset, r_info, r_addend = struct.unpack_from('<QQq', d, fo(tags[7]) + i)
    if r_info & 0xffffffff == 8: rel.setdefault(r_addend, []).append(r_offset)
targets = {}
for name in sys.argv[1:]:
    pos = d.find(name.encode() + b'\0')
    while pos > 0 and d[pos - 1] != 0: pos = d.find(name.encode() + b'\0', pos + 1)
    if pos < 0: print(name, 'not found'); continue
    va = pos - 0x4000
    targets[va] = name
    print('%s at 0x%x; data refs: %s' % (name, va, ' '.join('0x%x' % r for r in rel.get(va, []))))
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code):
    a = m.start(); disp, = struct.unpack_from('<i', code, a + 3)
    t = a + 7 + disp
    if t in targets:
        print('  %s: lea at 0x%x in sub_%X' % (targets[t], a, fs[bisect.bisect_right(fs, a) - 1]))
