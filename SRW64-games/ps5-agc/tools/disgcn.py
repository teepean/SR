#! /usr/bin/env python3
# disgcn.py <code.bin> [max bytes] - PS5 GPU (gfx1013) disassembly of a dumped shader up to the first s_endpgm
import subprocess, sys
data = open(sys.argv[1], 'rb').read()
limit = int(sys.argv[2], 0) if len(sys.argv) > 2 else len(data)
data = data[:limit]
text = ' '.join('0x%02x' % b for b in data)
out = subprocess.run(['llvm-mc', '-arch=amdgcn', '-mcpu=gfx1013', '-disassemble', '-show-encoding'], input=text,
                     capture_output=True, text=True).stdout
lines = []
for line in out.split('\n'):
    line = line.split(';')[0].rstrip()
    if not line.strip() or line.strip().startswith('.'): continue
    lines.append(line.strip())
    if line.strip().startswith('s_endpgm'): break
print('\n'.join(lines))
print('; %d instructions' % len(lines), file=sys.stderr)
