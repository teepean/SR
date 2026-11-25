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
        if t == 'src_scc':
            self.kind = 'scc'         # SCC as a 32-bit source (0 / 1)
            return
        if re.match(r'^(prim|pos\d|param\d+|mrt\d|mrtz)$', t):
            self.kind = 'target'      # export target of exp (self.text: prim, pos0..3, param0..31, mrt0..7, mrtz)
            return
        m = re.match(r'^attr(\d+)\.([xyzw])$', t)
        if m:
            self.kind = 'attr'        # v_interp: attribute (self.attr) and component (self.chan 0..3)
            self.attr, self.chan = int(m.group(1)), 'xyzw'.index(m.group(2))
            return
        if t in ('p10', 'p20', 'p0'):
            self.kind = 'interp_param'   # v_interp_mov_f32: P10 / P20 / P0 (self.text)
            return
        if t.startswith(('sendmsg(', 'hwreg(')) and t.endswith(')'):
            self.kind = 'const'       # s_sendmsg / s_getreg / s_setreg immediates: kept as text (self.text)
            self.value = 0
            return
        if t == 'off':
            self.kind = 'off'
            return
        m = re.match(r'^\[(v\d+(?:\s*,\s*v\d+)*)\]$', t)
        if m:
            # NSA address list: registers in any order (kind 'vlist'; first/count describe the list, not a range)
            self.kind, self.file = 'vlist', 'v'
            self.list = [int(x.strip()[1:]) for x in m.group(1).split(',')]
            self.first, self.count = self.list[0], len(self.list)
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
        if self.kind == 'vlist':
            return ['v%d' % r for r in self.list]
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
            if a.startswith('[') and ']' in a:
                # an NSA address list "[v74, v77]" (MIMG with non-sequential addresses) is one operand
                close = a.index(']') + 1
                words = [a[:close]] + a[close:].split()
            else:
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


def llvm_mc(raw):
    """llvm-mc on bytes: (stdout lines with an encoding, byte offsets of the words it could not decode)"""
    text = ' '.join('0x%02x' % b for b in raw)
    r = subprocess.run(LLVM_MC, input=text, capture_output=True, text=True)
    lines = []
    for line in r.stdout.split('\n'):
        m = re.search(r'encoding: \[(.*?)\]', line)
        if m:
            lines.append((line.split(';')[0].strip(), len(m.group(1).split(','))))
    skipped = sorted((int(m.group(1)) - 1) // 5
                     for m in re.finditer(r'<stdin>:\d+:(\d+): warning: invalid instruction encoding', r.stderr))
    return lines, skipped


def fallback(raw, off):
    """an instruction llvm-mc (gfx1013) does not decode: (text, size). The hint s_inst_prefetch; MUBUF with dlc on an
    atomic (decoded without dlc); SDWA with neg/abs on an integer move (decoded without, the modifiers put back);
    anything else is a visible unknown of its format's size"""
    w = struct.unpack_from('<I', raw, off)[0]
    if (w >> 16) == 0xbfa0:
        return 's_inst_prefetch', 4
    size = min(base_size(raw, off), 8)
    if off + size > len(raw):
        return '.unknown_word 0x%08x' % w, 4
    words = list(struct.unpack_from('<%dI' % (size // 4), raw, off))
    if (w >> 26) == 0b111000 and size == 8 and (w >> 15) & 1:           # MUBUF: drop dlc
        words[0] &= ~(1 << 15)
        lines, bad = llvm_mc(struct.pack('<2I', *words))
        if lines and not bad:
            return lines[0][0], 8
    if (w >> 31) == 0 and (w & 0x1ff) == 0xf9 and size == 8:            # VOP1/2/C SDWA: modifiers put back
        sd = words[1]
        neg0, abs0 = (sd >> 20) & 1, (sd >> 21) & 1
        words[1] &= ~((1 << 20) | (1 << 21) | (1 << 28) | (1 << 29))
        lines, bad = llvm_mc(struct.pack('<2I', *words))
        if lines and not bad and not ((sd >> 28) & 3):
            t = lines[0][0]
            head, _, rest = t.partition(' ')
            parts = rest.split(', ', 2)
            if len(parts) >= 2:
                src = parts[1].split(' ')[0]
                mod = ('|%s|' % src) if abs0 else src
                mod = ('-' + mod) if neg0 else mod
                parts[1] = mod + parts[1][len(src):]
                return head + ' ' + ', '.join(parts), 8
    return '.unknown_word 0x%08x' % w, size


def decode(path):
    """the program's instructions up to its first s_endpgm, then any block a branch jumps to past it (pixel
    shaders put their kill-everything export after the main s_endpgm), until the next s_endpgm"""
    raw = open(path, 'rb').read()
    insts = decode_from(raw, 0)
    while insts:
        end = insts[-1].off + insts[-1].size
        targets = [i.target for i in insts if i.target is not None and i.target >= end and i.target < len(raw)]
        if not targets or not insts[-1].text.startswith('s_endpgm'):
            break
        nxt = min(targets)
        if nxt != end:
            break          # a gap between the blocks: not decoded (unknown bytes)
        more = decode_from(raw, end)
        if not more:
            break
        insts += more
    return insts


def decode_from(raw, off):
    insts = []
    while off < len(raw):
        lines, skipped = llvm_mc(raw[off:])
        stop = off + skipped[0] if skipped else None
        pos = off
        for t, size in lines:
            if stop is not None and pos >= stop:
                break
            insts.append(Inst(pos, t, size, raw))
            pos += size
            if t.startswith('s_endpgm'):
                return insts
        if stop is None or pos != stop:
            break
        t, size = fallback(raw, pos)
        insts.append(Inst(pos, t, size, raw))
        off = pos + size
    return insts
