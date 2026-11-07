#!/usr/bin/env python3
"""LithTech REZ (RezMgr v1) archive tool: list / extract.
  rez.py list FILE.REZ [pattern]
  rez.py extract FILE.REZ OUTDIR [pattern]
Layout (lithshared/rezmgr): 127-byte text header, then u32 version, root dir pos, root dir size, ...;
directory blocks hold entries: u32 1 = directory (pos, size, time, name), u32 0 = resource
(pos, size, time, id, type, numkeys, name, description, keys[numkeys]); type = 4 chars of the
extension, stored reversed (little-endian)."""
import fnmatch, os, struct, sys

def u32(d, o): return struct.unpack_from('<I', d, o)[0]

def cstr(d, o):
    e = d.index(b'\0', o)
    return d[o:e].decode('latin-1'), e + 1

def typ(t):
    return struct.pack('<I', t).rstrip(b'\0').decode('latin-1')[::-1]

def walk(d, pos, size, path, out):
    o, end = pos, pos + size
    while o < end:
        kind = u32(d, o); o += 4
        if kind == 1:
            p, s, t = struct.unpack_from('<3I', d, o); o += 12
            name, o = cstr(d, o)
            walk(d, p, s, path + name + '/', out)
        elif kind == 0:
            p, s, t, rid, ty, nk = struct.unpack_from('<6I', d, o); o += 24
            name, o = cstr(d, o)
            desc, o = cstr(d, o)
            o += 4 * nk
            ext = typ(ty)
            out.append((path + name + ('.' + ext if ext else ''), p, s))
        else:
            raise ValueError('bad entry type %d at 0x%x' % (kind, o - 4))

def entries(fn):
    d = open(fn, 'rb').read()
    ver, rpos, rsize = struct.unpack_from('<3I', d, 127)
    if ver != 1: raise ValueError('not a RezMgr v1 file')
    out = []
    walk(d, rpos, rsize, '', out)
    return d, out

def main(a):
    if len(a) < 3 or a[1] not in ('list', 'extract'):
        print(__doc__); return 1
    d, ents = entries(a[2])
    pats = a[4:] if a[1] == 'extract' else a[3:]
    sel = [e for e in ents if not pats or any(fnmatch.fnmatch(e[0].lower(), p.lower()) for p in pats)]
    if a[1] == 'list':
        for n, p, s in sel: print('%10d  %s' % (s, n))
    else:
        for n, p, s in sel:
            dst = os.path.join(a[3], *n.split('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f: f.write(d[p:p + s])
        print('%d files' % len(sel))
    return 0

if __name__ == '__main__': sys.exit(main(sys.argv))
