#!/usr/bin/env python3
# Address maps for profiling an SRW64 build of a converted ELF (numpy):
#   func_entries.npy  original function entries: direct call targets (called twice or more, or with a prologue) and
#                     'push rbp; mov rbp, rsp' prologues of the input executable's code segment
#   exe_labels.txt    "recompiled address  original label" for every loc_<orig> symbol of the SRW64 build (nm -n)
# usage: funcmap.py <input ELF> <SRW64 executable> <output dir>
import os, struct, subprocess, sys
import numpy as np

elf, exe, out = sys.argv[1:4]
d = open(elf, 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20)
phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
entries = set()
for i in range(phnum):
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
    if p_type != 1 or not (p_flags & 1):
        continue
    seg = np.frombuffer(d[p_offset:p_offset + p_filesz], dtype=np.uint8)
    idx = np.nonzero(seg[:-5] == 0xe8)[0]
    rel = (seg[idx + 1].astype(np.int64) | (seg[idx + 2].astype(np.int64) << 8) | (seg[idx + 3].astype(np.int64) << 16)
           | (seg[idx + 4].astype(np.int64) << 24))
    rel = np.where(rel >= 2**31, rel - 2**32, rel)
    tgt = idx + p_vaddr + 5 + rel
    tgt = tgt[(tgt >= p_vaddr) & (tgt < p_vaddr + p_filesz)]
    calls, counts = np.unique(tgt, return_counts=True)
    pro = np.nonzero((seg[:-4] == 0x55) & (seg[1:-3] == 0x48) & (seg[2:-2] == 0x89) & (seg[3:-1] == 0xe5))[0] + p_vaddr
    pro_set = set(pro.tolist())
    entries |= pro_set | {int(c) for c, n in zip(calls, counts) if n >= 2 or int(c) in pro_set}
os.makedirs(out, exist_ok=True)
np.save(os.path.join(out, 'func_entries.npy'), np.array(sorted(entries), dtype=np.int64))
with open(os.path.join(out, 'exe_labels.txt'), 'w') as f:
    for line in subprocess.run(['nm', '-n', exe], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[2].startswith('loc_'):
            f.write('%s %s\n' % (p[0], p[2][4:]))
print('%d function entries' % len(entries))
