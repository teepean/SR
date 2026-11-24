#! /usr/bin/env python3
# ysrsites.py <libysr log> [marker substring] - the call sites of the last "[ysr] site" report, grouped by marker,
# mapped to the game functions (IDA_FUNCS); SR_EXE=<SRW64 build> maps addresses of a recompiled program back first
import bisect, collections, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
starts, names = [], []
for line in open(os.environ['IDA_FUNCS']):
    p = line.split(); starts.append(int(p[0], 16)); names.append(p[1])
srmap = None
if os.environ.get('SR_EXE'):
    from srmap import SRMap
    srmap = SRMap(os.environ['SR_EXE'])
def function(address):
    if srmap is not None:
        r = srmap.original(address)
        if r is None: return '0x%x' % address
        address = r[0]
    a = address - 0x400000
    i = bisect.bisect_right(starts, a) - 1
    return '%s+%x' % (names[i], a - starts[i]) if i >= 0 else '0x%x' % a
lines = open(sys.argv[1], errors='replace').read().split('\n')
last = max((i for i, l in enumerate(lines) if l.startswith('[ysr] frames')), default=-1)
report = []
for l in lines[last + 1:]:
    m = re.match(r'\[ysr\] site 0x([0-9a-f]+) \(\S+\) (\S+(?: of helper\))?)\s+(.*?)\s+([0-9.]+)/frame', l)
    if m: report.append((int(m.group(1), 16), m.group(2), m.group(3).strip(), float(m.group(4))))
    elif l.startswith('[ysr] frames'): break
pattern = sys.argv[2] if len(sys.argv) > 2 else ''
by_marker = collections.defaultdict(list)
for address, kind, marker, per_frame in report:
    if pattern in marker: by_marker[marker].append((per_frame, kind, function(address)))
for marker, sites in sorted(by_marker.items(), key=lambda kv: -sum(s[0] for s in kv[1])):
    print('%-40s %8.1f/frame' % (marker, sum(s[0] for s in sites)))
    for per_frame, kind, where in sorted(sites, reverse=True):
        print('    %8.1f  %-28s %s' % (per_frame, kind, where))
