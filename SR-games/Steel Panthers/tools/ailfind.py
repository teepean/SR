#!/usr/bin/env python3
"""Locate the Miles AIL API functions of a STEEL.EXE build by their trace strings ("AIL_xxx(").
The function containing a reference to the string is the nearest call target at or below it.
usage: ailfind.py STEEL.EXE  -> lines "addr name"
"""
import bisect, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addrmap

b = addrmap.Build(sys.argv[1])
code, base = b.code, b.code_base
targets = set()
for o in range(len(code) - 5):
    if code[o] == 0xe8:
        t = base + o + 5 + int.from_bytes(code[o + 1:o + 5], 'little', signed=True)
        if base <= t < base + len(code):
            targets.add(t)
targets = sorted(targets)
res = {}
for mo in re.finditer(rb'AIL_[A-Za-z0-9_]+\(', b.data):
    s = b.data_base + mo.start()
    name = mo.group(0)[:-1].decode()
    for src in b.by_target.get(s, []):
        if not (base <= src < base + len(code)):
            continue
        i = bisect.bisect_right(targets, src) - 1
        if i >= 0:
            res.setdefault(name, set()).add(targets[i])
for name, fs in sorted(res.items(), key=lambda x: min(x[1])):
    print('%x %s%s' % (min(fs), name, '' if len(fs) == 1 else '  (also %s)' % ' '.join('%x' % f for f in sorted(fs)[1:])))
