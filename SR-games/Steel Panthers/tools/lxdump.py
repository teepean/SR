#!/usr/bin/env python3
"""Minimal LX (OS/2 linear executable) DLL loader for the Miles VFX drivers (VESA480.DLL etc.).

Loads objects at their relocation base, applies internal fixups, prints the entry table and
writes each object to <out_prefix>.objN.bin for disassembly.

usage: lxdump.py VESA480.DLL out_prefix
"""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
lx = struct.unpack_from('<I', d, 0x3c)[0]
assert d[lx:lx + 2] == b'LX', 'not LX'
H = lambda f, o: struct.unpack_from(f, d, lx + o)[0]
page_size = H('<I', 0x28)
page_shift = H('<I', 0x2c)
obj_tab = lx + H('<I', 0x40)
nobj = H('<I', 0x44)
page_map = lx + H('<I', 0x48)
res_names = lx + H('<I', 0x58)
entry_tab = lx + H('<I', 0x5c)
fix_page = lx + H('<I', 0x68)
fix_rec = lx + H('<I', 0x6c)
data_pages = H('<I', 0x80)
eip_obj, eip = H('<I', 0x18), H('<I', 0x1c)

objs = []
for i in range(nobj):
    vsize, base, flags, pidx, pcnt, _ = struct.unpack_from('<6I', d, obj_tab + 24 * i)
    img = bytearray(max(vsize, pcnt * page_size))
    for p in range(pcnt):
        off, size, fl = struct.unpack_from('<IHH', d, page_map + 8 * (pidx - 1 + p))
        src = data_pages + (off << page_shift)
        img[p * page_size:p * page_size + size] = d[src:src + size]
    objs.append(dict(vsize=vsize, base=base, flags=flags, pidx=pidx, pcnt=pcnt, img=img))
    print('object %d base 0x%x size 0x%x flags 0x%x' % (i + 1, base, vsize, flags))

for oi, o in enumerate(objs):
    for p in range(o['pcnt']):
        pn = o['pidx'] + p
        a = struct.unpack_from('<I', d, fix_page + 4 * (pn - 1))[0]
        b = struct.unpack_from('<I', d, fix_page + 4 * pn)[0]
        r = fix_rec + a
        while r < fix_rec + b:
            st, fl = d[r], d[r + 1]; r += 2
            if st & 0x20:
                cnt = d[r]; r += 1; srcs = None
            else:
                srcs = [struct.unpack_from('<h', d, r)[0]]; r += 2
            assert (fl & 3) == 0, 'external fixup'
            if fl & 0x40:
                tobj = struct.unpack_from('<H', d, r)[0]; r += 2
            else:
                tobj = d[r]; r += 1
            toff = 0
            if (st & 0xf) != 2:
                if fl & 0x10:
                    toff = struct.unpack_from('<I', d, r)[0]; r += 4
                else:
                    toff = struct.unpack_from('<H', d, r)[0]; r += 2
            if srcs is None:
                srcs = list(struct.unpack_from('<%dh' % cnt, d, r)); r += 2 * cnt
            tgt = objs[tobj - 1]['base'] + toff
            for s in srcs:
                pos = p * page_size + s
                if 0 <= pos <= len(o['img']) - 4:
                    if (st & 0xf) == 7:
                        struct.pack_into('<I', o['img'], pos, tgt)
                    elif (st & 0xf) == 8:
                        struct.pack_into('<i', o['img'], pos, tgt - (o['base'] + pos + 4))

# resident names
r = res_names
while d[r]:
    n = d[r]; name = d[r + 1:r + 1 + n].decode('latin1'); ordn = struct.unpack_from('<H', d, r + 1 + n)[0]
    print('resident name %r ordinal %d' % (name, ordn)); r += 3 + n

# entry table
r = entry_tab
ordinal = 1
while d[r]:
    cnt, typ = d[r], d[r + 1]; r += 2
    if typ == 0:
        ordinal += cnt; continue
    obj = struct.unpack_from('<H', d, r)[0]; r += 2
    for k in range(cnt):
        if typ == 3:  # 32-bit entry
            fl, off = struct.unpack_from('<BI', d, r); r += 5
            print('entry ordinal %d -> object %d + 0x%x = 0x%x' % (ordinal, obj, off, objs[obj - 1]['base'] + off))
        else:
            raise SystemExit('entry type %d' % typ)
        ordinal += 1
print('eip object %d + 0x%x' % (eip_obj, eip))
for i, o in enumerate(objs):
    open('%s.obj%d.bin' % (sys.argv[2], i + 1), 'wb').write(bytes(o['img'][:o['vsize']]))
