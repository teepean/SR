#!/usr/bin/env python3
"""Shared helpers: direct call targets/callers in the code object (from work/obj1.bin)."""
import re, struct, bisect
BASE = 0x10000
code = open("work/obj1.bin", "rb").read()
data = open("work/obj2.bin", "rb").read()
callers = {}
for m in re.finditer(rb'\xe8', code):
    i = m.start()
    if i + 5 > len(code): continue
    t = BASE + i + 5 + struct.unpack_from('<i', code, i + 1)[0]
    if BASE <= t < BASE + len(code):
        callers.setdefault(t, []).append(BASE + i)
targets = sorted(callers)
def func_of(addr):
    k = bisect.bisect_right(targets, addr) - 1
    return targets[k] if k >= 0 else None
def strings_refs(pattern):
    out = {}
    for m in re.finditer(pattern, data):
        s = m.start(); st = data.rfind(b'\0', 0, s) + 1
        if st != s: continue
        pat = struct.pack('<I', 0xc0000 + s)
        for r in re.finditer(re.escape(pat), code):
            out.setdefault(m.group(1).decode(), []).append(BASE + r.start())
    return out
