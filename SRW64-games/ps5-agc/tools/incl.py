#!/usr/bin/env python3
# inclusive ms/frame of one thread's samples (broken stacks included) whose chain holds each given substring
# usage: incl.py <sqlite> <thread name> <frames> <substring>...
import collections, sqlite3, sys
db, tname, frames = sys.argv[1], sys.argv[2], float(sys.argv[3])
keys = sys.argv[4:]
c = sqlite3.connect(db)
strs = dict(c.execute('select id, value from StringIds'))
tn = {g: strs.get(n, '?') for n, g in c.execute('select nameId, globalTid from ThreadNames')}
cnt = collections.Counter(g for (g,) in c.execute('select globalTid from COMPOSITE_EVENTS'))
th = max((g for g, n in tn.items() if n == tname), key=lambda g: cnt[g])
times = sorted(t for t, g in c.execute('select start, globalTid from COMPOSITE_EVENTS') if g == th)
deltas = sorted(b - a for a, b in zip(times, times[1:]) if b > a)
period = deltas[len(deltas) // 4]
ids = set(sid for sid, g in c.execute('select id, globalTid from COMPOSITE_EVENTS') if g == th)
chains = collections.defaultdict(list)
for sid, sym, d in c.execute('select id, symbol, stackDepth from SAMPLING_CALLCHAINS'):
    if sid in ids:
        chains[sid].append(strs.get(sym, '?'))
hits = collections.Counter()
for sid, ch in chains.items():
    for k in keys:
        if any(k in s for s in ch):
            hits[k] += 1
ms = period / 1e6
print('%s: %d samples, %.2f ms/frame on-CPU' % (tname, len(chains), len(chains) * ms / frames))
for k in keys:
    print('%7.2f ms/frame  %s' % (hits[k] * ms / frames, k))
