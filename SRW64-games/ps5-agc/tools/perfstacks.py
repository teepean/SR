#! /usr/bin/env python3
# perfstacks.py <perf script output> <tid> [depth] - call-chain profile of one thread of a recompiled program
# record: perf record --call-graph fp -t <tid> -F 499 -o perf.data -- sleep 30
#         perf script -i perf.data -F tid,ip,sym,dso > stacks.txt
# Every sample's frames inside the game executable are mapped back to the original functions (SR_EXE=<SRW64 build>,
# IDA_FUNCS=<function list>); prints the leaf functions and, for each of the top leaves, the callers at each depth.
import bisect, collections, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from srmap import SRMap
srmap = SRMap(os.environ['SR_EXE'])
starts, names = [], []
for line in open(os.environ['IDA_FUNCS']):
    p = line.split(); starts.append(int(p[0], 16)); names.append(p[1])
exe = os.path.basename(os.environ['SR_EXE'])
def function(address):
    r = srmap.original(address)
    if r is None: return None
    a = r[0] - 0x400000
    i = bisect.bisect_right(starts, a) - 1
    return names[i] if i >= 0 else None
tid, depth = sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 4
samples, stack, current = [], [], None
for line in open(sys.argv[1], errors='replace'):
    if not line.strip():
        if current == tid: samples.append(stack)
        stack, current = [], None
        continue
    if not line.startswith('\t'):           # sample header (" <tid>"); frames start with a tab
        current = line.split()[0].split('/')[-1]
        continue
    m = re.match(r'\s*([0-9a-f]+)\s+(.*)\((.*)\)\s*$', line)
    if not m: continue
    address, dso = int(m.group(1), 16), m.group(3)
    stack.append(function(address) if dso.endswith(exe) else '[%s]' % os.path.basename(dso))
if current == tid: samples.append(stack)
total = len(samples)
print('samples of thread %s: %d' % (tid, total))
leaves = collections.Counter(s[0] if s else '?' for s in samples)
for leaf, count in leaves.most_common(12):
    print('%6.1f%%  %s' % (100.0 * count / total, leaf))
    chains = collections.Counter(' < '.join(str(f) for f in s[1:1 + depth]) for s in samples if s and s[0] == leaf)
    for chain, n in chains.most_common(5):
        print('           %5.1f%%  < %s' % (100.0 * n / total, chain))
