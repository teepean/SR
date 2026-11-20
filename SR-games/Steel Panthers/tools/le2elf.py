#!/usr/bin/env python3
"""Convert a (possibly DOS/4GW-bound) LE executable into an ELF32 i386 image.

Objects are placed at their LE relocation base addresses and internal fixups are
applied, so addresses match SR's loc_XXXXX labels. Used only for analysis
(objdump / Ghidra / REA); the recompiler itself reads the original EXE.

usage: le2elf.py STEEL.EXE out.elf [--info]
"""
import struct
import sys


def find_le(data):
    """Walk the MZ/BW chain of a bound executable and return (mz_offset, le_offset)."""
    # same walk as SR_loader.c: MZ size = last + 512*pages - 512, BW size = last + 512*pages
    off = 0
    while off + 0x40 <= len(data):
        sig = data[off:off + 2]
        if sig == b'MZ':
            lfanew = struct.unpack_from('<I', data, off + 0x3c)[0]
            reloc = struct.unpack_from('<H', data, off + 0x18)[0]
            if reloc == 0x40 and (lfanew & 0xffff) and data[off + lfanew:off + lfanew + 2] == b'LE':
                return off, off + lfanew
        elif sig != b'BW':
            break
        last, pages = struct.unpack_from('<HH', data, off + 2)
        off += last + 512 * pages - (512 if sig == b'MZ' else 0)
    raise SystemExit('LE header not found')


def parse(path):
    data = open(path, 'rb').read()
    mz, le = find_le(data)
    h = lambda fmt, o: struct.unpack_from(fmt, data, le + o)[0]
    info = {
        'mz': mz, 'le': le,
        'eip_obj': h('<I', 0x18), 'eip': h('<I', 0x1c),
        'esp_obj': h('<I', 0x20), 'esp': h('<I', 0x24),
        'page_size': h('<I', 0x28), 'last_page': h('<I', 0x2c),
        'npages': h('<I', 0x14),
    }
    obj_tab = le + h('<I', 0x40)
    nobj = h('<I', 0x44)
    page_tab = le + h('<I', 0x48)
    fix_page_tab = le + h('<I', 0x68)
    fix_rec_tab = le + h('<I', 0x6c)
    data_pages = mz + h('<I', 0x80)
    psz = info['page_size']

    objs = []
    for i in range(nobj):
        vsize, base, flags, pidx, pcnt, _ = struct.unpack_from('<6I', data, obj_tab + 24 * i)
        objs.append(dict(vsize=vsize, base=base, flags=flags, pidx=pidx, pcnt=pcnt))

    # load pages
    for o in objs:
        img = bytearray(max(o['vsize'], o['pcnt'] * psz))
        for p in range(o['pcnt']):
            pn = o['pidx'] + p  # 1-based
            e = data[page_tab + 4 * (pn - 1):page_tab + 4 * pn]
            num = (e[0] << 16) | (e[1] << 8) | e[2]
            src = data_pages + (num - 1) * psz
            ln = info['last_page'] if num == info['npages'] else psz
            img[p * psz:p * psz + ln] = data[src:src + ln]
        o['img'] = img

    # apply fixups
    nfix = 0
    kinds = {}
    fixups = []  # (source address, target address)
    for oi, o in enumerate(objs):
        for p in range(o['pcnt']):
            pn = o['pidx'] + p
            start = struct.unpack_from('<I', data, fix_page_tab + 4 * (pn - 1))[0]
            end = struct.unpack_from('<I', data, fix_page_tab + 4 * pn)[0]
            r = fix_rec_tab + start
            rend = fix_rec_tab + end
            while r < rend:
                stype, flg = data[r], data[r + 1]
                r += 2
                if stype & 0x20:
                    cnt = data[r]; r += 1
                    srcs = None
                else:
                    srcs = [struct.unpack_from('<h', data, r)[0]]; r += 2
                if (flg & 3) != 0:
                    raise SystemExit('non-internal fixup at page %d' % pn)
                if flg & 0x40:
                    tobj = struct.unpack_from('<H', data, r)[0]; r += 2
                else:
                    tobj = data[r]; r += 1
                st = stype & 0x0f
                toff = 0
                if st != 2:
                    if flg & 0x10:
                        toff = struct.unpack_from('<I', data, r)[0]; r += 4
                    else:
                        toff = struct.unpack_from('<H', data, r)[0]; r += 2
                if srcs is None:
                    srcs = list(struct.unpack_from('<%dh' % cnt, data, r)); r += 2 * cnt
                tgt = objs[tobj - 1]['base'] + toff
                for s in srcs:
                    pos = p * psz + s
                    kinds[st] = kinds.get(st, 0) + 1
                    fixups.append((o['base'] + pos, tgt))
                    nfix += 1
                    if pos < 0 or pos >= len(o['img']):
                        continue
                    if st == 7:  # 32-bit offset
                        struct.pack_into('<I', o['img'], pos, tgt & 0xffffffff)
                    elif st == 8:  # 32-bit self-relative
                        if pos + 4 <= len(o['img']):
                            struct.pack_into('<i', o['img'], pos, tgt - (o['base'] + pos + 4))
                    elif st == 5:  # 16-bit offset
                        struct.pack_into('<H', o['img'], pos, tgt & 0xffff)
                    elif st == 2:  # 16-bit selector
                        struct.pack_into('<H', o['img'], pos, 0x1000 + 8 * tobj)
                    elif st == 6:  # 16:32 pointer
                        struct.pack_into('<IH', o['img'], pos, tgt, 0x1000 + 8 * tobj)
                    elif st == 3:  # 16:16 pointer
                        struct.pack_into('<HH', o['img'], pos, tgt & 0xffff, 0x1000 + 8 * tobj)
    info['objs'] = objs
    info['nfix'] = nfix
    info['fixups'] = fixups
    info['fix_kinds'] = kinds
    return info


def write_elf(info, out):
    objs = info['objs']
    entry = objs[info['eip_obj'] - 1]['base'] + info['eip']
    phoff = 52
    nph = len(objs)
    off = (phoff + 32 * nph + 0xfff) & ~0xfff
    phdrs = b''
    body = bytearray()
    cur = off
    for o in objs:
        img = bytes(o['img'][:o['vsize']])
        pad = (-cur) & 0xfff
        body += b'\0' * pad
        cur += pad
        flags = 4 | (1 if o['flags'] & 4 else 0) | (2 if o['flags'] & 2 else 0)
        phdrs += struct.pack('<8I', 1, cur, o['base'], o['base'], len(img), o['vsize'], flags, 0x1000)
        body += img
        cur += len(img)
    ehdr = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01' + b'\0' * 9, 2, 3, 1,
                       entry, phoff, 0, 0, 52, 32, nph, 40, 0, 0)
    with open(out, 'wb') as f:
        f.write(ehdr + phdrs + b'\0' * (off - 52 - len(phdrs)) + body)


if __name__ == '__main__':
    info = parse(sys.argv[1])
    for i, o in enumerate(info['objs']):
        print('object %d: base 0x%x vsize 0x%x flags 0x%x pages %d..%d' % (
            i + 1, o['base'], o['vsize'], o['flags'], o['pidx'], o['pidx'] + o['pcnt'] - 1))
    print('entry: object %d + 0x%x, stack: object %d + 0x%x' % (
        info['eip_obj'], info['eip'], info['esp_obj'], info['esp']))
    print('LE header at 0x%x (inner MZ at 0x%x), %d fixups, kinds %s' % (
        info['le'], info['mz'], info['nfix'], info['fix_kinds']))
    if len(sys.argv) > 2 and not sys.argv[2].startswith('--'):
        write_elf(info, sys.argv[2])
        print('wrote', sys.argv[2])
