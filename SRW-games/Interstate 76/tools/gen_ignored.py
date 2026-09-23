#!/usr/bin/env python3
# Write ignored_areas.sci entries for dead functions of a recompiled module:
#   - functions redirected via external_procedures.sci (always dead), and
#   - functions SRW did not label as code whose every IDA reference comes from other dead functions
#     (fixpoint over the IDA xref graph; references from outside any dead function keep a function alive).
# Each area runs from the function start to the next function start (covers trailing jump tables).
# usage: gen_ignored.py <ida_out_dir> <seg01.inc> <external_procedures.sci|-> <out.sci> [keep_addr_hex...]
import sys, csv, re, bisect
d, seg, ext, out = sys.argv[1:5]
keep = {int(x, 16) for x in sys.argv[5:]}
funcs = sorted((int(r[0], 16), int(r[1], 16), r[2]) for r in csv.reader(open(d + '/funcs.csv')))
starts = [f[0] for f in funcs]
xr = {}
for r in csv.reader(open(d + '/func_xrefs.csv')):
    xr[int(r[0], 16)] = [int(c.lstrip('d'), 16) for c in r[3].split(';')] if r[3] else []
forced = set()
if ext != '-':
    for l in open(ext):
        l = l.strip()
        if l: forced.add(int(l.split(',')[0][4:], 16))
lines = open(seg).read().split('\n')
kind = {}
for i, l in enumerate(lines):
    m = re.match(r'^loc_([0-9A-F]+):$', l)
    if m:
        j = i + 1
        while j < len(lines) and re.match(r'^[A-Za-z_@?$][\w@?$]*:$', lines[j]): j += 1
        kind[int(m.group(1), 16)] = 'data' if re.match(r'^(db|dd|dw|times|align)', lines[j]) else 'code'
dead = {a for a, e, n in funcs if (a in forced) or (kind.get(a) != 'code' and a not in keep)}
rng = {a: e for a, e, n in funcs}
def in_dead(x):
    i = bisect.bisect_right(starts, x) - 1
    return i >= 0 and starts[i] in dead and x < rng[starts[i]]
changed = True
while changed:
    changed = False
    for a in sorted(dead):
        if a in forced: continue
        if any(not in_dead(src) for src in xr.get(a, [])):
            dead.discard(a); changed = True
res = []
for i, (a, e, n) in enumerate(funcs):
    if a not in dead: continue
    end = funcs[i + 1][0] if i + 1 < len(funcs) else e
    res.append((a, end - a, n))
with open(out, 'w') as f:
    for a, l, n in res: f.write('loc_%X,%d\n' % (a, l))
print(len(res), 'areas ->', out)
