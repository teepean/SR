#! /usr/bin/env python3
# dumptab.py <va> <before> <after> - qwords of the PS5 eboot data with relocations, strings, floats
import struct, sys, os
E = os.environ['PS5_EBOOT']
d = open(E, 'rb').read()
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
    if r_info & 0xffffffff == 8: rel[r_offset] = r_addend
def s(va):
    o = fo(va)
    if o is None: return ''
    e = d.find(b'\0', o, o + 80); t = d[o:e]
    return '"%s"' % t.decode() if e > o and all(32 <= c < 127 for c in t) else ''
base = int(sys.argv[1], 0); before = int(sys.argv[2], 0); after = int(sys.argv[3], 0)
for a in range(base - before, base + after, 8):
    if a in rel:
        print('0x%x: -> 0x%x %s' % (a, rel[a], s(rel[a])))
    else:
        o = fo(a)
        q = d[o:o + 8] if o is not None else b''
        if len(q) == 8:
            lo, hi = struct.unpack('<ff', q)
            print('0x%x: %016x  i(%d,%d) f(%g,%g)' % (a, struct.unpack('<Q', q)[0], *struct.unpack('<ii', q), lo, hi))
