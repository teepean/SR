#!/usr/bin/env python3
# callers of a frame on one thread from an nsys SQLite export (DWARF samples), in ms per frame: for every sample whose
# chain holds a frame matching <target>, the <depth> frames above its innermost match, aggregated.
# usage: callersof.py <sqlite> <thread name> <frames in window> <target substring> [depth, default 5] [top N, default 25]
import collections, sqlite3, sys
db, tname, frames, target = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
depth = int(sys.argv[5]) if len(sys.argv) > 5 else 5
topn = int(sys.argv[6]) if len(sys.argv) > 6 else 25
c = sqlite3.connect(db)
strs = dict(c.execute('select id, value from StringIds'))
tn = {g: strs.get(n, '?') for n, g in c.execute('select nameId, globalTid from ThreadNames')}
cnt = collections.Counter(g for (g,) in c.execute('select globalTid from COMPOSITE_EVENTS'))
th = max((g for g, n in tn.items() if n == tname), key=lambda g: cnt[g])
times = sorted(t for t, g in c.execute('select start, globalTid from COMPOSITE_EVENTS') if g == th)
deltas = sorted(b - a for a, b in zip(times, times[1:]) if b > a)
period = deltas[len(deltas) // 4] if deltas else 0
ids = set(sid for sid, g in c.execute('select id, globalTid from COMPOSITE_EVENTS') if g == th)
chains = collections.defaultdict(list)
for sid, sym, d in c.execute('select id, symbol, stackDepth from SAMPLING_CALLCHAINS'):
    if sid in ids:
        chains[sid].append((d, strs.get(sym, '?')))
def short(s):
    s = s.split('(')[0]
    return s[-90:]
agg = collections.Counter(); hit = 0
for sid, ch in chains.items():
    ch.sort()                                              # innermost first
    names = [short(s) for d, s in ch]
    k = next((i for i, n in enumerate(names) if target in n), None)
    if k is None:
        continue
    hit += 1
    agg[tuple(names[k:k + depth + 1])] += 1
ms = period / 1e6
print('%s: %d samples, %d hold %s = %.2f ms/frame' % (tname, len(chains), hit, target, hit * ms / frames))
for path, n in agg.most_common(topn):
    print('%6.3f ms/frame  %s' % (n * ms / frames, ' < '.join(path)))
