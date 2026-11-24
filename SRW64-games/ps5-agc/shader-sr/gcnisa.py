# gcnisa.py - decoding of PS5 GPU (gfx1013, wave64) shader code with llvm-mc into instruction objects
#
# llvm-mc must decode as gfx1013 with wavefrontsize64: gfx1030 rejects v_mad_f32/v_mac_f32 and mis-decodes the next
# dword; wave32 mode prints lane masks as 32-bit registers. Literals are taken from the raw bytes (llvm-mc prints
# v_fma_mix literals truncated to 16 bits).

import re
import struct
import subprocess

LLVM_MC = ['llvm-mc', '-arch=amdgcn', '-mcpu=gfx1013', '-mattr=+wavefrontsize64', '-disassemble', '-show-encoding']

SPECIAL_REGS = {
    'vcc': ('s', 106, 2), 'vcc_lo': ('s', 106, 1), 'vcc_hi': ('s', 107, 1),
    'exec': ('s', 126, 2), 'exec_lo': ('s', 126, 1), 'exec_hi': ('s', 127, 1),
    'm0': ('s', 124, 1), 'null': ('null', 0, 1),
}

INLINE_FLOATS = {'0.5': 0x3f000000, '-0.5': 0xbf000000, '1.0': 0x3f800000, '-1.0': 0xbf800000, '2.0': 0x40000000,
                 '-2.0': 0xc0000000, '4.0': 0x40800000, '-4.0': 0xc0800000, '0.15915494': 0x3e22f983}


class Operand:
    """one source or destination: register range ('s'/'v', first, count), constant (value, is_float_literal) or
    'off'/'null'; modifiers neg/abs/sext"""

    def __init__(self, text, raw_literal=None):
        self.text = text
        self.kind = None          # 'reg', 'const', 'off', 'null'
        self.file = None          # 's' or 'v'
        self.first = 0
        self.count = 1
        self.value = 0            # constant bits (32-bit)
        self.neg = False
        self.abs = False
        self.sext = False
        t = text.strip()
        if t.startswith('neg(') and t.endswith(')'):
            self.neg = True
            t = t[4:-1]
        if t.startswith('-') and not re.match(r'^-\d', t) and not re.match(r'^-0x', t) and t not in INLINE_FLOATS:
            self.neg = True
            t = t[1:]
        if t.startswith('|') and t.endswith('|'):
            self.abs = True
            t = t[1:-1]
        if t.startswith('-|') and t.endswith('|'):
            self.neg = self.abs = True
            t = t[2:-1]
        if t.startswith('sext(') and t.endswith(')'):
            self.sext = True
            t = t[5:-1]
        if t == 'off':
            self.kind = 'off'
            return
        if t in SPECIAL_REGS:
            f, n, c = SPECIAL_REGS[t]
            if f == 'null':
                self.kind = 'null'
                return
            self.kind, self.file, self.first, self.count = 'reg', f, n, c
            return
        m = re.match(r'^([sv])\[(\d+):(\d+)\]$', t)
        if m:
            self.kind, self.file = 'reg', m.group(1)
            self.first = int(m.group(2))
            self.count = int(m.group(3)) - self.first + 1
            return
        m = re.match(r'^([sv])(\d+)$', t)
        if m:
            self.kind, self.file, self.first = 'reg', m.group(1), int(m.group(2))
            return
        self.kind = 'const'
        if t in INLINE_FLOATS:
            self.value = INLINE_FLOATS[t]
            return
        if re.match(r'^-?0x[0-9a-fA-F]+$', t) or re.match(r'^-?\d+$', t):
            v = int(t, 0)
            self.value = v & 0xffffffff
            self.literal = raw_literal is not None and (v > 64 or v < -16)
            if raw_literal is not None and (v > 64 or v < -16):
                self.value = raw_literal
            return
        raise ValueError('operand %r' % text)

    def regs(self):
        if self.kind != 'reg':
            return []
        return ['%s%d' % (self.file, self.first + i) for i in range(self.count)]

    def __repr__(self):
        return self.text


class Inst:
    def __init__(self, off, text, size, raw):
        self.off, self.text, self.size = off, text, size
        head, _, rest = text.partition(' ')
        self.op = head
        self.mods = {}            # name -> value (offset:16, op_sel:[..], row_shr:1, glc, idxen, ...)
        self.ops = []
        literal = struct.unpack_from('<I', raw, off + size - 4)[0] if size > base_size(raw, off) else None
        args = split_args(rest)
        for a in args:
            for piece in a.split():
                pass
        positional = []
        for a in args:
            words = a.split()
            if not words:
                continue
            first, extra = words[0], words[1:]
            if is_modifier(first):
                extra = [first] + extra
            else:
                positional.append(first)
            for w in extra:
                if ':' in w:
                    k, v = w.split(':', 1)
                    self.mods[k] = v
                else:
                    self.mods[w] = True
        if head in NO_OPERANDS or head == 's_branch' or head.startswith('s_cbranch_'):
            self.ops = []
        else:
            self.ops = [Operand(p, literal) for p in positional]
        self.target = None
        if self.op == 's_branch' or self.op.startswith('s_cbranch_'):
            rel = int(positional[0], 0)
            rel = rel - 65536 if rel > 32767 else rel
            self.target = off + 4 + rel * 4
        self.base = re.sub(r'_(e32|e64|sdwa|dpp)$', '', self.op)
        self.encoding = re.search(r'_(e32|e64|sdwa|dpp)$', self.op)
        self.encoding = self.encoding.group(1) if self.encoding else ''

    def mod_list(self, name, n=3):
        v = self.mods.get(name)
        if v is None:
            return [0] * n
        return [int(x) for x in v.strip('[]').split(',')]

    def __repr__(self):
        return '%05x %s' % (self.off, self.text)


def base_size(raw, off):
    """size of the instruction's encoding without a literal constant (gfx10): a trailing literal exists only when the
    instruction is longer. SMEM, MUBUF, DS, FLAT, VOP3 are 8 bytes (an SMEM offset > 64 is not a literal); VOP1/VOP2/
    VOPC take a second dword for SDWA (src0 0xf9) and DPP16/DPP8 (0xfa/0xe9/0xea)"""
    w = struct.unpack_from('<I', raw, off)[0]
    top6 = w >> 26
    if (w >> 30) == 0b10:                       # SOP1/SOP2/SOPC/SOPP/SOPK
        return 4
    if top6 in (0b111101, 0b110101, 0b110011, 0b110110, 0b111000, 0b111010, 0b110111):
        return 8                                # SMEM, VOP3, VOP3P, DS, MUBUF, MTBUF, FLAT/GLOBAL/SCRATCH
    if top6 == 0b111100:                        # MIMG (NSA adds address dwords; no literals)
        return 64
    if top6 == 0b110010:                        # VINTRP
        return 4
    if (w >> 31) == 0:                          # VOP1/VOP2/VOPC
        src0 = w & 0x1ff
        return 8 if src0 in (0xf9, 0xfa, 0xe9, 0xea) else 4
    return 4


NO_OPERANDS = ('s_waitcnt', 's_inst_prefetch', 's_nop', 's_endpgm', 's_barrier', 's_code_end', 's_sleep',
               's_waitcnt_vscnt', 's_waitcnt_vmcnt', 's_waitcnt_lgkmcnt', 's_dcache_inv', 's_icache_inv')


def is_modifier(word):
    return (':' in word and not word.startswith(('s[', 'v[')) and not re.match(r'^[sv]\[', word)) or word in (
        'glc', 'slc', 'dlc', 'idxen', 'offen', 'clamp', 'gds', 'lds', 'tfe', 'bound_ctrl:0', 'bound_ctrl:1',
        'row_mirror', 'row_half_mirror')


def split_args(s):
    args, depth, cur = [], 0, ''
    for ch in s:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    # "v0, v1 offset:16 glc": the last positional argument carries the space separated modifiers
    return args


def decode(path):
    raw = open(path, 'rb').read()
    text = ' '.join('0x%02x' % b for b in raw)
    out = subprocess.run(LLVM_MC, input=text, capture_output=True, text=True).stdout
    insts, off = [], 0
    for line in out.split('\n'):
        m = re.search(r'encoding: \[(.*?)\]', line)
        if not m:
            continue
        size = len(m.group(1).split(','))
        t = line.split(';')[0].strip()
        insts.append(Inst(off, t, size, raw))
        off += size
        if t.startswith('s_endpgm'):
            break
    return insts
