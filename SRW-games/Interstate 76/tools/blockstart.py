# Heuristic: is address t the start of a code block (after ret/jmp/padding)?
# Linear-decodes with capstone from the closest preceding IDA code head, so IDA's own
# mistakes (code wrongly turned into data) don't hide a mid-instruction target.
import capstone, csv, bisect

TERM = ('ret', 'retn', 'retf', 'jmp', 'ljmp', 'int3', 'nop', 'hlt')

class BS:
    def __init__(self, exe, d, secs):
        self.b = open(exe, 'rb').read(); self.secs = secs
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        rows = [(int(r[0], 16), r[2]) for r in csv.reader(open(d + '/heads.csv'))]
        self.code = sorted(a for a, k in rows if k == 'c')
    def fo(self, a):
        for va, raw, sz in self.secs:
            if va <= a < va + sz: return a - va + raw
    def is_block_start(self, t):
        i = bisect.bisect_left(self.code, t)
        if i == 0: return True
        p = self.code[i - 1]
        if t - p > 0x40:           # long gap of non-code before target: treat as block start
            return True
        o = self.fo(p)
        prev = None
        for ins in self.md.disasm(self.b[o:o + (t - p) + 16], p):
            if ins.address == t:
                if prev is None: return True
                m = prev.mnemonic
                return m.startswith(TERM) or (m == 'lea' and prev.op_str in ('esi, [esi]', 'edi, [edi]'))
            if ins.address > t:
                return False      # target is in the middle of an instruction
            prev = ins
        return False
