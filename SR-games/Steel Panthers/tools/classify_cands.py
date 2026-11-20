#!/usr/bin/env python3
"""Classify SR's --list_invalid_code_fixups candidates as code or data (heuristic, for review)."""
import sys, json, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
md = Cs(CS_ARCH_X86, CS_MODE_32)
code = open("work/obj1.bin", "rb").read()
BASE = 0x10000
cands = sorted({int(l.strip()[4:], 16) for l in open(sys.argv[1]) if l.startswith("loc_")})
GOOD_FIRST = {"push", "mov", "sub", "xor", "cmp", "test", "call", "jmp", "lea", "movzx", "movsx", "inc", "dec",
              "add", "and", "or", "pushal", "cld", "fld", "fild", "enter", "ret", "pop", "shl", "shr", "sar", "imul", "neg", "not", "xchg", "cdq", "movsb", "lodsb", "stosb"}
BAD = {"arpl", "bound", "aaa", "aas", "daa", "das", "aam", "aad", "into", "hlt", "salc", "in", "out", "insb", "insd", "outsb", "outsd",
       "les", "lds", "lock", "sldt", "lar", "lsl", "retf", "iretd", "int1", "int3", "wait", "fwait"}
def classify(a):
    i = a - BASE
    blob = code[i:i + 64]
    printable = sum(1 for b in blob[:16] if 32 <= b < 127)
    insns = list(md.disasm(blob, a))
    if not insns: return "data", "undecodable"
    if insns[0].mnemonic not in GOOD_FIRST: return "data", "first=" + insns[0].mnemonic
    n = 0; covered = 0; last = None
    for ins in insns:
        if ins.mnemonic in BAD: return "data", "bad " + ins.mnemonic
        n += 1; covered += ins.size; last = ins
        if ins.mnemonic in ("ret", "jmp") or covered > 40: break
    if covered <= 40 and last.mnemonic not in ("ret", "jmp"): return "data", "decode stops"
    if printable >= 12: return "data", "ascii"
    # watcom prologue: push regs then mov ebp,esp / sub esp
    txt = " ; ".join("%s %s" % (x.mnemonic, x.op_str) for x in insns[:6])
    if insns[0].mnemonic == "push" and any(x.mnemonic == "mov" and x.op_str == "ebp, esp" for x in insns[:7]): return "code", txt
    if insns[0].mnemonic == "push" and insns[0].op_str in ("ebx", "ecx", "edx", "esi", "edi", "ebp"): return "code?", txt
    return "unsure", txt
res = collections.defaultdict(list)
for a in cands:
    k, why = classify(a)
    res[k].append((a, why))
for k in ("code", "code?", "unsure", "data"):
    print("== %s: %d" % (k, len(res[k])))
json.dump({k: [[a, w] for a, w in v] for k, v in res.items()}, open("work/cand_class.json", "w"), indent=0)
