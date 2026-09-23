#!/usr/bin/env python3
# List IDA functions that SRW did not emit as code.
# usage: check_coverage.py <ida_out_dir> <seg01.inc>
import sys, csv, re
funcs = [(int(r[0], 16), int(r[1], 16), r[2]) for r in csv.reader(open(sys.argv[1] + '/funcs.csv'))]
lines = open(sys.argv[2]).read().split('\n')
kind = {}
for i, l in enumerate(lines):
    m = re.match(r'^loc_([0-9A-F]+):$', l)
    if m:
        j = i + 1
        while j < len(lines) and re.match(r'^[A-Za-z_@?$][\w@?$]*:$', lines[j]): j += 1
        kind[int(m.group(1), 16)] = 'data' if re.match(r'^(db|dd|dw|times|align)', lines[j]) else 'code'
miss = [f for f in funcs if kind.get(f[0]) != 'code']
print(len(funcs), "IDA funcs;", len(miss), "not emitted as code (or no loc_ label, e.g. exported name)")
for f in miss: print(hex(f[0]), hex(f[1] - f[0]), f[2], kind.get(f[0], 'nolabel'))
