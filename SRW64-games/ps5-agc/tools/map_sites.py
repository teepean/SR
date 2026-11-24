#! /usr/bin/env python3
# Maps AnyKyty RIPs (PS5 address + 0x400000) from Kyty/libysr logs to game functions.
# usage: map_sites.py <log> [pattern (default readback-sites)] [max sites] [context instructions]
import bisect, re, sys, collections, os
import capstone

EBOOT = os.environ['PS5_EBOOT']                  # converted PS5 eboot (PS5 va -> file offset va + 0x4000)
FUNCS = os.environ['IDA_FUNCS']                  # IDA function list (address name - start-end)
DECOMP = os.environ.get('IDA_DECOMP', '')        # optional Hex-Rays output of the whole program
BIAS = 0x400000

log, pattern = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else 'readback-sites')
max_sites = int(sys.argv[3]) if len(sys.argv) > 3 else 25
context = int(sys.argv[4]) if len(sys.argv) > 4 else 10

starts, ends, names = [], [], []
for line in open(FUNCS):
    p = line.split()
    a, b = p[3].split('-')
    starts.append(int(p[0], 16)); ends.append(int(b, 16)); names.append(p[1])

totals = collections.defaultdict(lambda: [0, 0.0])
data_addresses = collections.defaultdict(set)
for line in open(log, errors='replace'):
    if pattern not in line: continue
    for m in re.finditer(r'0x([0-9a-f]+)\s+(\d+)\s+([0-9.]+)(?:\s*@0x([0-9a-f]+))?', line):
        rip = int(m.group(1), 16)
        totals[rip][0] += int(m.group(2)); totals[rip][1] += float(m.group(3))
        if m.group(4): data_addresses[rip].add(int(m.group(4), 16))

d = open(EBOOT, 'rb').read()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
byfunc = collections.defaultdict(lambda: [0, 0.0])
for rip, (count, ms) in sorted(totals.items(), key=lambda x: -x[1][1])[:max_sites]:
    va = rip - BIAS
    i = bisect.bisect_right(starts, va) - 1
    fn = names[i] if i >= 0 and va < ends[i] else '?'
    byfunc[fn][0] += count; byfunc[fn][1] += ms
    print('=== rip 0x%x (PS5 0x%x) %s  count %d  %.1f ms  data %s' % (rip, va, fn, count, ms, ' '.join('0x%x' % a for a in sorted(data_addresses[rip])[:6])))
    if fn != '?':
        start = max(starts[i], va - 0x60)
        ins = []
        for s in range(start, va + 1):     # find decode start that lands on va
            ins = list(md.disasm(d[0x4000 + s:0x4000 + va + 16], s))
            if any(x.address == va for x in ins): break
        k = [j for j, x in enumerate(ins) if x.address == va]
        if k:
            for x in ins[max(0, k[0] - context):k[0] + 2]:
                print('  %s%x: %s %s' % ('>' if x.address == va else ' ', x.address, x.mnemonic, x.op_str))
print('\n=== by function')
for fn, (count, ms) in sorted(byfunc.items(), key=lambda x: -x[1][1]):
    print('%-14s %8d %10.1f ms' % (fn, count, ms))
