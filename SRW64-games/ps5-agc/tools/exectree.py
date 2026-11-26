#!/usr/bin/env python3
# inclusive call tree (ms per frame) of one thread from an nsys SQLite export (DWARF samples): paths below a root frame,
# frames shortened to their function names; usage: exectree.py <sqlite> <thread name> <frames in window> [root] [min %] [depth]
import collections, sqlite3, sys
db, tname, frames = sys.argv[1], sys.argv[2], float(sys.argv[3])
root = sys.argv[4] if len(sys.argv) > 4 else 'CommandProcessor::Execute'
minpct = float(sys.argv[5]) if len(sys.argv) > 5 else 0.7
maxdepth = int(sys.argv[6]) if len(sys.argv) > 6 else 7
c = sqlite3.connect(db)
strs = dict(c.execute('select id, value from StringIds'))
tn = {g: strs.get(n, '?') for n, g in c.execute('select nameId, globalTid from ThreadNames')}
cnt = collections.Counter(g for (g,) in c.execute('select globalTid from COMPOSITE_EVENTS'))
th = max((g for g, n in tn.items() if n == tname), key=lambda g: cnt[g])
times = sorted(t for t, g in c.execute('select start, globalTid from COMPOSITE_EVENTS') if g == th)
deltas = sorted(b - a for a, b in zip(times, times[1:]) if b > a)
period = deltas[len(deltas) // 4] if deltas else 0            # typical interval between samples while running
span = (times[-1] - times[0]) / 1e9
ids = set(sid for sid, g in c.execute('select id, globalTid from COMPOSITE_EVENTS') if g == th)
chains = collections.defaultdict(list)
for sid, sym, depth in c.execute('select id, symbol, stackDepth from SAMPLING_CALLCHAINS'):
    if sid in ids:
        chains[sid].append((depth, strs.get(sym, '?')))
def short(s):
    s = s.split('(')[0]
    for p in ('Libs::Graphics::', 'Libs::LibKernel::', 'Common::', 'std::'):
        s = s.replace(p, '')
    return s[-60:]
paths = collections.Counter(); n = len(chains); underroot = 0
for sid, ch in chains.items():
    ch.sort(reverse=True)                                  # outermost first
    names = [short(s) for d, s in ch]
    if root not in names:
        continue
    underroot += 1
    i = names.index(root)
    sub = names[i:i + maxdepth + 1]
    for k in range(1, len(sub) + 1):
        paths[tuple(sub[:k])] += 1
ms_per_sample = period / 1e6
print('thread %s tid %d: %d samples over %.1f s, sample interval %.3f ms (on-CPU %.0f %%), %d under %s; %.0f frames' % (
    tname, th & 0xffffff, n, span, ms_per_sample, 100.0 * n * ms_per_sample / 1e3 / span, underroot, root, frames))
for p, v in sorted(paths.items(), key=lambda kv: kv[0]):
    if 100.0 * v / n < minpct:
        continue
    print('%6.2f ms/frame %5.1f %%  %s%s' % (v * ms_per_sample / frames, 100.0 * v / n, '  ' * (len(p) - 1), p[-1]))
