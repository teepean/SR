#!/usr/bin/env python3
# Merge SRW --list_invalid_code_fixups candidates into fixup_interpret_as_code.sci,
# keeping only candidates that IDA analysed as code heads (drops jump tables etc.).
# usage: gen_code_fixups.py <ida_out_dir> <candidates.txt> <fixup_interpret_as_code.sci>
import sys, csv, os
d, cand, out = sys.argv[1:4]
heads = {int(r[0], 16): r[2] for r in csv.reader(open(d + '/heads.csv'))}
cur = set()
if os.path.exists(out):
    cur = {l.strip() for l in open(out) if l.strip()}
new = set(); skipped = []
for l in open(cand):
    l = l.strip()
    if not l: continue
    a = int(l[4:], 16)
    if heads.get(a) == 'c': new.add('loc_%X' % a)
    else: skipped.append(l)
added = new - cur
with open(out, 'w') as f:
    for x in sorted(cur | new, key=lambda s: int(s[4:], 16)): f.write(x + '\n')
print("added", len(added), "total", len(cur | new), "skipped(non-code)", len(set(skipped)))
