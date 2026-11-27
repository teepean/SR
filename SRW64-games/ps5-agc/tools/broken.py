#!/usr/bin/env python3
# samples of one thread whose chain lacks <root> (broken unwinds): their outermost known frames, in ms per frame
# usage: broken.py <sqlite> <thread name> <frames> [root] [outer frames, default 3] [top N]
import collections, sqlite3, sys
db, tname, frames = sys.argv[1], sys.argv[2], float(sys.argv[3])
root = sys.argv[4] if len(sys.argv) > 4 else 'GuestGpu::ExecutionThread'
k = int(sys.argv[5]) if len(sys.argv) > 5 else 3
topn = int(sys.argv[6]) if len(sys.argv) > 6 else 30
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
        chains[sid].append((d, strs.get(sym, '?')))
def short(s):
    return s.split('(')[0][-70:]
agg = collections.Counter(); n = 0
for sid, ch in chains.items():
    ch.sort()
    names = [short(s) for d, s in ch]
    if any(root in x for x in names):
        continue
    n += 1
    agg[tuple(reversed(names[-k:]))] += 1
ms = period / 1e6
print('%d of %d samples lack %s: %.2f ms/frame' % (n, len(chains), root, n * ms / frames))
for p, m in agg.most_common(topn):
    print('%6.3f ms/frame  %s' % (m * ms / frames, ' > '.join(p)))
