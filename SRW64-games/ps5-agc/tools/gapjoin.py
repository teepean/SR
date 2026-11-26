#!/usr/bin/env python3
# GPU idle gaps of an nsys capture (Vulkan GPU workload trace) joined with what the emulator's execute thread did in
# them (sr64-9f, independent cross-check of libprosperopkg-4a's gap_execute.py).
#   gaps: busy = union of VULKAN_WORKLOAD intervals; a gap ends at the next workload's start; "starved" when that
#   workload's vkQueueSubmit (VULKAN_API, same correlationId) began after the gap began (the GPU had nothing queued).
#   execute: the thread named kytyGpuExecute with the most samples; samples inside the starved part of each gap
#   (gap start .. submit start) by inclusive frame; off-CPU time from SCHED_EVENTS when present.
# usage: gapjoin.py <export.sqlite> [min gap us, default 200]
import bisect, collections, sqlite3, sys

db = sys.argv[1]
min_gap = int(sys.argv[2]) * 1000 if len(sys.argv) > 2 else 200_000
c = sqlite3.connect(db)
strs = dict(c.execute('select id, value from StringIds'))

wl = sorted(c.execute('select start, end, correlationId from VULKAN_WORKLOAD'))
sub = {}
for start, end, corr, name in c.execute('select start, end, correlationId, nameId from VULKAN_API'):
    if 'QueueSubmit' in strs.get(name, ''):
        sub[corr] = (start, end)
t0, t1 = wl[0][0], max(e for s, e, k in wl)
span = (t1 - t0) / 1e9
busy, gaps, cur_end = 0, [], wl[0][1]
busy_start = wl[0][0]
for s, e, corr in wl[1:]:
    if s > cur_end:
        busy += cur_end - busy_start
        submit = sub.get(corr)
        gaps.append((cur_end, s, submit[0] if submit else None))
        busy_start = s
    cur_end = max(cur_end, e)
busy += cur_end - busy_start
starved = [(a, b, x) for a, b, x in gaps if x is not None and x > a]
waiting = [(a, b, x) for a, b, x in gaps if x is None or x <= a]
print('%.2f s, GPU busy %.1f %%, %d workloads; gaps %d (%.0f ms/s): starved %.0f ms/s, queued-but-idle %.0f ms/s' % (
    span, 100.0 * busy / (t1 - t0), len(wl), len(gaps), sum(b - a for a, b, x in gaps) / 1e6 / span,
    sum(b - a for a, b, x in starved) / 1e6 / span, sum(b - a for a, b, x in waiting) / 1e6 / span))
hist = collections.Counter()
for a, b, x in starved:
    d = b - a
    hist['<50us' if d < 50_000 else '<200us' if d < 200_000 else '<1ms' if d < 1_000_000 else '>=1ms'] += d
print('starved gap time by size (ms/s):', {k: round(v / 1e6 / span, 2) for k, v in hist.items()})

# execute thread
tn = {g: strs.get(n, '?') for n, g in c.execute('select nameId, globalTid from ThreadNames')}
cnt = collections.Counter(g for (g,) in c.execute('select globalTid from COMPOSITE_EVENTS'))
ex = max((g for g, n in tn.items() if n == 'kytyGpuExecute'), key=lambda g: cnt[g], default=None)
if ex is None:
    sys.exit('no kytyGpuExecute thread')
big = sorted((a, x) for a, b, x in starved if b - a >= min_gap)
starts = [a for a, x in big]
def in_big(t):
    k = bisect.bisect_right(starts, t) - 1
    return k >= 0 and big[k][0] <= t < big[k][1]
ids = set(sid for sid, t, g in c.execute('select id, start, globalTid from COMPOSITE_EVENTS') if g == ex and in_big(t))
incl, leaf, n = collections.Counter(), collections.Counter(), 0
chains = collections.defaultdict(list)
for sid, sym, mod, depth in c.execute('select id, symbol, module, stackDepth from SAMPLING_CALLCHAINS'):
    if sid in ids:
        chains[sid].append((depth, strs.get(sym, '?').split('(')[0][-80:], strs.get(mod, '?').rsplit('/', 1)[-1]))
for sid, ch in chains.items():
    ch.sort()
    n += 1
    leaf['%s!%s' % (ch[0][2], ch[0][1])] += 1
    for k in set(s for d, s, m in ch):
        incl[k] += 1
window = sum(x - a for a, x in big)
print('execute thread tid %d: %d samples inside %d starved gaps >= %d us (starved windows %.0f ms/s)' % (
    ex & 0xffffff, n, len(big), min_gap // 1000, window / 1e6 / span))
for title, ctr in (('inclusive', incl), ('leaf', leaf)):
    print(' ', title + ':')
    for k, v in ctr.most_common(22):
        print('    %5.1f %%  %s' % (100.0 * v / max(n, 1), k))
try:
    off = 0
    rows = sorted(c.execute('select start, cpu, isSchedIn, globalTid from SCHED_EVENTS where globalTid = ?', (ex,)))
    last_out = None
    for t, cpu, sched_in, g in rows:
        if not sched_in:
            last_out = t
        elif last_out is not None:
            # off-CPU interval [last_out, t] clipped to the starved windows
            k = bisect.bisect_right(starts, last_out) - 1
            for j in range(max(k, 0), len(big)):
                a, x = big[j]
                if a > t:
                    break
                off += max(0, min(t, x) - max(last_out, a))
            last_out = None
    print('  execute off-CPU inside the starved windows: %.0f ms/s of %.0f' % (off / 1e6 / span, window / 1e6 / span))
except sqlite3.OperationalError as e:
    print('  no SCHED_EVENTS:', e)
