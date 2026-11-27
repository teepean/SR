#!/usr/bin/env python3
# gcnfootprint.py <disassembly from disgcn.py> - a shader's memory footprint by a linear scan: which bytes of the user-data
# table (U = s[0:1]) and of the pointer tables loaded from it the scalar loads read, and which descriptors (V#[table+offset])
# the buffer / image operations use; dynamic offsets are listed as such. A linear scan ignores control flow, so a register
# reused for another value can be misattributed: treat fixed offsets as solid and anything else as a lead.
import re, sys, collections
lines = [l.strip() for l in open(sys.argv[1]) if l.strip()]
origin = {0: 'U.lo', 1: 'U.hi'}           # sgpr -> origin text
def sgprs(tok):
    m = re.match(r's\[(\d+):(\d+)\]', tok)
    if m: return list(range(int(m.group(1)), int(m.group(2)) + 1))
    m = re.match(r's(\d+)$', tok)
    if m: return [int(m.group(1))]
    return []
def base_name(regs):
    o = [origin.get(r, '?s%d' % r) for r in regs]
    if len(regs) >= 2 and o[0].endswith('.lo') and o[1].endswith('.hi') and o[0][:-3] == o[1][:-3]:
        return o[0][:-3]
    if len(regs) == 4 and all(x.startswith(o[0].rsplit('+', 1)[0]) for x in o):
        return 'V#@' + o[0]
    return '(' + ','.join(o) + ')'
uses = collections.OrderedDict()
for l in lines:
    op = l.split(' ', 1)[0]
    args = [a.strip() for a in l[len(op):].split(',')]
    if op.startswith('s_load_') or op.startswith('s_buffer_load_'):
        dst, base = args[0], args[1]
        off = args[2] if len(args) > 2 else '0'
        off = 0 if off == 'null' else int(off, 0) if re.match(r'^(0x[0-9a-f]+|\d+)$', off) else off
        b = base_name(sgprs(base))
        n = {'dword': 1, 'dwordx2': 2, 'dwordx4': 4, 'dwordx8': 8, 'dwordx16': 16}[op.split('_')[-1]]
        key = (op.startswith('s_buffer'), b)
        uses.setdefault(key, set()).add((off, n * 4) if isinstance(off, int) else (str(off), n * 4))
        d = sgprs(dst)
        for i, r in enumerate(d):
            if isinstance(off, int):
                origin[r] = '%s+0x%x' % (b, off + 4 * i)
            else:
                origin[r] = '%s+%s+%d' % (b, off, 4 * i)
        # pairs: mark 64-bit pointers as .lo/.hi for later use as bases
        for i in range(0, len(d) - 1, 2):
            if isinstance(off, int):
                p = '[%s+0x%x]' % (b, off + 4 * i)
                origin[d[i]] = p + '.lo'; origin[d[i + 1]] = p + '.hi'
        if len(d) % 4 == 0 and isinstance(off, int):
            for i in range(0, len(d), 4):
                v = 'V#[%s+0x%x]' % (b, off + 4 * i)
                for k in range(4): origin[d[i + k]] = v
    elif op.startswith('buffer_') or op.startswith('image_'):
        # find the descriptor operand s[a:b]
        for a in args:
            r = sgprs(a.split(' ')[0])
            if len(r) in (4, 8):
                o = origin.get(r[0], '?s%d' % r[0])
                uses.setdefault((op.startswith('image') and 'image' or 'buffer', o), set()).add(op)
                break
    else:
        # plain sgpr writes kill origins (s_mov copies)
        if op.startswith('s_') and args and sgprs(args[0]):
            d = sgprs(args[0])
            if op in ('s_mov_b32', 's_mov_b64') and sgprs(args[1]):
                src = sgprs(args[1])
                for i, r in enumerate(d):
                    origin[r] = origin.get(src[i] if i < len(src) else -1, '?')
            else:
                for r in d: origin[r] = 'alu'
for (kind, b), offs in uses.items():
    if kind in (True, False):
        offs2 = sorted(o for o in offs if isinstance(o[0], int))
        lo = min((o[0] for o in offs2), default=0); hi = max((o[0] + o[1] for o in offs2), default=0)
        other = [o for o in offs if not isinstance(o[0], int)]
        print('%s from %-28s bytes 0x%x..0x%x (%d loads)%s' % ('s_buffer_load' if kind else 's_load', b, lo, hi, len(offs), (' + dynamic %s' % other) if other else ''))
    else:
        print('%s via %-30s %s' % (kind, b, ' '.join(sorted(offs))))
