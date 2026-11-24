#! /usr/bin/env python3
# pushmap.py <pushmap.tsv> [min pushes per frame] - libysr YSR_PUSHMAP output -> marker tree with the game functions
# (PS5 addresses; site = function containing the PushMarker call, callers from the rbp chain)
# SR_EXE=<recompiled executable>: the log comes from an SRW64 build - addresses are mapped back with srmap.py
import bisect, collections, sys, os
FUNCS = os.environ['IDA_FUNCS']                  # IDA function list (address name - start-end)
starts, names = [], []
for line in open(FUNCS):
    p = line.split(); starts.append(int(p[0], 16)); names.append(p[1])
srmap = None
if os.environ.get('SR_EXE'):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from srmap import SRMap
    srmap = SRMap(os.environ['SR_EXE'])
def fn(text):
    if text in ('-', '') or text.startswith('!'): return text
    a = int(text, 16)
    if srmap is not None:
        r = srmap.original(a + 0x400000)       # libysr printed address - 0x400000
        if r is None: return text
        a = r[0] - 0x400000
    i = bisect.bisect_right(starts, a) - 1
    return '%s+%x' % (names[i], a - starts[i]) if i >= 0 else text
minimum = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
rows = []
for line in open(sys.argv[1]):
    if line.startswith('#'): print(line.rstrip()); continue
    count, per_frame, depth, marker, parent, site, c1, c2, c3 = line.rstrip('\n').split('\t')
    if float(per_frame) < minimum: continue
    rows.append((parent, marker, float(per_frame), int(depth), fn(site), fn(c1), fn(c2), fn(c3)))
children = collections.defaultdict(list)
for r in rows: children[r[0]].append(r)
seen = set()
def show(parent, indent):
    for r in sorted(children.get(parent, []), key=lambda r: -r[2]):
        key = (r[0], r[1], r[4], r[5])
        print('%s%-40s %7.2f/frame  site %-22s callers %s < %s < %s' % ('  ' * indent, r[1], r[2], r[4], r[5], r[6], r[7]))
        if r[1] not in seen:
            seen.add(r[1]); show(r[1], indent + 1)
show('(none)', 0)
