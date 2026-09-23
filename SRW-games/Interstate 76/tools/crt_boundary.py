#!/usr/bin/env python3
# Find statically linked CRT (IDA library, FUNC_LIB=0x4) functions called directly from non-library code.
# usage: crt_boundary.py <ida_out_dir>
import sys, csv
d = sys.argv[1]
funcs = {int(r[0], 16): (r[2], int(r[3], 16)) for r in csv.reader(open(d + '/funcs.csv'))}
xr = {int(r[0], 16): r[3].split(';') if r[3] else [] for r in csv.reader(open(d + '/func_xrefs.csv'))}
def islib(a): return a in funcs and (funcs[a][1] & 4)
out = []
for a, (n, fl) in sorted(funcs.items()):
    if not (fl & 4): continue
    callers = [c for c in xr.get(a, []) if not c.startswith('d')]
    datarefs = [c for c in xr.get(a, []) if c.startswith('d')]
    game = [c for c in callers if not islib(int(c, 16))]
    if game or datarefs:
        out.append((a, n, len(game), len(datarefs)))
for a, n, g, dr in out: print("0x%x,%s,game_callers=%d,datarefs=%d" % (a, n, g, dr))
