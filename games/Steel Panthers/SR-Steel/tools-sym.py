#!/usr/bin/env python3
"""Map host addresses of the native build to the nearest preceding loc_/SR_ symbol.
usage: tools-sym.py syms.txt ADDR...   (syms.txt from: nm -n steelpanthers)"""
import bisect, sys
syms = []
for l in open(sys.argv[1]):
    a, n = l.split()
    syms.append((int(a, 16), n))
addrs = [a for a, _ in syms]
for x in sys.argv[2:]:
    v = int(x, 16)
    i = bisect.bisect_right(addrs, v) - 1
    print("%s -> %s+0x%x" % (x, syms[i][1], v - syms[i][0]) if i >= 0 else x)
