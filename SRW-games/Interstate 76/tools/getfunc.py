#!/usr/bin/env python3
# print decompiled functions by address: getfunc.py <decompiled.c> <hexaddr>...
import sys, re
text = open(sys.argv[1]).read()
parts = re.split(r'(?m)^(?=// ---- 0x)', text)
want = {int(a, 16) for a in sys.argv[2:]}
for p in parts:
    m = re.match(r'// ---- 0x([0-9a-f]+)', p)
    if m and int(m.group(1), 16) in want:
        print(p.rstrip() + '\n')
