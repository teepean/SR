#!/usr/bin/env python3
# CPU profile of a recompiled game's process from an nsys SQLite export (CPU IP samples with DWARF backtraces).
# Samples whose instruction pointer is in the recompiled game code are mapped back to the original game function:
#   recompiled address -> nearest preceding loc_<orig> label of the SRW64 build (exe_labels.txt, from nm)
#   -> original address -> containing function (func_entries.npy: call targets + prologues of the input exe).
# usage: cpuprof.py <export.sqlite> <map dir (funcmap.py)> [top N] [thread name/tid filter]
import bisect, collections, sqlite3, sys
import numpy as np

db, here = sys.argv[1], sys.argv[2]
topn = int(sys.argv[3]) if len(sys.argv) > 3 else 25
want = sys.argv[4] if len(sys.argv) > 4 else None
lab_rec, lab_orig = [], []
for line in open(here + '/exe_labels.txt'):
    a, o = line.split()
    a = int(a, 16)
    if a >= 0x8000000:                      # recompiled code (data labels keep their original addresses)
        lab_rec.append(a)
        lab_orig.append(int(o, 16))
order = np.argsort(lab_rec)
lab_rec = np.array(lab_rec, dtype=np.int64)[order]
lab_orig = np.array(lab_orig, dtype=np.int64)[order]
fentries = np.load(here + '/func_entries.npy')
code_lo, code_hi = int(lab_rec[0]), int(lab_rec[-1]) + 0x1000

# srh_spin_* wait loops of the ps5-agc hooks (instruction_replacements.sci) by original address: the wait sites
SPIN = {0xFAF4A0: 'spin Ocean label (site 2)', 0x12A76F0: 'spin job wait (site 1)', 0x12A74E0: 'spin job group (site 3)',
        0x11E3EC0: 'spin frame fence (site 4)', 0x11E37E0: 'spin label 11E37E0', 0x11E4210: 'spin 11E4210'}

def game_func(ip):
    k = int(np.searchsorted(lab_rec, ip, side='right')) - 1
    if k < 0:
        return None
    orig = int(lab_orig[k])
    j = int(np.searchsorted(fentries, orig, side='right')) - 1
    return orig, (int(fentries[j]) if j >= 0 else orig)

c = sqlite3.connect(db)
strs = dict(c.execute('select id, value from StringIds'))
tname = {}
for nameId, gtid in c.execute('select nameId, globalTid from ThreadNames'):
    tname[gtid] = strs.get(nameId, '?')
# samples: id -> (thread, callchain rows)
samples = {}
for sid, gtid in c.execute('select id, globalTid from COMPOSITE_EVENTS'):
    samples[sid] = gtid
chains = collections.defaultdict(list)
for sid, sym, mod, ip, depth in c.execute('select id, symbol, module, originalIP, stackDepth from SAMPLING_CALLCHAINS'):
    chains[sid].append((depth, sym, mod, ip))
start, end = c.execute('select min(start), max(start) from COMPOSITE_EVENTS').fetchone()
span = (end - start) / 1e9

per_thread = collections.Counter(samples.values())
print('%d samples over %.1f s; threads with most samples:' % (len(samples), span))
for g, n in per_thread.most_common(14):
    print('  %-28s tid %-8d %6d samples' % (tname.get(g, '?')[:28], g & 0xffffff, n))

def frame_name(sym, mod, ip):
    ip = ip or 0
    if code_lo <= ip < code_hi:
        gf = game_func(ip)
        if gf:
            orig, f = gf
            for s, n in SPIN.items():
                if 0 <= orig - s < 0x20:
                    return n
            return 'game loc_%X' % f
    m = strs.get(mod, '?').rsplit('/', 1)[-1]
    return '%s!%s' % (m, strs.get(sym, '?')[:60])

def report(gtid_filter):
    selfc, firstgame, mods = collections.Counter(), collections.Counter(), collections.Counter()
    n = 0
    for sid, g in samples.items():
        if g not in gtid_filter:
            continue
        ch = sorted(chains.get(sid, []))
        if not ch:
            continue
        n += 1
        d, sym, mod, ip = ch[0]
        selfc[frame_name(sym, mod, ip)] += 1
        mods[('eboot (game)' if code_lo <= (ip or 0) < code_hi else strs.get(mod, '?').rsplit('/', 1)[-1])] += 1
        for d, sym, mod, ip in ch:
            if code_lo <= (ip or 0) < code_hi:
                firstgame[frame_name(sym, mod, ip)] += 1
                break
        else:
            firstgame['(no game frame)'] += 1
    print('\n  %d samples; by module of the leaf:' % n)
    for k, v in mods.most_common(8):
        print('    %5.1f%%  %s' % (100.0 * v / n, k))
    print('  self (leaf frame):')
    for k, v in selfc.most_common(topn):
        print('    %5.1f%%  %s' % (100.0 * v / n, k))
    print('  first game frame on the stack (the game code that is running or calling out):')
    for k, v in firstgame.most_common(topn):
        print('    %5.1f%%  %s' % (100.0 * v / n, k))

targets = [g for g, _ in per_thread.most_common(14)]
if want:
    targets = [g for g in per_thread if want in tname.get(g, '') or want == str(g & 0xffffff)]
for g in targets[:4] if not want else targets:
    print('\n=== thread %s (tid %d, %d samples)' % (tname.get(g, '?'), g & 0xffffff, per_thread[g]))
    report({g})
