#!/usr/bin/env python3
"""patch_pe.py <PE file> <patches.txt>: byte patches applied to a copy of a game module before SRW translates it.

patches.txt lines: <virtual address> <original bytes> <new bytes> [; comment] (hex; bytes without spaces). Every
patch checks the original bytes first (another version of the file: an error, nothing is written). Used for changes
SRW should translate as ordinary code (instruction_replacements.sci text needs the flags SRW's llasm output tracks)."""
import struct, sys

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    fn = sys.argv[1]
    d = bytearray(open(fn, 'rb').read())
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = pe + 24
    base = struct.unpack_from('<I', d, opt + 28)[0]
    sec = opt + struct.unpack_from('<H', d, pe + 20)[0]
    sections = []
    for i in range(nsec):
        s = sec + 40 * i
        vsize, va, rsize, raw = struct.unpack_from('<IIII', d, s + 8)
        sections.append((va, max(vsize, rsize), raw, rsize))
    n = 0
    for line in open(sys.argv[2]):
        line = line.split(';', 1)[0].strip()
        if not line:
            continue
        addr, old, new = line.split()
        rva = int(addr, 16) - base
        old, new = bytes.fromhex(old), bytes.fromhex(new)
        if len(old) != len(new):
            sys.exit('patch_pe: %s: the original and the new bytes differ in length' % addr)
        for va, size, raw, rsize in sections:
            if va <= rva < va + size and rva - va + len(old) <= rsize:
                off = raw + rva - va
                break
        else:
            sys.exit('patch_pe: %s is not in the file' % addr)
        if d[off:off + len(old)] != old:
            sys.exit('patch_pe: %s: expected %s, found %s' % (addr, old.hex(), d[off:off + len(old)].hex()))
        d[off:off + len(new)] = new
        n += 1
    open(fn, 'wb').write(d)
    print('patch_pe: %d patches' % n)

if __name__ == '__main__':
    main()
