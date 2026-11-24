#!/usr/bin/env python3
# gcn2glsl.py <code.bin> <out.comp> [--threads N] [--lds-bytes N] [--user-sgprs N] [--tgid-sgpr N]
#
# Static recompilation of a PS5 GPU (gfx1013, wave64) compute shader into a GLSL compute shader for NVIDIA GPUs:
# one guest lane = one thread (see DESIGN.md). This first version covers the instruction set of the cloth solver
# shaders; unknown instructions are reported.
#
# Execution model of the generated code
# - VGPRs: uint V[n] per thread; SGPRs: uint S[128] per thread (wave-uniform values computed redundantly).
# - exec: the thread's own lane bit (bool EX). SGPR pairs holding lane masks keep the thread's bit in the low
#   register (0/1) and 0 in the high one; a mask read as data needs the wave's ballot (reported).
# - VALU instructions are predicated by EX (if (EX) ...), which NVIDIA compiles to predicated instructions.
# - Control flow: forward branches set a per-thread flag that guards the instructions up to the target; backward
#   branches are loops. Branches on exec/vcc decide per thread (the thread's own lane), except where the analysis
#   marks them wave-coupled (then the wave's decision is computed with a ballot over the 64 threads of the wave).
# - Memory: guest addresses are translated through a range table (push constant) to buffer device addresses.

import argparse
import collections
import re
import struct
import sys

import gcnisa

WAVE = 64


def f32const(bits):
    return 'uintBitsToFloat(0x%08xu)' % (bits & 0xffffffff)


class Translator:
    def __init__(self, insts, threads, lds_bytes, user_sgprs, tgid_sgpr):
        self.insts = insts
        self.threads = threads
        self.lds_dwords = max(1, (lds_bytes + 3) // 4)
        self.user_sgprs = user_sgprs
        self.readfirst_offs = set()
        self.tgid_sgpr = tgid_sgpr
        self.unknown = collections.Counter()
        self.notes = []
        self.mask_kind = {}       # SGPR index (even) -> 'mask' / 'value', last definition in program order
        self.max_v = 0
        self.lines = []
        self.wave_branches = set()   # offsets of branches that need the wave's decision
        self.used_s, self.used_v = set(range(16)), {0}
        self.ballot_uses = set()     # offsets of instructions reading a lane mask as data
        # lane pairs: a host thread runs guest lanes j (half 0) and j + 32 (half 1); per-lane instructions are
        # translated once per half (h = 0/1: VGPR v<n>_<h>, exec bit EX -> EXB<h>, lane bit B = 1 << h), scalar ones
        # once (h = None: a lane mask is the thread's two bits, exec is EXM)
        self.h = None

    # ------------------------------------------------------------------------------------------------ operands
    def note_reg(self, o):
        if o.kind == 'reg' and o.file == 'v':
            self.max_v = max(self.max_v, o.first + o.count)

    def reg(self, o, i=0):
        self.note_reg(o)
        n = o.first + i
        if o.file == 's':
            self.used_s.add(n)
            return 's%d' % n
        if self.h is None:
            raise ValueError('VGPR in a scalar instruction: %r' % o)
        self.used_v.add(n)
        return 'v%d_%d' % (n, self.h)

    def vname(self, o, i, h):
        """a VGPR of either half (for the scalar instructions that read lanes: readlane, readfirstlane)"""
        self.note_reg(o)
        self.used_v.add(o.first + i)
        return 'v%d_%d' % (o.first + i, h)

    def lane_bit_const(self):
        return '%du' % (1 << self.h)

    def src_u32(self, o, i=0):
        """32-bit source bits as uint (no modifiers)"""
        if o.kind == 'const':
            return '0x%08xu' % o.value
        if o.kind == 'reg':
            if o.file == 's' and o.first + i in (126, 127):
                if self.h is None:
                    return 'EXM' if o.first + i == 126 else '0u'
                return '(EX ? 1u : 0u)' if o.first + i == 126 else '0u'
            return self.reg(o, i)
        if o.kind in ('null', 'off'):
            return '0u'
        raise ValueError(o)

    def src_f32(self, o, inst, idx=None):
        """32-bit float source with neg/abs and SDWA select"""
        bits = self.src_u32(o)
        sel = None
        if inst.encoding == 'sdwa' and idx is not None:
            sel = inst.mods.get('src%d_sel' % idx)
        if sel and sel != 'DWORD':
            bits = self.sdwa_sel(bits, sel, o.sext)
        e = 'uintBitsToFloat(%s)' % bits
        if o.kind == 'const' and o.value in (0, 1, 2, 3, 4) and not o.text.startswith('0x'):
            # integer inline constants in float operations are integers, as float: 0, 1, 2 ... -> 0.0? no: hardware
            # uses the integer bit pattern for ints, but for f32 operands inline ints mean the float value
            e = '%d.0' % o.value
        elif o.kind == 'const' and (o.value >= 0xfffffff0) and not o.text.startswith('0x'):
            e = '%d.0' % (o.value - 0x100000000)
        if o.abs:
            e = 'abs(%s)' % e
        if o.neg:
            e = '(-%s)' % e
        return e

    def sdwa_sel(self, bits, sel, sext):
        if sel == 'DWORD':
            return bits
        m = {'BYTE_0': (0, 8), 'BYTE_1': (8, 8), 'BYTE_2': (16, 8), 'BYTE_3': (24, 8), 'WORD_0': (0, 16),
             'WORD_1': (16, 16)}[sel]
        if sext:
            return 'uint(bitfieldExtract(int(%s), %d, %d))' % (bits, m[0], m[1])
        return 'bitfieldExtract(%s, %d, %d)' % (bits, m[0], m[1])

    def src_int(self, o, inst, idx=None):
        bits = self.src_u32(o)
        if inst.encoding == 'sdwa' and idx is not None:
            sel = inst.mods.get('src%d_sel' % idx, 'DWORD')
            bits = self.sdwa_sel(bits, sel, o.sext)
        elif o.sext:
            bits = 'uint(int(%s))' % bits
        if o.neg:
            bits = '(-%s)' % bits
        return bits

    def write_v_f32(self, inst, dst, expr):
        """write a float result with clamp / omod and SDWA dst_sel"""
        if 'mul' in inst.mods:
            expr = '(%s * %s.0)' % (expr, inst.mods['mul'])
        if 'div' in inst.mods:
            expr = '(%s * 0.5)' % expr if inst.mods['div'] == '2' else '(%s * 0.25)' % expr
        if inst.mods.get('clamp'):
            expr = 'clamp(%s, 0.0, 1.0)' % expr
        self.write_v_u32(inst, dst, 'floatBitsToUint(%s)' % expr)

    def write_v_u32(self, inst, dst, expr):
        target = self.reg(dst)
        sel = inst.mods.get('dst_sel', 'DWORD') if inst.encoding == 'sdwa' else 'DWORD'
        if sel != 'DWORD':
            m = {'BYTE_0': (0, 8), 'BYTE_1': (8, 8), 'BYTE_2': (16, 8), 'BYTE_3': (24, 8), 'WORD_0': (0, 16),
                 'WORD_1': (16, 16)}[sel]
            unused = inst.mods.get('dst_unused', 'UNUSED_PAD')
            if unused == 'UNUSED_PRESERVE':
                expr = 'bitfieldInsert(%s, %s, %d, %d)' % (target, expr, m[0], m[1])
            elif unused == 'UNUSED_SEXT':
                expr = 'uint(bitfieldExtract(int(%s) << %d, 0, 32) >> %d)' % (expr, 32 - m[0] - m[1], 32 - m[1])
            else:
                expr = '((%s & 0x%xu) << %d)' % (expr, (1 << m[1]) - 1, m[0])
        self.emit('%s = %s;' % (target, expr), valu=True)

    def emit(self, text, valu=False):
        self.cur.append((text, valu, None))

    # ------------------------------------------------------------------------------------------------ f16 helpers
    def f16_operand(self, o, half, inst):
        """operand as f16 -> float (half 0 = low, 1 = high), with neg/abs"""
        if o.kind == 'const':
            # inline float constants given as f32 text; 16-bit literal / inline: use the f16 value of the text
            if o.text in gcnisa.INLINE_FLOATS:
                e = repr(struct.unpack('<f', struct.pack('<I', gcnisa.INLINE_FLOATS[o.text]))[0])
            elif o.value < 65 and not o.text.startswith('0x'):
                e = '%d.0' % o.value
            else:
                e = 'unpackHalf2x16(0x%04xu).x' % (o.value & 0xffff)
        else:
            e = 'unpackHalf2x16(%s).%s' % (self.src_u32(o), 'y' if half else 'x')
        if o.abs:
            e = 'abs(%s)' % e
        if o.neg:
            e = '(-%s)' % e
        return e

    # ------------------------------------------------------------------------------------------------ instructions
    LANE_PREFIXES = ('v_', 'buffer_', 'tbuffer_', 'global_', 'flat_', 'scratch_', 'ds_', 'image_')
    CROSS_SCALAR = ('v_readlane_b32', 'v_readfirstlane_b32', 'v_writelane_b32')

    def translate(self, inst):
        if inst.op.startswith(self.LANE_PREFIXES) and inst.base not in self.CROSS_SCALAR:
            halves = []
            for h in (0, 1):
                self.h = h
                saved, self.cur = self.cur, []
                self.translate_one(inst)
                halves.append(self.cur)
                self.cur = saved
            self.h = None
            for h in (0, 1):
                for item in halves[h]:
                    self.cur.append((item[0], item[1], h))
            return
        self.h = None
        self.translate_one(inst)

    def translate_one(self, inst):
        op, b, ops = inst.op, inst.base, inst.ops
        for o in ops:
            self.note_reg(o)
        handler = getattr(self, 'op_' + b, None)
        if handler is not None:
            handler(inst)
            return
        for prefix, h in (('v_cmpx_', self.op_vcmpx), ('v_cmp_', self.op_vcmp), ('s_cmp_', self.op_scmp),
                          ('s_load_', self.op_sload), ('s_buffer_load_', self.op_sbufload),
                          ('buffer_load_', self.op_bufload), ('buffer_store_', self.op_bufstore),
                          ('global_load_', self.op_globalload), ('ds_', self.op_ds)):
            if b.startswith(prefix):
                h(inst)
                return
        f = FLOAT2.get(b)
        if f:
            d, a, c = ops[0], ops[1], ops[2]
            self.write_v_f32(inst, d, f % (self.src_f32(a, inst, 0), self.src_f32(c, inst, 1)))
            return
        f = FLOAT1.get(b)
        if f:
            d, a = ops[0], ops[1]
            self.write_v_f32(inst, d, f % self.src_f32(a, inst, 0))
            return
        f = INT2.get(b)
        if f:
            d, a, c = ops[0], ops[1], ops[2]
            self.write_v_u32(inst, d, f % (self.src_int(a, inst, 0), self.src_int(c, inst, 1)))
            return
        f = INT3.get(b)
        if f:
            d, a, c, e = ops[0], ops[1], ops[2], ops[3]
            self.write_v_u32(inst, d, f % (self.src_int(a, inst, 0), self.src_int(c, inst, 1), self.src_int(e, inst, 2)))
            return
        if b in ('s_waitcnt', 's_nop', 's_inst_prefetch', 's_barrier'):   # s_barrier: barrier() in the emission
            return
        self.unknown[op] += 1
        self.emit('/* UNHANDLED %s */' % inst.text)

    # --- VALU float
    def op_v_mac_f32(self, inst):
        d, a, c = inst.ops
        self.write_v_f32(inst, d, 'fma_unfused(%s, %s, uintBitsToFloat(%s))' % (
            self.src_f32(a, inst, 0), self.src_f32(c, inst, 1), self.reg(d)))

    def op_v_mad_f32(self, inst):
        d, a, c, e = inst.ops
        self.write_v_f32(inst, d, 'fma_unfused(%s, %s, %s)' % (self.src_f32(a, inst), self.src_f32(c, inst),
                                                               self.src_f32(e, inst)))

    def op_v_madmk_f32(self, inst):
        d, a, k, c = inst.ops
        self.write_v_f32(inst, d, 'fma_unfused(%s, %s, %s)' % (self.src_f32(a, inst), f32const(k.value),
                                                               self.src_f32(c, inst)))

    def op_v_med3_f32(self, inst):
        d, a, c, e = inst.ops
        self.write_v_f32(inst, d, 'med3(%s, %s, %s)' % (self.src_f32(a, inst), self.src_f32(c, inst),
                                                        self.src_f32(e, inst)))

    def op_v_fma_mix_f32(self, inst):
        d, a, c, e = inst.ops
        op_sel, op_sel_hi = inst.mod_list('op_sel'), inst.mod_list('op_sel_hi')
        srcs = []
        for k, o in enumerate((a, c, e)):
            if op_sel_hi[k]:
                srcs.append(self.f16_operand(o, op_sel[k], inst))
            else:
                srcs.append(self.src_f32(o, inst))
        self.write_v_f32(inst, d, 'fma(%s, %s, %s)' % tuple(srcs))

    def op_v_cvt_f32_f16(self, inst):
        d, a = inst.ops
        sel = inst.mods.get('src0_sel', 'DWORD') if inst.encoding == 'sdwa' else 'DWORD'
        half = 'y' if sel == 'WORD_1' else 'x'
        e = 'unpackHalf2x16(%s).%s' % (self.src_u32(a), half)
        if a.abs:
            e = 'abs(%s)' % e
        if a.neg:
            e = '(-%s)' % e
        self.write_v_f32(inst, d, e)

    def op_v_cvt_f32_i32(self, inst):
        d, a = inst.ops
        self.write_v_f32(inst, d, 'float(int(%s))' % self.src_int(a, inst, 0))

    def op_v_cvt_f32_ubyte0(self, inst):
        d, a = inst.ops
        self.write_v_f32(inst, d, 'float(%s & 0xffu)' % self.src_u32(a))

    def op_v_cvt_u32_f32(self, inst):
        d, a = inst.ops
        self.write_v_u32(inst, d, 'cvt_u32_f32(%s)' % self.src_f32(a, inst, 0))

    def op_v_cmp_class_f32(self, inst):
        # VOPC with sdst (sdwa/e64) or vcc
        d, a, m = inst.ops if len(inst.ops) == 3 else (None,) + tuple(inst.ops)
        cond = 'cmp_class(%s, %s)' % (self.src_f32(a, inst, 0), self.src_int(m, inst, 1))
        self.write_mask(inst, d, cond)

    # --- VALU integer / moves
    def op_v_mov_b32(self, inst):
        d, a = inst.ops
        if a.kind == 'reg' and a.file == 's' and a.first in (106, 107) and self.mask_kind.get(106) == 'mask':
            self.ballot_uses.add(inst.off)
            # the wave's ballot of the lane mask: all threads of the wave take part (outside the exec guard)
            self.emit('{ uvec2 bal_ = wave_ballot2((s106 & 1u) != 0u, (s106 & 2u) != 0u); if (EX) %s = bal_.%s; }' % (
                self.reg(d), 'x' if a.first == 106 else 'y'))
            return
        self.write_v_u32(inst, d, self.src_int(a, inst, 0))

    def op_v_cndmask_b32(self, inst):
        d, a, c, m = inst.ops
        self.write_v_u32(inst, d, '(%s ? %s : %s)' % (self.mask_bit(m), self.src_int(c, inst, 1),
                                                      self.src_int(a, inst, 0)))

    def op_v_add_co_u32(self, inst):
        d, cy, a, c = inst.ops
        self.emit('{ uint lo_; uint cy_ = 0u; lo_ = uaddCarry(%s, %s, cy_); if (EX) %s = lo_; %s }' % (
            self.src_int(a, inst, 0), self.src_int(c, inst, 1), self.reg(d), self.mask_store(cy, 'EX && cy_ != 0u')))

    def op_v_add_co_ci_u32(self, inst):
        d, cy, a, c, ci = inst.ops
        self.emit('{ uint c1_ = 0u, c2_ = 0u; uint t_ = uaddCarry(%s, %s, c1_); uint r_ = uaddCarry(t_, %s ? 1u : 0u, '
                  'c2_); if (EX) %s = r_; %s }' % (self.src_int(a, inst, 0), self.src_int(c, inst, 1),
                                                   self.mask_bit(ci), self.reg(d),
                                                   self.mask_store(cy, 'EX && (c1_ | c2_) != 0u')))

    def op_v_lshlrev_b64(self, inst):
        d, sh, a = inst.ops
        self.emit('{ uint64_t t_ = packUint2x32(uvec2(%s, %s)) << (%s & 63u); if (EX) { %s = uint(t_); %s = uint(t_ >> 32); } }'
                  % (self.src_u32(a, 0), self.src_u32(a, 1) if a.kind == 'reg' else '0u', self.src_u32(sh),
                     self.reg(d, 0), self.reg(d, 1)))

    def op_v_mul_hi_u32(self, inst):
        d, a, c = inst.ops
        self.write_v_u32(inst, d, 'mulhi_u32(%s, %s)' % (self.src_int(a, inst, 0), self.src_int(c, inst, 1)))

    def op_v_readfirstlane_b32(self, inst):
        d, a = inst.ops
        # the value of the first active lane: the thread's own value where the whole wave is known to be active (the
        # compiler reads wave-uniform values here), else through the wave (translate_shader resolves "readfirst(")
        self.readfirst_offs.add(inst.off)
        if a.kind == 'reg' and a.file == 'v':
            self.emit('%s = readfirst(%s, %s);' % (self.reg(d), self.vname(a, 0, 0), self.vname(a, 0, 1)))
        else:
            self.emit('%s = %s;' % (self.reg(d), self.src_u32(a)))
        self.set_kind(d, 'value')

    def op_v_readlane_b32(self, inst):
        d, a, lane = inst.ops
        self.emit('%s = wave_readlane2(%s, %s, %s);' % (self.reg(d), self.vname(a, 0, 0), self.vname(a, 0, 1),
                                                       self.src_u32(lane)))
        self.wave_points.add(inst.off)
        self.set_kind(d, 'value')

    def op_v_permlanex16_b32(self, inst):
        d, a, s1, s2 = inst.ops
        if not (s1.kind == 'const' and s2.kind == 'const' and s1.value == 0xffffffff and s2.value == 0xffffffff):
            self.unknown['v_permlanex16 with lane selects'] += 1
        # lane i reads lane (i ^ 16) of the other row within the same 32 lanes (selects -1: reverse order?)
        # with selects 0xffffffff every lane picks lane 15 of the other row -> row broadcast of lane 15/31
        self.write_v_u32(inst, d, 'permlanex16_ff(%s)' % self.src_u32(a))
        self.wave_points.add(inst.off)

    def op_dpp(self, inst, fn):
        d, a, c = inst.ops
        shr = int(inst.mods.get('row_shr', '0'))
        if not shr or inst.mods.get('row_mask', '0xf') != '0xf' or inst.mods.get('bank_mask', '0xf') != '0xf':
            self.unknown['dpp form %s' % inst.text] += 1
        # v_op_dpp d, a, b row_shr:n : lane i combines a from lane i-n of the same 16-lane row with b of lane i;
        # lanes whose source is outside the row keep d (bound_ctrl off)
        self.emit('{ uint src_ = subgroupShuffleUp(%s, %du); bool ok_ = (gl_SubgroupInvocationID & 15u) >= %du; '
                  'if (EX && ok_) %s = floatBitsToUint(%s(uintBitsToFloat(src_), uintBitsToFloat(%s))); }'
                  % (self.src_u32(a), shr, shr, self.reg(d), fn, self.src_u32(c)))
        self.wave_points.add(inst.off)

    def op_v_min_f32(self, inst):
        if inst.encoding == 'dpp':
            return self.op_dpp(inst, 'min')
        d, a, c = inst.ops
        self.write_v_f32(inst, d, 'min(%s, %s)' % (self.src_f32(a, inst, 0), self.src_f32(c, inst, 1)))

    def op_v_max_f32(self, inst):
        if inst.encoding == 'dpp':
            return self.op_dpp(inst, 'max')
        d, a, c = inst.ops
        self.write_v_f32(inst, d, 'max(%s, %s)' % (self.src_f32(a, inst, 0), self.src_f32(c, inst, 1)))

    # --- VALU compares
    def write_mask(self, inst, d, cond):
        """VOPC result: thread's bit of the lane mask (0 for inactive lanes)"""
        if d is None:
            d = gcnisa.Operand('vcc')
        self.emit('%s' % self.mask_store(d, 'EX && (%s)' % cond))

    def mask_store(self, d, cond):
        """per-lane write of a lane mask: this half's bit of the thread's two bits"""
        if d.kind == 'null':
            return ''
        self.set_kind(d, 'mask')
        b = self.lane_bit_const()
        return '%s = (%s & ~%s) | ((%s) ? %s : 0u); %s = 0u;' % (self.reg(d, 0), self.reg(d, 0), b, cond, b,
                                                                 self.reg(d, 1))

    def mask_bit(self, o):
        """a lane mask operand: this half's bit (per-lane instruction), or any of the thread's two (scalar)"""
        if o.kind == 'reg' and o.file == 's' and o.first == 126:
            return 'EX' if self.h is not None else '(EXM != 0u)'
        if o.kind == 'reg' and o.file == 's':
            if self.mask_kind.get(o.first) == 'value':
                if self.h is None:
                    return '(lane_bits2(%s, %s) != 0u)' % (self.reg(o, 0), self.reg(o, 1))
                return 'lane_bit(%s, %s, LANE)' % (self.reg(o, 0), self.reg(o, 1))
            if self.h is None:
                return '(%s != 0u)' % self.reg(o, 0)
            return '((%s & %s) != 0u)' % (self.reg(o, 0), self.lane_bit_const())
        if o.kind == 'const':
            return 'true' if o.value else 'false'
        raise ValueError(o)

    def mask_val(self, o):
        """a lane mask operand of a scalar instruction as the thread's two bits"""
        if o.kind == 'reg' and o.file == 's' and o.first == 126:
            return 'EXM'
        if o.kind == 'reg' and o.file == 's':
            if self.mask_kind.get(o.first) == 'value':
                return 'lane_bits2(%s, %s)' % (self.reg(o, 0), self.reg(o, 1))
            return '(%s & 3u)' % self.reg(o, 0)
        if o.kind == 'const':
            lo = o.value & 0xffffffff
            hi = 0xffffffff if o.text.startswith('-') else 0
            if lo == 0xffffffff and hi == 0xffffffff:
                return '3u'
            if lo == 0 and hi == 0:
                return '0u'
            return 'lane_bits2(0x%08xu, 0x%08xu)' % (lo, hi)
        raise ValueError(o)

    def set_kind(self, o, kind):
        if o.kind == 'reg' and o.file == 's':
            self.mask_kind[o.first] = kind
            if o.count == 1 and kind == 'value' and o.first % 2 == 1:
                self.mask_kind[o.first - 1] = kind

    def cmp_expr(self, inst, base):
        m = re.match(r'v_cmpx?_(\w+?)_(f32|f16|u32|i32|u16|i16|u64|i64)$', base)
        if not m:
            raise ValueError(base)
        cond, ty = m.group(1), m.group(2)
        a, c = inst.ops[-2], inst.ops[-1]
        if ty == 'f32':
            x, y = self.src_f32(a, inst, 0), self.src_f32(c, inst, 1)
        elif ty == 'f16':
            sel0 = inst.mods.get('src0_sel', 'DWORD') if inst.encoding == 'sdwa' else 'DWORD'
            sel1 = inst.mods.get('src1_sel', 'DWORD') if inst.encoding == 'sdwa' else 'DWORD'
            x = self.f16_operand(a, 1 if sel0 == 'WORD_1' else 0, inst)
            y = self.f16_operand(c, 1 if sel1 == 'WORD_1' else 0, inst)
        elif ty in ('u64', 'i64'):
            def wide(o):
                if o.kind == 'const':
                    return 'uint64_t(%du)' % o.value if o.value < 65 else 'uint64_t(0x%xul)' % o.value
                return 'packUint2x32(uvec2(%s, %s))' % (self.reg(o, 0), self.reg(o, 1))
            x, y = wide(a), wide(c)
            if ty == 'i64':
                x, y = 'int64_t(%s)' % x, 'int64_t(%s)' % y
        else:
            x, y = self.src_int(a, inst, 0), self.src_int(c, inst, 1)
            if ty in ('u16', 'i16'):
                x, y = '(%s & 0xffffu)' % x, '(%s & 0xffffu)' % y
                if ty == 'i16':
                    x, y = 'bitfieldExtract(int(%s), 0, 16)' % x, 'bitfieldExtract(int(%s), 0, 16)' % y
            elif ty == 'i32':
                x, y = 'int(%s)' % x, 'int(%s)' % y
        ops = {'eq': '==', 'ne': '!=', 'lg': '!=', 'lt': '<', 'le': '<=', 'gt': '>', 'ge': '>='}
        if cond in ops:
            e = '%s %s %s' % (x, ops[cond], y)
            if ty in ('f32', 'f16') and cond in ('ne', 'lg'):
                e = '(%s %s %s)' % (x, '<', y) + ' || ' + '(%s %s %s)' % (x, '>', y)
            return e
        neg = {'neq': '==', 'nlt': '<', 'nle': '<=', 'ngt': '>', 'nge': '>=', 'nlg': None}
        if cond in neg and neg[cond]:
            return '!(%s %s %s)' % (x, neg[cond], y)
        if cond == 'class':
            return 'cmp_class(%s, %s)' % (x, y)
        raise ValueError(cond)

    def op_vcmp(self, inst):
        if inst.base == 'v_cmp_class_f32':
            return self.op_v_cmp_class_f32(inst)
        d = inst.ops[0] if len(inst.ops) == 3 else gcnisa.Operand('vcc')
        self.write_mask(inst, d, self.cmp_expr(inst, inst.base))

    def op_vcmpx(self, inst):
        if inst.base == 'v_cmpx_class_f32':
            a, m = inst.ops[-2], inst.ops[-1]
            cond = 'cmp_class(%s, %s)' % (self.src_f32(a, inst, 0), self.src_int(m, inst, 1))
        else:
            cond = self.cmp_expr(inst, inst.base.replace('v_cmpx_', 'v_cmp_'))
        self.emit('if (!(EX && (%s))) EXM &= ~%s;' % (cond, self.lane_bit_const()))

    # --- SALU
    def sval(self, o, i=0):
        return self.src_u32(o, i)

    def op_s_mov_b32(self, inst):
        d, a = inst.ops
        self.emit('%s = %s;' % (self.reg(d), self.sval(a)))
        self.set_kind(d, 'value')

    def op_s_movk_i32(self, inst):
        d, a = inst.ops
        v = a.value & 0xffff
        v = v - 0x10000 if v & 0x8000 else v
        self.emit('%s = %du;' % (self.reg(d), v & 0xffffffff))
        self.set_kind(d, 'value')

    def op_s_mov_b64(self, inst):
        d, a = inst.ops
        if d.first == 126:        # exec = mask
            self.emit('EXM = %s;' % self.mask_val(a))
            return
        if a.kind == 'reg' and a.first == 126:
            self.emit('%s = EXM; %s = 0u;' % (self.reg(d, 0), self.reg(d, 1)))
            self.set_kind(d, 'mask')
            return
        if a.kind == 'const':
            v = a.value if not (a.value & 0x80000000 and a.text.startswith('-')) else a.value
            self.emit('%s = %s; %s = %s;' % (self.reg(d, 0), '0x%08xu' % (a.value & 0xffffffff), self.reg(d, 1),
                                             '0xffffffffu' if a.text.startswith('-') else '0u'))
            self.set_kind(d, 'value')
            return
        self.emit('%s = %s; %s = %s;' % (self.reg(d, 0), self.reg(a, 0), self.reg(d, 1), self.reg(a, 1)))
        self.mask_kind[d.first] = self.mask_kind.get(a.first, 'value')

    def logic64(self, inst, fn_bits, fn_mask):
        d, a, c = inst.ops
        ka = 'mask' if (a.kind == 'reg' and (a.first == 126 or self.mask_kind.get(a.first) == 'mask')) else 'value'
        kc = 'mask' if (c.kind == 'reg' and (c.first == 126 or self.mask_kind.get(c.first) == 'mask')) else 'value'
        if a.kind == 'const':
            ka = kc
        if c.kind == 'const':
            kc = ka
        if ka == 'mask' or kc == 'mask':
            if ka != kc:
                self.notes.append('%05x %s mixes a lane mask and a value' % (inst.off, inst.text))
            r = fn_mask(self.mask_val(a), self.mask_val(c))
            if d.first == 126:
                self.emit('{ uint r_ = (%s) & 3u; EXM = r_; SCC = r_ != 0u; }' % r)
            else:
                self.emit('{ uint r_ = (%s) & 3u; %s = r_; %s = 0u; SCC = r_ != 0u; }' % (r, self.reg(d, 0),
                                                                                       self.reg(d, 1)))
                self.set_kind(d, 'mask')
            self.scc_note(inst)
            return
        lo = fn_bits(self.sval(a, 0), self.sval(c, 0))
        hi = fn_bits(self.sval(a, 1) if a.kind == 'reg' else ('0xffffffffu' if a.text.startswith('-') else '0u'),
                     self.sval(c, 1) if c.kind == 'reg' else ('0xffffffffu' if c.text.startswith('-') else '0u'))
        self.emit('{ uint lo_ = %s, hi_ = %s; %s = lo_; %s = hi_; SCC = (lo_ | hi_) != 0u; }' % (
            lo, hi, self.reg(d, 0), self.reg(d, 1)))
        self.set_kind(d, 'value')

    def scc_note(self, inst):
        pass

    def op_s_and_b64(self, inst):
        self.logic64(inst, lambda x, y: '(%s & %s)' % (x, y), lambda x, y: '(%s & %s)' % (x, y))

    def op_s_or_b64(self, inst):
        self.logic64(inst, lambda x, y: '(%s | %s)' % (x, y), lambda x, y: '(%s | %s)' % (x, y))

    def op_s_andn2_b64(self, inst):
        self.logic64(inst, lambda x, y: '(%s & ~%s)' % (x, y), lambda x, y: '(%s & ~%s)' % (x, y))

    def op_s_orn2_b64(self, inst):
        self.logic64(inst, lambda x, y: '(%s | ~%s)' % (x, y), lambda x, y: '(%s | ~%s)' % (x, y))

    def op_s_nor_b64(self, inst):
        self.logic64(inst, lambda x, y: '~(%s | %s)' % (x, y), lambda x, y: '~(%s | %s)' % (x, y))

    def op_s_not_b64(self, inst):
        d, a = inst.ops
        if a.kind == 'reg' and (a.first == 126 or self.mask_kind.get(a.first) == 'mask'):
            self.emit('%s = ~%s & 3u; %s = 0u; SCC = %s != 0u;' % (self.reg(d, 0), self.mask_val(a),
                                                                  self.reg(d, 1), self.reg(d, 0)))
            self.set_kind(d, 'mask')
            return
        self.emit('%s = ~%s; %s = ~%s; SCC = (%s | %s) != 0u;' % (self.reg(d, 0), self.reg(a, 0), self.reg(d, 1),
                                                                  self.reg(a, 1), self.reg(d, 0), self.reg(d, 1)))
        self.set_kind(d, 'value')

    def saveexec(self, inst, fn):
        d, a = inst.ops
        self.emit('{ uint old_ = EXM; %s = old_; %s = 0u; EXM = (%s) & 3u; SCC = EXM != 0u; }' % (
            self.reg(d, 0), self.reg(d, 1), fn('old_', self.mask_val(a))))
        self.set_kind(d, 'mask')

    def op_s_and_saveexec_b64(self, inst):
        self.saveexec(inst, lambda e, s: '(%s & %s)' % (s, e))

    def op_s_andn1_saveexec_b64(self, inst):       # EXEC = ~S0 & EXEC
        self.saveexec(inst, lambda e, s: '(~%s & %s)' % (s, e))

    def op_s_andn2_saveexec_b64(self, inst):       # EXEC = S0 & ~EXEC
        self.saveexec(inst, lambda e, s: '(%s & ~%s)' % (s, e))

    def op_s_or_saveexec_b64(self, inst):
        self.saveexec(inst, lambda e, s: '(%s | %s)' % (s, e))

    def op_s_orn2_saveexec_b64(self, inst):        # EXEC = S0 | ~EXEC
        self.saveexec(inst, lambda e, s: '(%s | ~%s)' % (s, e))

    def op_s_cselect_b32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = SCC ? %s : %s;' % (self.reg(d), self.sval(a), self.sval(c)))
        self.set_kind(d, 'value')

    def op_s_cselect_b64(self, inst):
        d, a, c = inst.ops
        ka = a.kind == 'reg' and (a.first == 126 or self.mask_kind.get(a.first) == 'mask')
        kc = c.kind == 'reg' and (c.first == 126 or self.mask_kind.get(c.first) == 'mask')
        if ka or kc:
            self.emit('%s = SCC ? %s : %s; %s = 0u;' % (self.reg(d, 0), self.mask_val(a), self.mask_val(c),
                                                        self.reg(d, 1)))
            self.set_kind(d, 'mask')
            return
        self.emit('%s = SCC ? %s : %s; %s = SCC ? %s : %s;' % (
            self.reg(d, 0), self.sval(a, 0), self.sval(c, 0), self.reg(d, 1),
            self.sval(a, 1) if a.kind == 'reg' else '0u', self.sval(c, 1) if c.kind == 'reg' else '0u'))
        self.set_kind(d, 'value')

    def salu2(self, inst, expr, scc=None):
        d, a, c = inst.ops
        self.emit('{ uint r_ = %s; %s%s = r_; }' % (expr % (self.sval(a), self.sval(c)),
                                                   ('SCC = %s; ' % scc) if scc else '', self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_add_i32(self, inst):
        self.salu2(inst, 'uint(int(%s) + int(%s))')

    def op_s_sub_i32(self, inst):
        self.salu2(inst, 'uint(int(%s) - int(%s))')

    def op_s_mul_i32(self, inst):
        self.salu2(inst, '(%s * %s)')

    def op_s_mul_hi_u32(self, inst):
        self.salu2(inst, 'mulhi_u32(%s, %s)')

    def op_s_and_b32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = %s & %s; SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_lshl_b32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = %s << (%s & 31u); SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_lshl_b64(self, inst):
        d, a, c = inst.ops
        lo = self.sval(a, 0)
        hi = self.sval(a, 1) if a.kind == 'reg' else '0u'
        self.emit('{ uint64_t t_ = packUint2x32(uvec2(%s, %s)) << (%s & 63u); %s = uint(t_); %s = uint(t_ >> 32); '
                  'SCC = t_ != 0ul; }' % (lo, hi, self.sval(c), self.reg(d, 0), self.reg(d, 1)))
        self.set_kind(d, 'value')

    def op_s_bfm_b32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = ((1u << (%s & 31u)) - 1u) << (%s & 31u);' % (self.reg(d), self.sval(a), self.sval(c)))
        self.set_kind(d, 'value')

    def op_s_sub_u32(self, inst):
        d, a, c = inst.ops
        self.emit('{ uint x_ = %s, y_ = %s; %s = x_ - y_; SCC = y_ > x_; }' % (self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_subb_u32(self, inst):
        d, a, c = inst.ops
        self.emit('{ uint x_ = %s, y_ = %s, b_ = SCC ? 1u : 0u; %s = x_ - y_ - b_; SCC = uint64_t(y_) + b_ > '
                  'uint64_t(x_); }' % (self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_ff1_i32_b64(self, inst):
        d, a = inst.ops
        if a.kind == 'reg' and (a.first == 126 or self.mask_kind.get(a.first) == 'mask'):
            # a lane mask read as data: the wave's 64 bits
            self.ballot_uses.add(inst.off)
            m = self.mask_val(a)
            self.emit('{ uvec2 b_ = wave_ballot2((%s & 1u) != 0u, (%s & 2u) != 0u); %s = b_.x != 0u ? '
                      'uint(findLSB(b_.x)) : (b_.y != 0u ? 32u + uint(findLSB(b_.y)) : 0xffffffffu); }' % (
                          m, m, self.reg(d)))
            self.set_kind(d, 'value')
            return
        self.emit('{ uint lo_ = %s, hi_ = %s; %s = lo_ != 0u ? uint(findLSB(lo_)) : (hi_ != 0u ? 32u + uint(findLSB(hi_)) '
                  ': 0xffffffffu); }' % (self.sval(a, 0), self.sval(a, 1), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_bitcmp1_b32(self, inst):
        a, c = inst.ops
        self.emit('SCC = ((%s >> (%s & 31u)) & 1u) != 0u;' % (self.sval(a), self.sval(c)))

    def op_scmp(self, inst):
        m = re.match(r's_cmp_(eq|lg|lt|le|gt|ge)_(u32|i32|u64)$', inst.base)
        a, c = inst.ops
        x, y = self.sval(a), self.sval(c)
        if m.group(2) == 'i32':
            x, y = 'int(%s)' % x, 'int(%s)' % y
        rel = {'eq': '==', 'lg': '!=', 'lt': '<', 'le': '<=', 'gt': '>', 'ge': '>='}[m.group(1)]
        self.emit('SCC = %s %s %s;' % (x, rel, y))

    # --- scalar memory
    def op_sload(self, inst):
        d, base, off = inst.ops
        n = d.count
        # gfx10 SMEM: the offset operand is an immediate or an SGPR (soffset), plus an immediate offset:N modifier
        imm = int(inst.mods.get('offset', '0'), 0)
        if off.kind == 'null':
            offset = '%dul' % imm
        elif off.kind == 'const':
            offset = '%dul' % (off.value + imm)
        else:
            offset = '(uint64_t(%s) + %dul)' % (self.reg(off), imm)
        addr = 'packUint2x32(uvec2(%s, %s & 0xffffu))' % (self.reg(base, 0), self.reg(base, 1))
        # one translation per load (at its first dword: a load does not cross a 16 KB page): through the snapshot
        # entry of the load's reach node (scalar_reach); a pointer the chains do not reach (loaded at a run-time
        # offset, computed) is read through the page table, as the emulator's own translation reads it
        node = getattr(self, 'sload_node', {}).get(inst.off)
        xl = ('sxlate_k(%du, %s + %s, %du)' % (node, addr, offset, 4 * n) if node is not None else
              'xlate(%s + %s)' % (addr, offset))
        loads = ' '.join('%s = sld32(d_ == 0ul ? 0ul : d_ + %dul);' % (self.reg(d, k), 4 * k) for k in range(n))
        self.emit('{ uint64_t d_ = %s; %s probe_sload(%s + %s, %s); }' % (
            xl, loads, addr, offset, self.reg(d, 0)))
        self.set_kind(d, 'value')

    def op_sbufload(self, inst):
        d, vs, off = inst.ops
        imm = int(inst.mods.get('offset', '0'), 0)
        if off.kind == 'null':
            soff = '0u'
        elif off.kind == 'const':
            soff = '%du' % off.value
        else:
            soff = self.reg(off)
        w = [self.reg(vs, k) for k in range(4)]
        # the buffer's size in bytes: num_records, times the stride when there is one (a constant buffer V# with a
        # stride counts elements)
        loads = ' '.join('%s = (o_ + %du < l_) ? sld32(d_ == 0ul ? 0ul : d_ + %dul) : 0u;' % (
            self.reg(d, k), 4 * k, 4 * k) for k in range(d.count))
        # one translation per V# (sbuf_xlate: the V#'s snapshot entry or the page table), shared by its loads
        node = getattr(self, 'sload_node', {}).get(inst.off)
        self.emit('{ uint o_ = %s + %du; uint st_ = (%s >> 16) & 0x3fffu; uint l_ = st_ != 0u ? %s * st_ : %s; '
                  'uint64_t b_ = sbuf_xlate(%d, %s, %s); uint64_t d_ = (o_ < l_ && b_ != 0ul) ? b_ + uint64_t(o_) : 0ul; '
                  '%s }' % (soff, imm, w[1], w[2], w[2], node if node is not None else -1, w[0], w[1], loads))
        self.set_kind(d, 'value')

    # --- vector memory
    def mubuf_addr(self, inst, vaddr, vs, soff):
        imm = int(inst.mods.get('offset', '0'), 0)
        idx = self.reg(vaddr, 0) if inst.mods.get('idxen') else '0u'
        voff = self.reg(vaddr, 1 if inst.mods.get('idxen') else 0) if inst.mods.get('offen') else '0u'
        so = '0u' if soff.kind in ('const', 'null') and soff.value == 0 else self.src_u32(soff)
        return '%s, %s, %s, %s, %s, %s + %s + %du' % (
            self.reg(vs, 0), self.reg(vs, 1), self.reg(vs, 2), self.reg(vs, 3), idx, voff, so, imm)

    def op_bufload(self, inst):
        d, vaddr, vs, soff = inst.ops
        b = inst.base
        a = self.mubuf_addr(inst, vaddr, vs, soff)
        if b.startswith('buffer_load_dword'):
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); %s }' % (a, ' '.join(
                '%s = buf_load32(ba_, %d);' % (self.reg(d, k), 4 * k) for k in range(d.count))))
        elif b.startswith('buffer_load_format_'):
            comps = len(b.rsplit('_', 1)[1])
            self.emit('if (EX) { vec4 f_ = buf_load_format(buf_addr(%s)); %s }' % (a, ' '.join(
                '%s = floatBitsToUint(f_.%s);' % (self.reg(d, k), 'xyzw'[k]) for k in range(comps))))
        elif b == 'buffer_load_short_d16':
            self.emit('if (EX) %s = (%s & 0xffff0000u) | buf_load16(buf_addr(%s));' % (self.reg(d), self.reg(d), a))
        elif b == 'buffer_load_short_d16_hi':
            self.emit('if (EX) %s = (%s & 0xffffu) | (buf_load16(buf_addr(%s)) << 16);' % (self.reg(d), self.reg(d), a))
        elif b == 'buffer_load_sshort':
            self.emit('if (EX) %s = uint(bitfieldExtract(int(buf_load16(buf_addr(%s))), 0, 16));' % (self.reg(d), a))
        else:
            self.unknown[inst.op] += 1

    def op_bufstore(self, inst):
        data, vaddr, vs, soff = inst.ops
        b = inst.base
        a = self.mubuf_addr(inst, vaddr, vs, soff)
        if b.startswith('buffer_store_dword'):
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); %s }' % (a, ' '.join(
                'buf_store32(ba_, %d, %s);' % (4 * k, self.reg(data, k)) for k in range(data.count))))
        elif b == 'buffer_store_short':
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); buf_store16(ba_, %s & 0xffffu); }' % (
                a, self.reg(data)))
        elif b == 'buffer_store_short_d16_hi':
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); buf_store16(ba_, %s >> 16); }' % (
                a, self.reg(data)))
        else:
            self.unknown[inst.op] += 1

    def op_globalload(self, inst):
        d, vaddr, saddr = inst.ops
        imm = int(inst.mods.get('offset', '0'), 0)
        # device address accesses: each lane translates its own address, as Kyty's DMA loads do
        if saddr.kind == 'off':
            addr = 'packUint2x32(uvec2(%s, %s))' % (self.reg(vaddr, 0), self.reg(vaddr, 1))
        else:
            addr = '(packUint2x32(uvec2(%s, %s)) + uint64_t(%s))' % (self.reg(saddr, 0), self.reg(saddr, 1),
                                                                    self.reg(vaddr, 0))
        addr = '%s + %dul' % (addr, imm) if imm else addr
        b = inst.base
        tr_ = 'xlate_dma(%s)' % addr
        if b.startswith('global_load_dword'):
            self.emit('if (EX) { uint64_t d_ = %s; %s }' % (tr_, ' '.join(
                '%s = ldd32(d_ == 0ul ? 0ul : d_ + %dul);' % (self.reg(d, k), 4 * k) for k in range(d.count))))
        elif b == 'global_load_short_d16':
            self.emit('if (EX) %s = (%s & 0xffff0000u) | ldd16(%s);' % (self.reg(d), self.reg(d), tr_))
        elif b == 'global_load_short_d16_hi':
            self.emit('if (EX) %s = (%s & 0xffffu) | (ldd16(%s) << 16);' % (self.reg(d), self.reg(d), tr_))
        elif b == 'global_load_ubyte':
            self.emit('if (EX) %s = ldd8(%s);' % (self.reg(d), tr_))
        else:
            self.unknown[inst.op] += 1

    def op_ds(self, inst):
        b = inst.base
        o0 = int(inst.mods.get('offset0', '0'), 0)
        o1 = int(inst.mods.get('offset1', '0'), 0)
        off = int(inst.mods.get('offset', '0'), 0)
        self.lds_points.add(inst.off)
        if b in ('ds_read_b32', 'ds_read_b64', 'ds_read_b128'):
            d, addr = inst.ops
            n = {'ds_read_b32': 1, 'ds_read_b64': 2, 'ds_read_b128': 4}[b]
            self.emit('if (EX) { uint a_ = (%s + %du) >> 2; %s }' % (self.reg(addr), off, ' '.join(
                '%s = LDS[a_ + %du];' % (self.reg(d, k), k) for k in range(n))))
            self.lds_reads.add(inst.off)
        elif b in ('ds_read2_b32', 'ds_read2st64_b32'):
            d, addr = inst.ops
            scale = 64 if 'st64' in b else 1
            self.emit('if (EX) { uint a_ = %s >> 2; %s = LDS[a_ + %du]; %s = LDS[a_ + %du]; }' % (
                self.reg(addr), self.reg(d, 0), o0 * scale, self.reg(d, 1), o1 * scale))
            self.lds_reads.add(inst.off)
        elif b in ('ds_write_b32', 'ds_write_b64', 'ds_write_b128'):
            addr, data = inst.ops
            n = {'ds_write_b32': 1, 'ds_write_b64': 2, 'ds_write_b128': 4}[b]
            self.emit('if (EX) { uint a_ = (%s + %du) >> 2; %s }' % (self.reg(addr), off, ' '.join(
                'LDS[a_ + %du] = %s;' % (k, self.reg(data, k)) for k in range(n))))
            self.lds_writes.add(inst.off)
        elif b in ('ds_write2_b32', 'ds_write2st64_b32'):
            addr, d0, d1 = inst.ops
            scale = 64 if 'st64' in b else 1
            self.emit('if (EX) { uint a_ = %s >> 2; LDS[a_ + %du] = %s; LDS[a_ + %du] = %s; }' % (
                self.reg(addr), o0 * scale, self.reg(d0), o1 * scale, self.reg(d1)))
            self.lds_writes.add(inst.off)
        elif b in ('ds_min_f32', 'ds_max_f32'):
            addr, data = inst.ops
            self.emit('if (EX) lds_atomic_%s_f32((%s + %du) >> 2, %s);' % (b[3:6], self.reg(addr), off,
                                                                           self.reg(data)))
            self.lds_writes.add(inst.off)
        else:
            self.unknown[inst.op] += 1


FLOAT2 = {
    'v_add_f32': '(%s + %s)', 'v_sub_f32': '(%s - %s)', 'v_mul_f32': '(%s * %s)',
}
FLOAT1 = {
    'v_rcp_f32': 'rcp_f32(%s)', 'v_rcp_iflag_f32': 'rcp_f32(%s)', 'v_rsq_f32': 'rsq_f32(%s)',
    'v_sqrt_f32': 'sqrt(%s)', 'v_exp_f32': 'exp2(%s)', 'v_log_f32': 'log2(%s)',
    'v_sin_f32': 'sin_rev(%s)', 'v_fract_f32': 'fract_f32(%s)',
}
INT2 = {
    'v_add_nc_u32': '(%s + %s)', 'v_sub_nc_u32': '(%s - %s)', 'v_and_b32': '(%s & %s)',
    'v_mul_lo_u32': '(%s * %s)',
    'v_mul_u32_u24': '((%s & 0xffffffu) * (%s & 0xffffffu))', 'v_max_i32': 'uint(max(int(%s), int(%s)))',
}
INT3 = {
    'v_add3_u32': '(%s + %s + %s)', 'v_lshl_add_u32': '((%s << (%s & 31u)) + %s)',
    'v_mad_u32_u24': '((%s & 0xffffffu) * (%s & 0xffffffu) + %s)',
}


def install_reversed(tr):
    """operations whose GLSL form needs the operands in another order"""
    def subrev_f(inst):
        d, a, c = inst.ops
        tr.write_v_f32(inst, d, '(%s - %s)' % (tr.src_f32(c, inst, 1), tr.src_f32(a, inst, 0)))

    def subrev_u(inst):
        d, a, c = inst.ops
        tr.write_v_u32(inst, d, '(%s - %s)' % (tr.src_int(c, inst, 1), tr.src_int(a, inst, 0)))

    def lshlrev(inst):
        d, a, c = inst.ops
        tr.write_v_u32(inst, d, '(%s << (%s & 31u))' % (tr.src_int(c, inst, 1), tr.src_int(a, inst, 0)))

    def lshrrev(inst):
        d, a, c = inst.ops
        tr.write_v_u32(inst, d, '(%s >> (%s & 31u))' % (tr.src_int(c, inst, 1), tr.src_int(a, inst, 0)))

    def ashrrev(inst):
        d, a, c = inst.ops
        tr.write_v_u32(inst, d, 'uint(int(%s) >> (%s & 31u))' % (tr.src_int(c, inst, 1), tr.src_int(a, inst, 0)))

    tr.op_v_subrev_f32 = subrev_f
    tr.op_v_subrev_nc_u32 = subrev_u
    tr.op_v_lshlrev_b32 = lshlrev
    tr.op_v_lshrrev_b32 = lshrrev
    tr.op_v_ashrrev_i32 = ashrrev


# ---------------------------------------------------------------------------------------------------------- structure
LANE_BRANCHES = ('s_cbranch_execz', 's_cbranch_execnz', 's_cbranch_vccz', 's_cbranch_vccnz')
NEVER_SKIP = False

CROSS_LANE = ('v_readlane_b32', 'v_permlanex16_b32', 'v_writelane_b32', 'ds_swizzle_b32', 'ds_bpermute_b32',
              'ds_permute_b32', 's_barrier')


class Structure:
    def __init__(self, insts):
        self.insts = insts
        self.index = {i.off: k for k, i in enumerate(insts)}
        n = len(insts)
        self.loops = {}           # header index -> last latch index
        self.latches = {}         # latch index -> header index
        self.fwd = {}             # branch index -> target index
        for k, i in enumerate(insts):
            if i.target is None:
                continue
            t = self.index.get(i.target, n if i.target >= insts[-1].off + insts[-1].size else None)
            if t is None:
                raise ValueError('branch into the middle of an instruction: %r' % i)
            if t <= k:
                self.loops[t] = max(self.loops.get(t, k), k)
                self.latches[k] = t
            else:
                self.fwd[k] = t
        spans = sorted(self.loops.items())
        for a in spans:
            for b in spans:
                if a[0] < b[0] <= a[1] < b[1]:
                    raise ValueError('overlapping loops %s %s' % (a, b))
        for b, t in self.fwd.items():
            for h, j in self.loops.items():
                if h < t <= j and not (h <= b <= j):
                    raise ValueError('branch %05x enters the loop at %05x..%05x in the middle' % (
                        insts[b].off, insts[h].off, insts[j].off))

    def loop_of(self, k):
        """innermost loop (header, last latch) containing instruction k"""
        best = None
        for h, j in self.loops.items():
            if h <= k <= j and (best is None or h > best[0]):
                best = (h, j)
        return best


def value_defs(inst):
    """SGPRs an instruction writes as wave-uniform values (not lane masks)"""
    b = inst.base
    if b.startswith(('v_cmp', 's_and_saveexec', 's_or_saveexec', 's_andn1_saveexec', 's_andn2_saveexec',
                     's_orn2_saveexec')) or b in ('s_and_b64', 's_or_b64', 's_andn2_b64', 's_orn2_b64', 's_nor_b64',
                                                  's_not_b64', 's_cselect_b64', 's_mov_b64', 'v_add_co_u32',
                                                  'v_add_co_ci_u32'):
        return []
    if not inst.ops or b.startswith(('buffer_', 'global_', 'ds_', 's_cbranch', 's_branch', 's_cmp', 's_bitcmp')):
        return []
    d = inst.ops[0]
    if d.kind == 'reg' and d.file == 's':
        return d.regs()
    return []


def all_sgpr_defs(inst):
    """every SGPR an instruction writes (values and lane masks), including exec"""
    b = inst.base
    out = []
    if b.startswith('v_cmpx') or 'saveexec' in b:
        out += ['s126', 's127']
    if not inst.ops or b.startswith(('buffer_store', 'ds_write', 'ds_min', 'ds_max', 's_cbranch', 's_branch',
                                     's_cmp', 's_bitcmp', 'v_cmpx')):
        return out
    d = inst.ops[0]
    if d.kind == 'reg' and d.file == 's':
        out += d.regs()
    if b in ('v_add_co_u32', 'v_add_co_ci_u32') and len(inst.ops) > 1:
        out += inst.ops[1].regs()
    return out


def uses_of(inst):
    regs = []
    for o in inst.ops[1:] if inst.ops else []:
        regs += o.regs()
    if inst.base.startswith(('buffer_store', 'ds_write', 'ds_min', 'ds_max', 's_cmp', 's_bitcmp')):
        regs += inst.ops[0].regs()
    return regs


def scalar_reach(insts, user_sgprs, hints=None):
    """The guest memory the scalar loads read, as pointer chains from the user data, for the emulator to bring
    CPU-written tables up to date before a dispatch: nodes are 64-bit pointers, a user SGPR pair or a pair loaded by an
    s_load from (node + offset); each node gets the extent read through it. Program order: a pair stops being a
    node once anything else writes it. Returns [(kind, a, b, extent)]: ('user', sgpr, 0, extent) or
    ('load', parent node, offset, extent); unbounded extents (offset in an SGPR) are left to the emulator's own
    tracking. ('vsharp', parent node, offset, 0): a V# that s_buffer_loads read through (its whole range).
    hints (a dict): instruction offset -> the node an s_load reads through, or the V# node of an s_buffer_load (the
    emulator's ScalarTable has one entry per node, so a load finds its snapshot without a search)."""
    nodes = []
    holder = {}        # SGPR -> (node, byte offset) of the dword loaded into it
    redefined = set()

    def node_of(base):
        if base.kind != 'reg' or base.file != 's' or base.count < 2:
            return None
        r = base.first
        a, b = holder.get(r), holder.get(r + 1)
        if a is not None and b is not None and a[0] == b[0] and b[1] == a[1] + 4:
            key = ('load', a[0], a[1])
        elif r + 1 < user_sgprs and r not in redefined and r + 1 not in redefined:
            key = ('user', r, 0)
        else:
            return None
        for i, n in enumerate(nodes):
            if tuple(n[:3]) == key:
                return i
        nodes.append([key[0], key[1], key[2], 0])
        return len(nodes) - 1

    def vsharp_of(base):
        """a V# in s[r:r+3] loaded by one s_load from (node + offset): ('vsharp', node, offset)"""
        if base.kind != 'reg' or base.file != 's' or base.count != 4:
            return None
        parts = [holder.get(base.first + k) for k in range(4)]
        if any(p is None for p in parts) or any(p[0] != parts[0][0] or p[1] != parts[0][1] + 4 * k
                                                for k, p in enumerate(parts)):
            return None
        key = ('vsharp', parts[0][0], parts[0][1])
        for i, n in enumerate(nodes):
            if tuple(n[:3]) == key:
                return i
        nodes.append([key[0], key[1], key[2], 0])
        return len(nodes) - 1

    for i in insts:
        if i.base.startswith('s_buffer_load_dword'):
            v = vsharp_of(i.ops[1])     # the emulator reads the V# and snapshots its range
            if hints is not None and v is not None:
                hints[i.off] = v
            for r in i.ops[0].regs():
                holder.pop(int(r[1:]), None)
                redefined.add(int(r[1:]))
            continue
        if i.base.startswith('s_load_dword'):
            d, base, off = i.ops
            n = node_of(base)
            if hints is not None and n is not None:
                hints[i.off] = n
            imm = int(i.mods.get('offset', '0'), 0)
            offset = imm if off.kind == 'null' else (off.value + imm if off.kind == 'const' else None)
            for r in d.regs():
                holder.pop(int(r[1:]), None)
                redefined.add(int(r[1:]))
            if n is None or offset is None:
                continue
            nodes[n][3] = max(nodes[n][3], offset + 4 * d.count)
            for k in range(d.count):
                holder[d.first + k] = (n, offset + 4 * k)
            continue
        for r in all_sgpr_defs(i):
            holder.pop(int(r[1:]), None)
            redefined.add(int(r[1:]))
    return [tuple(n) for n in nodes]


def classify_lp(tr, st):
    """lane pairs: one guest wave is one warp, so every exec / lane mask branch is decided for the wave with a
    subgroup vote (the guest's own semantics); no branch or loop runs per lane"""
    insts = st.insts
    lane_branch = set(k for k, i in enumerate(insts) if i.op in LANE_BRANCHES and not tr.uniform_branch.get(k))
    return set(lane_branch), lane_branch, set()


def classify(tr, st):
    """branches that need the wave's decision; loops whose threads run per lane"""
    insts = st.insts
    n = len(insts)
    wave_point = [False] * n
    for k, i in enumerate(insts):
        if i.base in CROSS_LANE or i.encoding == 'dpp' or k in tr.ballot_index:
            wave_point[k] = True
    lane_branch = set(k for k, i in enumerate(insts) if i.op in LANE_BRANCHES and not tr.uniform_branch.get(k))

    def live_after(start, regs):
        pending, found = set(regs), set()
        for i in insts[start:]:
            for r in uses_of(i):
                if r in pending:
                    found.add(r)
                    pending.discard(r)
            for r in all_sgpr_defs(i):
                pending.discard(r)
            if not pending:
                break
        return found

    wave = set()
    for b, t in st.fwd.items():
        if b not in lane_branch:
            continue
        defs = set()
        for i in insts[b + 1:t]:
            defs.update(value_defs(i))
        if defs and live_after(t, defs):
            wave.add(b)
    changed = True
    while changed:
        changed = False
        for b, t in st.fwd.items():
            if b in lane_branch and b not in wave and any(wave_point[k] or k in wave for k in range(b + 1, t)):
                wave.add(b)
                changed = True
        for h, j in st.loops.items():
            if not any(wave_point[k] or k in wave for k in range(h, j + 1)):
                continue
            for k in range(h, j + 1):
                exits = (k in st.fwd and st.fwd[k] > j) or (k in st.latches and st.latches[k] == h)
                if exits and k in lane_branch and k not in wave:
                    wave.add(k)
                    changed = True
    per_lane_loops = set()
    for h, j in st.loops.items():
        for k in range(h, j + 1):
            exits = (k in st.fwd and st.fwd[k] > j) or k in st.latches
            if exits and k in lane_branch and k not in wave:
                per_lane_loops.add(h)
    return wave, lane_branch, per_lane_loops


def exec_known_true(insts, st, lane_branch, wave):
    """per instruction: is the thread's exec bit known to be 1 before it (forward must-analysis over the
    instruction-level control flow; lane masks in SGPR pairs that are known 1 are tracked too)"""
    n = len(insts)
    succ = [[] for _ in range(n)]
    for k, i in enumerate(insts):
        if i.op == 's_endpgm':
            continue
        if i.target is not None:
            t = st.index.get(i.target, n)
            succ[k].append((t, 'taken'))
            if i.op != 's_branch':
                succ[k].append((k + 1, 'fall'))
        else:
            succ[k].append((k + 1, 'fall'))
    state_in = [None] * (n + 1)
    state_in[0] = (True, frozenset())
    work = [0]
    queued = {0}

    def transfer(k, ekt, mk):
        i = insts[k]
        b, ops = i.base, i.ops
        mk = set(mk)
        d = ops[0] if ops else None
        if b.startswith('v_cmpx'):
            return False, mk
        if 'saveexec' in b:
            src = ops[1]
            src_known = src.kind == 'reg' and src.first in mk
            if ekt:
                mk.add(d.first)
            else:
                mk.discard(d.first)
            if b == 's_and_saveexec_b64':
                return ekt and src_known, mk
            if b == 's_orn2_saveexec_b64':
                return src_known, mk
            return False, mk
        if d is not None and d.kind == 'reg' and d.file == 's' and d.first == 126:
            a = ops[1] if len(ops) > 1 else None
            if b == 's_mov_b64':
                if a.kind == 'const':
                    return a.value != 0 and a.text.startswith('-'), mk
                return a.first in mk if a.kind == 'reg' else False, mk
            if b == 's_or_b64':
                x, y = ops[1], ops[2]
                known = lambda o: o.kind == 'reg' and (o.first in mk or (o.first == 126 and ekt))
                return known(x) or known(y), mk
            return False, mk
        if b == 's_mov_b64' and d is not None and d.kind == 'reg' and len(ops) > 1 and ops[1].kind == 'reg' and \
                ops[1].first == 126:
            if ekt:
                mk.add(d.first)
            else:
                mk.discard(d.first)
            return ekt, mk
        for r in all_sgpr_defs(i):
            idx = int(r[1:])
            mk.discard(idx)
            mk.discard(idx - 1)
        return ekt, mk

    while work:
        k = work.pop()
        queued.discard(k)
        if k >= n:
            continue
        ekt, mk = state_in[k]
        out_ekt, out_mk = transfer(k, ekt, mk)
        i = insts[k]
        for t, kind in succ[k]:
            e = out_ekt
            if k in lane_branch and k not in wave:
                if i.op == 's_cbranch_execz':
                    e = kind == 'fall'
                elif i.op == 's_cbranch_execnz':
                    e = kind == 'taken'
            new = (e, frozenset(out_mk))
            old = state_in[t] if t <= n else None
            if old is None:
                merged = new
            else:
                merged = (old[0] and new[0], old[1] & new[1])
            if merged != old:
                state_in[t] = merged
                if t not in queued:
                    work.append(t)
                    queued.add(t)
    return [s[0] if s is not None else False for s in state_in[:n]]


def innermost_end_outer(stack):
    """end of the block enclosing the innermost one (where an else-part may extend to)"""
    for blk in reversed(stack[:-1]):
        if blk[0] in ('if', 'else'):
            return blk[1]
        if blk[0] == 'loop':
            return blk[1] + 1
    return 1 << 30


def translate_shader(path, threads, lds_bytes, user_sgprs, tgid_sgpr, probes=False):
    insts = gcnisa.decode(path)
    tr = Translator(insts, threads, lds_bytes, user_sgprs, tgid_sgpr)
    install_reversed(tr)
    tr.wave_points, tr.lds_points, tr.lds_reads, tr.lds_writes = set(), set(), set(), set()
    tr.ballot_index = set()
    tr.uniform_branch = {}
    st = Structure(insts)
    tr.sload_node = {}
    tr.reach = scalar_reach(insts, user_sgprs, tr.sload_node)
    # pass 1: translate every instruction (statements per instruction, kinds of lane masks in program order)
    per_inst = []
    for k, i in enumerate(insts):
        tr.cur = []
        if i.op in ('s_cbranch_vccz', 's_cbranch_vccnz'):
            tr.uniform_branch[k] = tr.mask_kind.get(106) == 'value'
        if i.target is None and i.op != 's_endpgm':
            tr.translate(i)
        per_inst.append(tr.cur)
        if i.off in tr.ballot_uses:
            tr.ballot_index.add(k)
    # classify and emit; LDS reads that follow LDS writes inside lane-level regions (cross-lane data through LDS,
    # which the guest wave orders by executing in lockstep) become wave points, then again until none is left
    per_inst_orig = [list(x) for x in per_inst]
    notes_before = len(tr.notes)
    for attempt in range(8):
        per_inst = [list(x) for x in per_inst_orig]
        tr.notes[notes_before:] = []
        wave, lane_branch, per_lane_loops = classify_lp(tr, st)
        ekt = exec_known_true(insts, st, lane_branch, wave)
        # readfirstlane where the wave may be partly inactive goes through the wave (a wave point); repeat until stable
        readfirst = [k for k, i in enumerate(insts) if i.off in tr.readfirst_offs]
        while True:
            through_wave = [k for k in readfirst if not ekt[k] and k not in tr.ballot_index]
            if not through_wave:
                break
            tr.ballot_index.update(through_wave)
            wave, lane_branch, per_lane_loops = classify_lp(tr, st)
            ekt = exec_known_true(insts, st, lane_branch, wave)
        for k in readfirst:
            per_inst[k] = [(t.replace('readfirst(', 'own_first(' if ekt[k] else 'wave_readfirst2('), v, hh)
                           for t, v, hh in per_inst[k]]
        tr.notes.append('readfirstlane: %d with the whole wave active, %d through the wave' % (
            sum(1 for k in readfirst if ekt[k]), sum(1 for k in readfirst if not ekt[k])))
        tr.notes.append('exec known to be set before %d of %d instructions' % (sum(ekt), len(ekt)))
        tr.notes.append('%d forward branches, %d loops; %d lane-dependent branches, %d of them decided for the wave; '
                        '%d per-lane loops' % (len(st.fwd), len(st.loops), len(lane_branch), len(wave),
                                               len(per_lane_loops)))

        def leaves_loop(k):
            """the forward branch at k jumps out of a loop that contains it"""
            t = st.fwd[k]
            return any(h <= k <= j and t > j for h, j in st.loops.items())

        def cond(k):
            i = insts[k]
            lane = k in lane_branch
            w = k in wave
            if i.op == 's_branch':
                return 'true'
            if i.op == 's_cbranch_scc0':
                return '!SCC'
            if i.op == 's_cbranch_scc1':
                return 'SCC'
            if i.op == 's_cbranch_execz' and w and k in st.fwd and not leaves_loop(k) and NEVER_SKIP:
                # skipping a region when no lane is active is an optimization: compiler code is valid with exec = 0
                # (the compiler leaves short regions unskipped), so the whole wave could run it with no vote. Off: the
                # skipped regions hold loops and scalar loads, and running them costs far more than the vote (game)
                return 'false'
            if i.op in ('s_cbranch_execz', 's_cbranch_execnz'):
                e = 'wave_any(EXM != 0u)' if w else '(EXM != 0u)'
                return ('!' + e) if i.op == 's_cbranch_execz' else e
            if i.op in ('s_cbranch_vccz', 's_cbranch_vccnz'):
                if not lane:
                    e = '((s106 | s107) != 0u)'
                else:
                    e = 'wave_any((s106 & 3u) != 0u)' if w else '((s106 & 3u) != 0u)'
                return ('!' + e) if i.op == 's_cbranch_vccz' else e
            raise ValueError(i)

        flag = lambda b: 'F%05x' % insts[b].off
        n = len(insts)
        out = []
        used_flags = set()
        for h, j in st.loops.items():
            used_flags.update(b for b in st.fwd if h <= b <= j)

        def is_lane(b):
            return b in lane_branch and b not in wave

        class Blk:
            def __init__(self, kind, end, lane, flag=None, header=None, else_to=None):
                self.kind, self.end, self.lane, self.flag, self.header, self.else_to = kind, end, lane, flag, header, else_to

        stack = []
        pending = {}                  # crossing branch index -> target: instructions before the target are skipped

        def put(text, extra=0):
            out.append('    ' * (1 + len(stack) + extra) + text)

        def scope_end():
            for blk in reversed(stack):
                return blk.end if blk.kind != 'loop' else blk.end + 1
            return n + 1

        def loop_blk():
            for blk in reversed(stack):
                if blk.kind == 'loop':
                    return blk
            return None

        def lane_here():
            return any(blk.lane for blk in stack)

        def open_guards(k):
            """guards of pending crossing branches that are not open in the current scope"""
            open_flags = set(blk.flag for blk in stack if blk.kind == 'guard')
            lp = loop_blk()
            for b, t in sorted(pending.items(), key=lambda x: -x[1]):
                if b in open_flags or t <= k:
                    continue
                if lp is not None and t > lp.end and not (lp.header <= b <= lp.end):
                    pass
                end = min(t, scope_end())
                if end <= k:
                    continue
                put('if (!%s) {' % flag(b))
                stack.append(Blk('guard', end, is_lane(b), flag=b))

        dirty = False
        unsynced_reads = []    # LDS reads after a write with no wave-uniform point between (inside lane-level regions)
        for k, i in enumerate(insts):
            # close blocks ending here (if/else/guard), with if -> else conversion
            while stack and stack[-1].kind != 'loop' and stack[-1].end == k:
                blk = stack.pop()
                if blk.kind == 'if' and blk.else_to is not None:
                    put('} else {')
                    stack.append(Blk('else', blk.else_to, blk.lane))
                else:
                    put('}')
            for b in [b for b, t in pending.items() if t <= k]:
                del pending[b]
            open_guards(k)
            # loop headers (outermost first)
            for h, j in sorted(((h, j) for h, j in st.loops.items() if h == k), key=lambda x: -x[1]):
                per_lane = h in per_lane_loops
                put('for (;;) {  // loop %05x..%05x%s' % (insts[h].off, insts[j].off, ' (per lane)' if per_lane else ''))
                stack.append(Blk('loop', j, per_lane, header=h))
                inner = [b for b in st.fwd if h <= b <= j]
                if inner:
                    put(' '.join('%s = false;' % flag(b) for b in inner))
                if any(insts[x].off in tr.lds_writes for x in range(h, j + 1)) and not lane_here():
                    put('wave_sync();  // LDS written in this loop')
            if i.off in tr.lds_reads and dirty and not lane_here():
                put('wave_sync();  // LDS written before')
                dirty = False
            elif i.off in tr.lds_reads and dirty:
                unsynced_reads.append(i.off)
            if i.off in tr.lds_writes:
                dirty = True
            for text, valu, half in per_inst[k]:
                if ekt[k]:
                    text = text.replace('if (EX) ', '').replace('EX && ', '')
                    guard = None
                else:
                    guard = 'EXB%d' % half if half is not None else None
                if half is not None:
                    text = re.sub(r'\bEX\b', 'EXB%d' % half, text)
                    text = re.sub(r'\bLANE\b', 'LANE%d' % half, text)
                if guard and valu and not text.startswith(('if (EXB', '{')):
                    prefix = '    ' * (1 + len(stack)) + 'if (%s) { ' % guard
                    if out and out[-1].startswith(prefix) and out[-1].endswith(' }'):
                        out[-1] = out[-1][:-2] + ' ' + text + ' }'
                    else:
                        put('if (%s) { %s }' % (guard, text))
                else:
                    put(text)
            if i.op == 's_barrier':
                put('barrier();')
            if k in st.fwd:
                t = st.fwd[k]
                c = cond(k)
                tgt = insts[t].off if t < n else 0
                top = stack[-1] if stack else None
                lp = loop_blk()
                if t == k + 1:
                    pass
                elif i.op == 's_branch' and top is not None and top.kind == 'if' and top.end == k + 1 and \
                        top.else_to is None and t <= (stack[-2].end if len(stack) > 1 and stack[-2].kind != 'loop'
                                                      else (stack[-2].end + 1 if len(stack) > 1 else n + 1)):
                    top.else_to = t                                   # then-part ends: "} else {" up to t
                elif t <= scope_end():
                    if c == 'true':
                        put('if (false) {  // -> %05x' % tgt)
                        stack.append(Blk('if', t, False))
                    else:
                        put('if (!(%s)) {  // -> %05x' % (c, tgt))
                        stack.append(Blk('if', t, is_lane(k)))
                elif lp is not None and t == lp.end + 1 and not any(blk.kind != 'loop' and blk.end > lp.end for blk in
                                                                   stack):
                    put('if (%s) break;  // -> %05x' % (c, tgt))
                else:
                    put('%s = %s;  // -> %05x (crossing)' % (flag(k), c, tgt))
                    used_flags.add(k)
                    pending[k] = t
                    if lp is not None and t > lp.end:
                        put('if (%s) break;' % flag(k))
                    else:
                        end = min(t, scope_end())
                        put('if (!%s) {' % flag(k))
                        stack.append(Blk('guard', end, is_lane(k), flag=k))
            elif k in st.latches:
                put('if (%s) continue;  // -> %05x' % (cond(k), insts[st.latches[k]].off))
            elif i.op == 's_endpgm':
                put('return;')
            # loop ends (close inner guards first)
            while stack and any(blk.kind == 'loop' and blk.end == k for blk in stack):
                blk = stack.pop()
                if blk.kind != 'loop':
                    put('}')
                    continue
                put('break;', 1)
                put('}')
                outer = loop_blk()
                for b, t in pending.items():
                    if blk.header <= b <= blk.end and outer is not None and t > outer.end:
                        put('if (%s) break;' % flag(b))
        while stack:
            stack.pop()
            put('}')
        if unsynced_reads:
            tr.notes.append('%d LDS reads follow LDS writes inside lane-level regions (no wave sync there): %s' % (
                len(unsynced_reads), ' '.join('%05x' % o for o in unsynced_reads[:12])))
        if not unsynced_reads or attempt == 7:
            break
        index_of = {i.off: k for k, i in enumerate(insts)}
        tr.ballot_index.update(index_of[o] for o in unsynced_reads)
    nvgpr = max(tr.max_v, 1)
    body = '\n'.join(out)
    flags_decl = '\n'.join('    bool %s = false;' % flag(b) for b in sorted(used_flags))
    lines = [prelude.prelude(threads, tr.lds_dwords, nvgpr, probes)]
    # initial SGPRs as the hardware sets them: user data s[0..user_count), then the enabled workgroup ids; one guest
    # dispatch per workgroup when the user data comes from a table (its workgroup ids are 0)
    lines.append('''uint initial_sgpr(uint n) {
    uvec3 id = gl_WorkGroupID;
    if (pc.user_table != 0ul) {
        // batched guest dispatches: user data and workgroup id of this workgroup's dispatch
        uint base = WgMap(pc.wg_map).v[gl_WorkGroupID.x] * 24u;
        UserTable t = UserTable(pc.user_table);
        if (n < pc.user_count) return t.v[base + n];
        uint lin = gl_WorkGroupID.x - t.v[base + 16u], gx = max(t.v[base + 17u], 1u), gy = max(t.v[base + 18u], 1u);
        id = uvec3(lin % gx, (lin / gx) % gy, lin / (gx * gy));
    } else if (n < pc.user_count) {
        return pc.user[n];
    }
    uint k = pc.user_count;
    if ((pc.tgid_mask & 1u) != 0u) { if (n == k) return id.x; k++; }
    if ((pc.tgid_mask & 2u) != 0u) { if (n == k) return id.y; k++; }
    if ((pc.tgid_mask & 4u) != 0u) { if (n == k) return id.z; k++; }
    return 0u;
}''')
    lines.append('void body() {')
    lines.append('    LANE0 = gl_SubgroupInvocationID; LANE1 = LANE0 + 32u;')
    for n in sorted(tr.used_s):
        lines.append('    uint s%d = %s;' % (n, 'initial_sgpr(%du)' % n if n < 32 else '0u'))
    for n in sorted(tr.used_v):
        # v0: the guest's local thread id (wave * 64 + lane)
        for h in (0, 1):
            lines.append('    uint v%d_%d = %s;' % (n, h, ('gl_SubgroupID * 64u + LANE%d' % h) if n == 0 else '0u'))
    lines.append('    EXM = 3u; SCC = false;')
    lines.append(flags_decl)
    lines.append(body)
    lines.append('}')
    lines.append('void main() {')
    lines.append('    if (pc.pages == 0ul) return;   // a check that could not save the outputs: run nothing')
    lines.append('    FAULT_PAGE = NUM_PAGES;')
    lines.append('    PROBE_N = 0u;')
    lines.append('    SCALARS = pc.scalars;')
    lines.append('    if (pc.user_table != 0ul) {')
    lines.append('        uint base_ = WgMap(pc.wg_map).v[gl_WorkGroupID.x] * 24u;')
    lines.append('        SCALARS = packUint2x32(uvec2(UserTable(pc.user_table).v[base_ + 20u], '
                 'UserTable(pc.user_table).v[base_ + 21u]));')
    lines.append('    }')
    lines.append('    body();')
    lines.append('    record_faults();')
    lines.append('}')
    return '\n'.join(lines) + '\n', tr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('code')
    ap.add_argument('out')
    ap.add_argument('--threads', type=int, default=64)
    ap.add_argument('--lds-bytes', type=int, default=2048)
    ap.add_argument('--user-sgprs', type=int, default=2)
    ap.add_argument('--tgid-sgpr', type=int, default=2)
    ap.add_argument('--reach-out', help='write the scalar load chains (scalar_reach) to this file')
    ap.add_argument('--probes', action='store_true', help='compile the probes (pc.debug) in')
    a = ap.parse_args()
    glsl, tr = translate_shader(a.code, a.threads, a.lds_bytes, a.user_sgprs, a.tgid_sgpr, a.probes)
    open(a.out, 'w').write(glsl)
    if a.reach_out:
        with open(a.reach_out, 'w') as f:
            for kind, x, off, extent in tr.reach:
                f.write('%s %d %d %d\n' % (kind, x, off, extent))
    for n in tr.notes:
        print('note:', n)
    if tr.unknown:
        print('UNHANDLED:', ', '.join('%s x%d' % kv for kv in tr.unknown.most_common()))


import prelude  # noqa: E402

if __name__ == '__main__':
    main()
