#!/usr/bin/env python3
# per-site main-thread wait time in the last <seconds> of a wait trace (the bench window ends with the game), ms/s and
# ms/frame at the given fps.  usage: waitsum.py <wait trace> <fps> [seconds, default 90]
import collections, sys
path, fps = sys.argv[1], float(sys.argv[2]); span = float(sys.argv[3]) if len(sys.argv) > 3 else 90.0
ws = [l.split() for l in open(path) if l[0] != '#']
end = max(int(w[3]) for w in ws)
start = end - int(span * 1e9) - int(5e9)            # the last 5 s before the end: shutdown, skip
end = end - int(5e9)
c2 = collections.Counter(w[1] for w in ws if w[0] == '1')
main = c2.most_common(1)[0][0]
tot = collections.defaultdict(float); n = collections.Counter()
for w in ws:
    if w[1] != main and w[0] not in ('6', '7'):
        continue
    a, b = max(int(w[2]), start), min(int(w[3]), end)
    if b > a:
        tot[w[0]] += (b - a) / 1e6; n[w[0]] += 1
frames = fps * span
print('%s: main %s, %.0f s window, %.1f fps' % (path.split('/')[-1], main, span, fps))
for s in sorted(tot):
    print('  site %s: %6.1f ms/s  %5.2f ms/frame  (%d waits)' % (s, tot[s] / span, tot[s] / frames, n[s]))
print('  main thread waits total: %.2f ms/frame of %.2f' % (sum(v for k, v in tot.items() if k not in ('6', '7')) / frames, 1000 / fps))
