#!/usr/bin/env python3
# gcnlits.py <code.bin> <out.txt>: every instruction (gfx1013) whose encoding carries a 32-bit literal, with the full
# literal (llvm-mc prints v_fma_mix literals truncated to 16 bits)
import re, struct, subprocess, sys
data = open(sys.argv[1], 'rb').read()
text = ' '.join('0x%02x' % b for b in data)
out = subprocess.run(['llvm-mc', '-arch=amdgcn', '-mcpu=gfx1013', '-disassemble', '-show-encoding'], input=text,
                     capture_output=True, text=True).stdout
off = 0; lines = []
for line in out.split('\n'):
    m = re.search(r'encoding: \[(.*?)\]', line)
    if not m: continue
    enc = [int(x, 16) for x in m.group(1).split(',')]
    ins = line.split(';')[0].strip()
    # literal = last dword when the instruction is 8 (VOP1/2/C + literal) or 12 bytes (VOP3/VOP3P + literal) and an
    # operand field is 255; report the last dword for those encodings
    if len(enc) in (8, 12):
        lit = struct.unpack_from('<I', data, off + len(enc) - 4)[0]   # raw bytes (llvm-mc re-encodes)
        if '0x' in ins or len(enc) == 12:
            f = struct.unpack('<f', struct.pack('<I', lit))[0]
            lines.append('%05x  %-70s literal 0x%08x = %g' % (off, ins, lit, f))
    off += len(enc)
    if ins.startswith('s_endpgm'): break
open(sys.argv[2], 'w').write('\n'.join(lines) + '\n')
print(len(lines), 'instructions with literals;', sum(1 for l in lines if 'fma_mix' in l), 'fma_mix')
