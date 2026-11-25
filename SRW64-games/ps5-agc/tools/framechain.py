#!/usr/bin/env python3
# Frame-centric critical chain: what each main-thread wait of frame N+1 waits for, back to the
# game's submission that carries the releasing operation.
# usage: framechain.py <play log> <wait trace> <exec timeline> [frames to print]
#
# Frame boundary S_N: the frame-end submit batch (the small graphics DCB, by default the most frequent graphics submit
# size, here 38 dwords, submitted together with q30/q38/q72 at the end of loc_11E0FE0).
# Per frame: the frame labels 0x2000008020 (graphics frame end, release-mem) and 0x2000000000 (write-data, waited on
# by site 4): parse, execute (exec timeline), completion; the Ocean query label (srhooks-qtrace); the relay label of
# site 7. Each main-thread wait of the frame is tied to its releasing write, and the write to the submission it came in.
import bisect, collections, re, sys

log, waits_path, timeline_path = sys.argv[1:4]
nprint = int(sys.argv[4]) if len(sys.argv) > 4 else 3

re_lab = re.compile(r'\[kyty-labeltrace\] t=(\d+) addr=0x([0-9a-f]+) value=0x([0-9a-f]+) q([0-9a-f]+) (\w+)(?: seq=(\d+))?')
re_sub = re.compile(r'\[kyty-labeltrace\] t=(\d+) submit (?:graphics dwords (\d+)|compute queue (\d+) \(index \d+\) dwords (\d+)|flip (\d+))')
re_q = re.compile(r'\[srhooks-qtrace\] frame \d+ record (\d+)-(\d+) written (\d+) .*label 0x([0-9a-f]+) value 0x([0-9a-f]+)')
re_qt = re.compile(r'\[kyty-queuetrace\] t=(\d+) q([0-9a-f]+) process (\w+)')
re_irq = re.compile(r'\[kyty-irq\] t=(\d+) q([0-9a-f]+) ctx (\d+) recorded (\d+) delay ([\d.]+) ms')

lab = collections.defaultdict(list)
subs = []                                   # (t, queue ('gfx' or int), dwords)
qrec = []
qtrace = []
irqs = []
pipe = []
for line in open(log, errors='replace'):
    if line.startswith('[kyty-labeltrace] t='):
        m = re_lab.match(line)
        if m:
            lab[int(m.group(2), 16)].append((int(m.group(1)), int(m.group(3), 16), m.group(5),
                                             int(m.group(6)) if m.group(6) else None, m.group(4)))
            continue
        m = re_sub.match(line)
        if m:
            t = int(m.group(1))
            if m.group(2):
                subs.append((t, 'gfx', int(m.group(2))))
            elif m.group(3):
                subs.append((t, int(m.group(3)), int(m.group(4))))
            else:
                subs.append((t, 'flip', 0))
    elif line.startswith('[srhooks-qtrace]'):
        m = re_q.match(line)
        if m:
            qrec.append(tuple(int(m.group(k)) for k in (1, 2, 3)) + (int(m.group(4), 16), int(m.group(5), 16)))
    elif line.startswith('[kyty-queuetrace] t='):
        m = re_qt.match(line)
        if m:
            qtrace.append((int(m.group(1)), m.group(3)))
    elif line.startswith('[kyty-irq]'):
        m = re_irq.match(line)
        if m:
            irqs.append((int(m.group(1)), m.group(2), int(m.group(4)), float(m.group(5))))
    elif line.startswith('[kyty-pipe]'):
        pipe.append(line.rstrip())
for v in lab.values():
    v.sort()

tl = {}
t0 = t1 = None
for line in open(timeline_path):
    if line[0] == '#':
        continue
    p = line.split()
    s, e, seq = int(p[0]), int(p[1]), int(p[2])
    tl[seq] = (s, e, int(p[3]), int(p[4]))
    t0 = s if t0 is None else min(t0, s)
    t1 = e if t1 is None else max(t1, e)

waits = []
for line in open(waits_path):
    if line[0] == '#':
        continue
    p = line.split()
    w = dict(site=int(p[0]), tid=int(p[1]), enter=int(p[2]), exit=int(p[3]))
    if len(p) >= 6:
        w['label'], w['value'] = int(p[4], 16), int(p[5])
    waits.append(w)
c2 = collections.Counter(w['tid'] for w in waits if w['site'] == 2)
main = (c2 or collections.Counter(w['tid'] for w in waits)).most_common(1)[0][0]
mw = sorted((w for w in waits if w['tid'] == main), key=lambda w: w['enter'])
relay = sorted((w for w in waits if w['site'] == 7), key=lambda w: w['exit'])
relay_exit = [w['exit'] for w in relay]
ev6 = sorted((w for w in waits if w['site'] == 6), key=lambda w: w['exit'])

def exec_of(seq):
    return tl.get(seq)

def write_time(e):
    """when the value is visible to the CPU: early / completion at their time, write-data at its op's execute end"""
    if e[2] == 'writedata':
        op = exec_of(e[3])
        return op[1] if op else None
    return e[0]

def first_visible(addr, value, after=0):
    """the earliest event that makes label >= value visible (value None: any), and its parse event"""
    best = None
    for e in lab.get(addr, []):
        if e[0] < after - 2000000000:
            continue
        if e[2] in ('early', 'completion', 'writedata') and (value is None or e[1] >= value):
            wt = write_time(e)
            if wt is not None and wt >= after and (best is None or wt < best[0]):
                best = (wt, e)
            if best and e[0] > best[0]:
                break
    return best

def parse_of(addr, value, near):
    cand = [e for e in lab.get(addr, []) if e[2] in ('parse', 'writedata') and e[1] == value and e[0] <= near + 1]
    return cand[-1] if cand else None

def last_submit_before(t, queue=None):
    k = bisect.bisect_right(subs, (t, 'zzzz')) - 1
    while k >= 0:
        if queue is None or subs[k][1] == queue:
            return subs[k]
        k -= 1
    return None

# frame boundaries: the frame-end graphics DCB size
gsizes = collections.Counter(d for (t, q, d) in subs if q == 'gfx')
fsize = gsizes.most_common(1)[0][0]
S = [t for (t, q, d) in subs if q == 'gfx' and d == fsize and t0 <= t <= t1]
print('window %.0f ms; main thread %d; frame-end graphics DCB %d dwords; %d frames, %.2f ms mean period' % (
    (t1 - t0) / 1e6, main, fsize, len(S) - 1, (S[-1] - S[0]) / 1e6 / max(1, len(S) - 1)))

L8020, L0, LQ = 0x2000008020, 0x2000000000, 0x2000000080
agg = collections.defaultdict(list)
for n in range(len(S) - 1):
    sN, sN1 = S[n], S[n + 1]
    row = {}
    # graphics frame end: 0x2000008020 events right after S_N
    p = [e for e in lab.get(L8020, []) if sN <= e[0] < sN1 and e[2] == 'parse']
    if p:
        pe = p[0]
        row['8020 parse'] = pe[0] - sN
        op = exec_of(pe[3])
        if op:
            row['8020 exec'] = op[1] - sN
        for e in lab[L8020]:
            if e[1] == pe[1] and e[2] == 'early' and e[0] >= pe[0]:
                row['8020 early'] = e[0] - sN
                break
        for e in lab[L8020]:
            if e[1] == pe[1] and e[2] == 'completion' and e[0] >= pe[0]:
                row['8020 completion'] = e[0] - sN
                break
    wd = [e for e in lab.get(L0, []) if sN <= e[0] < sN1 and e[2] == 'writedata']
    if wd:
        row['0 parse'] = wd[0][0] - sN
        op = exec_of(wd[0][3])
        if op:
            row['0 exec'] = op[1] - sN
    # main-thread waits of this frame (between S_N and S_N+1)
    fw = [w for w in mw if sN <= w['enter'] < sN1]
    waited = 0
    for w in fw:
        d = (w['exit'] - w['enter']) / 1e6
        waited += d
        agg['wait site %d' % w['site']].append(d)
        origin = None
        if w['site'] in (4, 5) and 'label' in w:
            fv = first_visible(w['label'], w['value'], w['enter'] - 50000000)
            if fv:
                pr = parse_of(w['label'], fv[1][1], fv[1][0])
                origin = last_submit_before(pr[0] if pr else fv[1][0], 'gfx')
                agg['site %d release -> exit' % w['site']].append((w['exit'] - fv[0]) / 1e6)
                if origin:
                    agg['site %d: submit -> visible' % w['site']].append((fv[0] - origin[0]) / 1e6)
                    agg['site %d: origin frame offset' % w['site']].append((sN - origin[0]) / 1e6)
        elif w['site'] == 2:
            q = [r for r in qrec if r[2] <= w['exit'] + 100000 and r[2] >= w['enter'] - 100000]
            if q:
                r = q[-1]
                agg['site 2: record end -> label written'].append((r[2] - r[1]) / 1e6)
                sub72 = last_submit_before(r[2], 72)
                if sub72:
                    agg['site 2: q72 submit -> label written'].append((r[2] - sub72[0]) / 1e6)
                    agg['site 2: record end -> q72 submit (game)'].append((sub72[0] - r[1]) / 1e6)
        elif w['site'] == 1 and relay:
            k = bisect.bisect_right(relay_exit, w['exit']) - 1
            if k >= 0 and relay[k]['exit'] >= w['enter'] - 1000000:
                r = relay[k]
                agg['site 1: relay release -> exit (job)'].append((w['exit'] - r['exit']) / 1e6)
                if 'label' in r:
                    fv = first_visible(r['label'], r['value'], r['enter'] - 100000000)
                    if fv:
                        e = fv[1]
                        agg['site 1: relay label written by %s q%s' % (e[2], e[4])].append(1)
                        pr = parse_of(r['label'], e[1], e[0])
                        if pr:
                            op = exec_of(pr[3])
                            agg['site 1: label parse -> visible'].append((fv[0] - pr[0]) / 1e6)
                            if op:
                                agg['site 1: parse -> exec start'].append((op[0] - pr[0]) / 1e6)
                                agg['site 1: exec -> visible'].append((fv[0] - op[0]) / 1e6)
                        agg['site 1: visible -> relay release'].append((r['exit'] - fv[0]) / 1e6)
                        # the relay's event wait (site 6) that preceded the release: interrupt delivery
                        k6 = bisect.bisect_right([x['exit'] for x in ev6], r['enter'] + 1) - 1
                        if k6 >= 0:
                            agg['site 1: visible -> relay woken (irq)'].append((ev6[k6]['exit'] - fv[0]) / 1e6)
                    agg['relay label 0x%x' % r['label']].append(1)
    agg['frame period'].append((sN1 - sN) / 1e6)
    agg['main waited'].append(waited)
    agg['main own work'].append((sN1 - sN) / 1e6 - waited)
    for k, v in row.items():
        agg['S_N -> ' + k].append(v / 1e6)
    if n < nprint:
        print('\nframe %d (S_N at +%.1f ms, period %.2f ms):' % (n, (sN - t0) / 1e6, (sN1 - sN) / 1e6))
        print('  ' + '  '.join('%s %+.2f' % (k, v / 1e6) for k, v in row.items()))
        for w in fw:
            if w['exit'] - w['enter'] > 200000:
                print('  wait site %d  %+.2f .. %+.2f ms (%.2f)%s' % (w['site'], (w['enter'] - sN) / 1e6,
                      (w['exit'] - sN) / 1e6, (w['exit'] - w['enter']) / 1e6,
                      ('  label 0x%x >= %d' % (w['label'], w['value'])) if 'label' in w else ''))

print('\naggregate (mean ms; n):')
for k in sorted(agg):
    v = agg[k]
    if all(x == 1 for x in v):
        print('  %-48s count %d' % (k, len(v)))
    else:
        v2 = sorted(v)
        print('  %-48s mean %7.2f  median %7.2f  n %d' % (k, sum(v) / len(v), v2[len(v2) // 2], len(v)))
for line in pipe[-3:]:
    print(line[:300])
