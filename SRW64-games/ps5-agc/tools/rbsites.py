#!/usr/bin/env python3
# Kyty's "[kyty-readback-sites]" lines (KYTY_READBACK_SITES=1: per 10 s the top instructions faulting on GPU-written
# memory, "0x<rip> <count> <ms> @0x<data address>;") summed over a game log, the rips mapped through funcmap.py's maps
# of the SRW64 build to the original game address and function.
# usage: rbsites.py <play log> <map dir> [top N] [skip first K lines (warm-up)]
import collections, re, sys
import numpy as np

log, mapdir = sys.argv[1], sys.argv[2]
topn = int(sys.argv[3]) if len(sys.argv) > 3 else 25
skip = int(sys.argv[4]) if len(sys.argv) > 4 else 0
rec, org = [], []
for line in open(mapdir + '/exe_labels.txt'):
    a, o = line.split()
    a = int(a, 16)
    if a >= 0x8000000:
        rec.append(a)
        org.append(int(o, 16))
order = np.argsort(rec)
rec = np.array(rec, dtype=np.int64)[order]
org = np.array(org, dtype=np.int64)[order]
fe = np.load(mapdir + '/func_entries.npy')
lo, hi = int(rec[0]), int(rec[-1]) + 0x1000

def game(ip):
    if not lo <= ip < hi:
        return None
    k = int(np.searchsorted(rec, ip, side='right')) - 1
    j = int(np.searchsorted(fe, int(org[k]), side='right')) - 1
    return int(org[k]), int(fe[j])

item = re.compile(r'0x([0-9a-f]+) (\d+) (\d+) @0x([0-9a-f]+);')
count, ms, addrs = collections.Counter(), collections.Counter(), collections.defaultdict(set)
lines = 0
for line in open(log, errors='replace'):
    if not line.startswith('[kyty-readback-sites]'):
        continue
    lines += 1
    if lines <= skip:
        continue
    for rip, n, t, addr in item.findall(line):
        rip = int(rip, 16)
        count[rip] += int(n)
        ms[rip] += int(t)
        addrs[rip].add(int(addr, 16))
used = max(lines - skip, 0)
print('%d readback-site lines (%d used, ~%d s): %d faults, %.0f ms' % (lines, used, 10 * used, sum(count.values()), sum(ms.values())))
byfunc = collections.Counter()
print('   ms/10s  count/10s  site (original address, function)        data addresses')
for rip, t in ms.most_common(topn):
    g = game(rip)
    where = 'game 0x%X in loc_%X' % g if g else 'host 0x%x' % rip
    a = sorted(addrs[rip])
    print('  %7.0f  %9.0f  %-40s %s' % (t / max(used, 1), count[rip] / max(used, 1), where,
                                        ' '.join('0x%x' % x for x in a[:3]) + (' ...' if len(a) > 3 else '')))
for rip, t in ms.items():
    g = game(rip)
    byfunc['loc_%X' % g[1] if g else 'host'] += t
print('by function (ms per 10 s):', ', '.join('%s %.0f' % (k, v / max(used, 1)) for k, v in byfunc.most_common(10)))
