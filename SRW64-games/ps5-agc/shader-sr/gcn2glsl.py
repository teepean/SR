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
import os
import collections
import re
import struct
import sys

import gcnisa
import images

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
        # graphics stages (vs, ps): one guest lane per invocation (half 0 only, LMASK = 1u); the host subgroup is the
        # wave for the cross-lane instructions
        self.stage = 'cs'
        self.halves = (0, 1)
        self.gfx = None          # graphics: gfx.Interface of the module (Kyty's record)
        self.param_outputs = set()  # graphics: (parameter, location) of the exported parameters
        self.ps_inputs = {}      # pixel stage: spirv location -> Kyty's input (interface info) the program reads
        self.ps_outputs = {}     # pixel stage: MRT -> Kyty's output (interface info) the program writes
        self.ps_depth = False
        self.heap_addrs = set()  # s_load offsets whose T# address a heap image lookup reads (TADDR_<off>)
        self.fetch_map = {}      # vertex stage: embedded fetch instruction offset -> attribute (embedded_fetches)
        self.vs_inputs = {}      # vertex stage: attribute -> (location, interface info) of Kyty's vertex inputs read
        self.vgpr_limit = 1      # one past the highest VGPR the program names (relative accesses stay below it)
        self.images = None

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
        if o.kind == 'scc':
            return '(SCC ? 1u : 0u)'
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
        # input modifiers on a 32-bit operand of a bit-moving instruction (v_cndmask_b32 VOP3 / SDWA: LLVM's select of
        # -x / |x|) act on the sign bit, as the float modifiers they are; not integer negation
        if o.abs:
            bits = '(%s & 0x7fffffffu)' % bits
        if o.neg:
            bits = '(%s ^ 0x80000000u)' % bits
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
    LANE_PREFIXES = ('v_', 'buffer_', 'tbuffer_', 'global_', 'flat_', 'scratch_', 'ds_', 'image_', 'exp')
    CROSS_SCALAR = ('v_readlane_b32', 'v_readfirstlane_b32', 'v_writelane_b32')

    def translate(self, inst):
        if inst.base in ('v_mbcnt_lo_u32_b32', 'v_mbcnt_hi_u32_b32'):
            self.mbcnt_ballot(inst)
        if inst.op.startswith(self.LANE_PREFIXES) and inst.base not in self.CROSS_SCALAR:
            halves = []
            for h in self.halves:
                self.h = h
                saved, self.cur = self.cur, []
                self.translate_one(inst)
                halves.append(self.cur)
                self.cur = saved
            self.h = None
            for h, items in zip(self.halves, halves):
                for item in items:
                    self.cur.append((item[0], item[1], h))
            self.trace(inst)
            return
        self.h = None
        self.translate_one(inst)
        self.trace(inst)

    TRACE = set(int(x, 16) for x in __import__('os').environ.get('SHADER_SR_TRACE', '').split(',') if x)

    def trace(self, inst):
        """SHADER_SR_TRACE=<hex pc>,...: after the instruction, lane 0 of workgroup 0 records its destination
        registers in the probe slots (logged as s_load entries at address 0xffff0000_<pc>, one per dword; needs
        SHADER_SR_PROBES)"""
        if inst.off not in self.TRACE or not inst.ops or inst.ops[0].kind not in ('reg',):
            return
        d = inst.ops[0]
        for k in range(d.count):
            name = ('v%d_0' % (d.first + k)) if d.file == 'v' else ('s%d' % (d.first + k))
            self.emit('probe_sload(0xffff000000000000ul | %dul | (%dul << 32), %s);' % (inst.off, k, name))

    def translate_one(self, inst):
        op, b, ops = inst.op, inst.base, inst.ops
        for o in ops:
            self.note_reg(o)
        handler = getattr(self, 'op_' + b, None)
        if handler is not None:
            handler(inst)
            return
        for prefix, h in (('v_cmpx_', self.op_vcmpx), ('v_cmp_', self.op_vcmp), ('s_cmp_', self.op_scmp),
                          ('s_cmpk_', self.op_scmpk), ('s_load_', self.op_sload), ('s_buffer_load_', self.op_sbufload),
                          ('buffer_load_', self.op_bufload), ('buffer_store_', self.op_bufstore),
                          ('buffer_atomic_', self.op_bufatomic), ('image_', self.op_image),
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
        if b in ('s_waitcnt', 's_waitcnt_vscnt', 's_waitcnt_vmcnt', 's_waitcnt_expcnt', 's_waitcnt_lgkmcnt', 's_nop',
                 's_inst_prefetch', 's_barrier'):   # s_barrier: barrier() in the emission
            return
        self.unknown[op] += 1
        self.emit('/* UNHANDLED %s */' % inst.text)

    # --- graphics: exports and messages
    def op_exp(self, inst):
        tgt = inst.ops[0].text
        srcs = inst.ops[1:5]
        if tgt == 'prim':
            return                   # NGG primitive export: the input assembler builds Vulkan's primitives
        if self.stage == 'ps':
            self.ps_export(inst, tgt)
            return
        if inst.mods.get('compr'):
            self.unknown['exp compr'] += 1
            return
        comps = [None if o.kind in ('off', 'null') else self.src_u32(o) for o in srcs]
        if tgt == 'pos0':
            self.emit('if (EX) { exp_pos0(uvec4(%s)); }' % ', '.join(c or '0u' for c in comps), valu=True)
            return
        if tgt.startswith('param'):
            n = int(tgt[5:])
            loc = self.gfx.param_location(n) if self.gfx is not None else None
            if loc is None:
                return               # a parameter the pixel stage does not read (not in Kyty's interface)
            self.param_outputs.add((n, loc))
            writes = ' '.join('P%d.%s = uintBitsToFloat(%s);' % (n, 'xyzw'[k], c) for k, c in enumerate(comps) if c)
            self.emit('if (EX) { %s }' % writes, valu=True)
            return
        self.unknown['exp ' + tgt] += 1

    # --- pixel stage: interpolation (Kyty: v_interp_p1 nothing, p2 = the interpolated input, mov = the input's
    # interpolation parameter), whole quad mode
    def ps_input(self, attr):
        """Kyty's pixel-stage input of guest attribute `attr`: (spirv location, interface info) or (None, None)"""
        if self.gfx is None:
            return None, None
        for d in self.gfx.inputs:
            if d.get('kind') == 'Parameter' and int(d.get('location', -1)) == attr:
                sloc = int(d.get('spirv_location', d['location']))
                return sloc, self.gfx.interface.get(('in', sloc, 0))
        return None, None

    def op_v_interp_p1_f32(self, inst):
        pass

    def op_v_interp_p2_f32(self, inst):
        d, a = inst.ops[0], inst.ops[2]
        sloc, info = self.ps_input(a.attr)
        if info is None or info['type'] != 'float' or info['n'] <= a.chan:
            self.unknown['v_interp_p2_f32 attr %d' % a.attr] += 1
            return
        c = 'xyzw'[a.chan]
        if info['array'] is not None:
            # a per-vertex input (Kyty: spirvEmitterFlow.cpp EmitPixelAttribute): flat -> the provoking vertex, else
            # the barycentric sum over the three vertices
            if 'PerVertex' not in info['decorations'] or info['array'] != 3:
                self.unknown['v_interp_p2_f32 of an input array'] += 1
                return
            desc = next((x for x in self.gfx.inputs if x.get('kind') == 'Parameter' and int(x.get('location', -1)) ==
                         a.attr), {})
            self.ps_inputs[sloc] = info
            if desc.get('flat') == '1':
                vtx = 2 if self.gfx.pixel.get('parameter_mode') == '1' else 0
                self.write_v_u32(inst, d, 'floatBitsToUint(IN%d[%d].%s)' % (sloc, vtx, c))
                return
            bary = 'gl_BaryCoordNoPerspEXT' if self.gfx.pixel.get('no_perspective') == '1' else 'gl_BaryCoordEXT'
            self.write_v_u32(inst, d, 'floatBitsToUint(IN%d[0].%s * %s.x + IN%d[1].%s * %s.y + IN%d[2].%s * %s.z)' % (
                sloc, c, bary, sloc, c, bary, sloc, c, bary))
            return
        self.ps_inputs[sloc] = info
        self.write_v_u32(inst, d, 'floatBitsToUint(IN%d.%s)' % (sloc, c))

    def op_v_interp_mov_f32(self, inst):
        # Kyty (spirvEmitterFlow.cpp EmitInterpolationParameter): an input that is not per vertex gives its value for
        # every mode; a per-vertex one gives vertex (mode + 1) % 3 (p10: 1, p20: 2, p0: 0, or the last vertex for a
        # flat p0 when the parameter mode says so), minus vertex 0 for p10 / p20 unless the input is custom
        d, p, a = inst.ops[0], inst.ops[1], inst.ops[2]
        sloc, info = self.ps_input(a.attr)
        mode = {'p10': 0, 'p20': 1, 'p0': 2}.get(p.text)
        if info is None or info['n'] <= a.chan or mode is None or info['type'] != 'float':
            self.unknown['v_interp_mov_f32 %s attr %d' % (p.text, a.attr)] += 1
            return
        self.ps_inputs[sloc] = info
        c = 'xyzw'[a.chan]
        if info['array'] is None:
            self.write_v_u32(inst, d, 'floatBitsToUint(IN%d.%s)' % (sloc, c))
            return
        if 'PerVertex' not in info['decorations'] or info['array'] != 3:
            self.unknown['v_interp_mov_f32 of an input array'] += 1
            return
        desc = next((x for x in self.gfx.inputs if x.get('kind') == 'Parameter' and int(x.get('location', -1)) ==
                     a.attr), {})
        custom = desc.get('custom') == '1'
        flat = desc.get('flat') == '1'
        vtx = (mode + 1) % 3
        if mode == 2 and flat and self.gfx.pixel.get('parameter_mode') == '1':
            vtx = 2
        e = 'IN%d[%d].%s' % (sloc, vtx, c)
        if not custom and mode < 2:
            e = '(%s - IN%d[0].%s)' % (e, sloc, c)
        self.write_v_u32(inst, d, 'floatBitsToUint(%s)' % e)

    def wqm_expr(self, bits):
        """whole quad mode of the thread's lane bit: set when any lane of the pixel quad has it"""
        return 'quad_any(%s)' % bits

    def op_s_wqm_b64(self, inst):
        d, a = inst.ops
        if self.stage != 'ps':
            self.unknown['s_wqm_b64 outside the pixel stage'] += 1
            return
        e = self.wqm_expr(self.mask_val(a))
        if d.first == 126:
            self.emit('EXM = %s; SCC = EXM != 0u;' % e)
            return
        self.emit('%s = %s; %s = 0u; SCC = %s != 0u;' % (self.reg(d, 0), e, self.reg(d, 1), self.reg(d, 0)))
        self.set_kind(d, 'mask')

    def ps_export(self, inst, tgt):
        """exp mrt<n> / mrtz / null: the color outputs as Kyty declares them, converted per the target's export
        format (SPI_SHADER_COL_FORMAT); vm: the lane's exec bit is its pixel's valid mask"""
        srcs = inst.ops[1:5]
        compr = bool(inst.mods.get('compr'))
        writes = ''
        if tgt.startswith('mrt') and tgt != 'mrtz':
            n = int(tgt[3:])
            info = self.gfx.interface.get(('out', n, 0)) if self.gfx is not None else None
            mode = self.gfx.target_output_mode[n] if self.gfx is not None and n < len(self.gfx.target_output_mode) \
                else 0
            # Kyty's per-target conversion flags (CB format classes); they matter for the 16-bit exports only (the
            # 32-bit ones are raw bits into Kyty's output type)
            special = compr and any((int(self.gfx.pixel.get(k, '0'), 0) >> n) & 1
                                    for k in ('packed_unorm', 'uint', 'sint', 'round'))
            if info is not None and mode != 0:
                if special or info['array'] is not None or info['type'] not in ('float', 'uint'):
                    self.unknown['exp %s: format mode %d special' % (tgt, mode)] += 1
                    return
                if compr:
                    lo = self.src_u32(srcs[0]) if srcs[0].kind == 'reg' else '0u'
                    hi = self.src_u32(srcs[2]) if srcs[2].kind == 'reg' else '0u'
                    unpack = {4: 'unpackHalf2x16', 5: 'unpackUnorm2x16', 6: 'unpackSnorm2x16'}.get(mode)
                    if info['type'] == 'float' and unpack:
                        val = 'vec4(%s(%s), %s(%s))' % (unpack, lo, unpack, hi)
                    elif info['type'] == 'uint' and mode == 7:
                        val = 'uvec4(%s & 0xffffu, %s >> 16, %s & 0xffffu, %s >> 16)' % (lo, lo, hi, hi)
                    else:
                        self.unknown['exp %s: compressed mode %d into %s' % (tgt, mode, info['type'])] += 1
                        return
                else:
                    comps = [self.src_u32(o) if o.kind == 'reg' else '0u' for o in srcs]
                    if mode not in (1, 2, 9):
                        self.unknown['exp %s: 32-bit export mode %d' % (tgt, mode)] += 1
                        return
                    if mode == 1:
                        comps = [comps[0], '0u', '0u', '0u']
                    elif mode == 2:
                        comps = [comps[0], comps[1], '0u', '0u']
                    val = 'uvec4(%s)' % ', '.join(comps)
                    if info['type'] == 'float':
                        val = 'uintBitsToFloat(%s)' % val
                n_out = info['n']
                sw = 'xyzw'[:n_out]
                self.ps_outputs[n] = info
                writes = 'OUT%d = (%s).%s;' % (n, val, sw) if n_out < 4 else 'OUT%d = %s;' % (n, val)
        elif tgt == 'mrtz':
            if srcs[0].kind != 'reg' or any(o.kind == 'reg' for o in srcs[1:]):
                self.unknown['exp mrtz other than depth'] += 1
                return
            if self.gfx is None or self.gfx.pixel.get('depth_export') != '1':
                self.unknown['exp mrtz without depth_export'] += 1
                return
            self.ps_depth = True
            writes = 'gl_FragDepth = uintBitsToFloat(%s);' % self.src_u32(srcs[0])
        elif tgt != 'null':
            self.unknown['exp ' + tgt] += 1
            return
        text = ''
        if writes:
            text += 'if (EX) { %s } ' % writes
        if inst.mods.get('vm'):
            text += 'PS_VALID = EX; '
        if text:
            self.emit(text.strip())

    def op_s_sendmsg(self, inst):
        text = inst.ops[0].text if inst.ops else ''
        if 'GS_ALLOC_REQ' in text or 'DEALLOC_VGPRS' in text:
            return                   # NGG allocation: Vulkan's input assembler; VGPR release: nothing to do
        self.unknown['s_sendmsg ' + text] += 1

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

    def fma_mix_expr(self, inst):
        d, a, c, e = inst.ops
        op_sel, op_sel_hi = inst.mod_list('op_sel'), inst.mod_list('op_sel_hi')
        srcs = [self.f16_operand(o, op_sel[k], inst) if op_sel_hi[k] else self.src_f32(o, inst)
                for k, o in enumerate((a, c, e))]
        expr = 'fma(%s, %s, %s)' % tuple(srcs)
        return 'clamp(%s, 0.0, 1.0)' % expr if inst.mods.get('clamp') else expr

    def op_v_fma_mixlo_f16(self, inst):
        d = inst.ops[0]
        self.write_v_u32(inst, d, '((%s & 0xffff0000u) | uint(float16BitsToUint16(float16_t(%s))))' % (
            self.reg(d), self.fma_mix_expr(inst)))

    def op_v_fma_mixhi_f16(self, inst):
        d = inst.ops[0]
        self.write_v_u32(inst, d, '((%s & 0xffffu) | (uint(float16BitsToUint16(float16_t(%s))) << 16))' % (
            self.reg(d), self.fma_mix_expr(inst)))

    # --- packed f16 (VOP3P: op_sel picks the half for the low result, op_sel_hi (default 1) for the high one)
    def pk_operand(self, o, inst, k, lane):
        op_sel = inst.mod_list('op_sel')
        op_sel_hi = inst.mod_list('op_sel_hi') if 'op_sel_hi' in inst.mods else [1, 1, 1]
        neg = inst.mod_list('neg_lo' if lane == 0 else 'neg_hi')
        half = op_sel[k] if lane == 0 else op_sel_hi[k]
        if o.kind == 'const':
            if o.text in gcnisa.INLINE_FLOATS:
                e = 'float16_t(%r)' % struct.unpack('<f', struct.pack('<I', gcnisa.INLINE_FLOATS[o.text]))[0]
            elif getattr(o, 'literal', False) or o.text.startswith('0x'):
                e = 'uint16BitsToHalf(uint16_t(0x%04xu))' % ((o.value >> (16 * half)) & 0xffff)
            else:
                e = 'uint16BitsToHalf(uint16_t(0x%04xu))' % (o.value & 0xffff)   # inline integer: both halves
        else:
            e = 'unpackFloat2x16(%s).%s' % (self.src_u32(o), 'y' if half else 'x')
        if neg[k] if k < len(neg) else 0:
            e = '(-%s)' % e
        return e

    def pk_result(self, inst, d, lo, hi):
        if inst.mods.get('clamp'):
            lo, hi = 'clamp(%s, float16_t(0.0), float16_t(1.0))' % lo, 'clamp(%s, float16_t(0.0), float16_t(1.0))' % hi
        self.write_v_u32(inst, d, 'packFloat2x16(f16vec2(%s, %s))' % (lo, hi))

    def op_v_pk_fma_f16(self, inst):
        d, a, c, e = inst.ops
        lanes = [tuple(self.pk_operand(o, inst, k, lane) for k, o in enumerate((a, c, e))) for lane in (0, 1)]
        self.pk_result(inst, d, 'fma(%s, %s, %s)' % lanes[0], 'fma(%s, %s, %s)' % lanes[1])

    def pk2(self, inst, fmt):
        d, a, c = inst.ops
        lanes = [tuple(self.pk_operand(o, inst, k, lane) for k, o in enumerate((a, c))) for lane in (0, 1)]
        self.pk_result(inst, d, fmt % lanes[0], fmt % lanes[1])

    def op_v_pk_mul_f16(self, inst):
        self.pk2(inst, '(%s * %s)')

    def op_v_pk_add_f16(self, inst):
        self.pk2(inst, '(%s + %s)')

    def op_v_pk_min_f16(self, inst):
        self.pk2(inst, 'min(%s, %s)')

    def op_v_pk_max_f16(self, inst):
        self.pk2(inst, 'max(%s, %s)')

    def op_v_pk_fmac_f16(self, inst):
        d, a, c = inst.ops
        self.write_v_u32(inst, d, 'packFloat2x16(fma(unpackFloat2x16(%s), unpackFloat2x16(%s), unpackFloat2x16(%s)))' % (
            self.src_u32(a), self.src_u32(c), self.reg(d)))

    def op_v_cvt_pkrtz_f16_f32(self, inst):
        d, a, c = inst.ops
        self.write_v_u32(inst, d, '(f2h_rtz(%s) | (f2h_rtz(%s) << 16))' % (self.src_f32(a, inst, 0),
                                                                        self.src_f32(c, inst, 1)))

    def op_v_cvt_f16_f32(self, inst):
        d, a = inst.ops
        self.write_v_u32(inst, d, 'uint(float16BitsToUint16(float16_t(%s)))' % self.src_f32(a, inst, 0))

    def op_v_cvt_i32_f32(self, inst):
        d, a = inst.ops
        self.write_v_u32(inst, d, 'uint(cvt_i32_f32(%s))' % self.src_f32(a, inst, 0))

    def op_v_cvt_flr_i32_f32(self, inst):
        d, a = inst.ops
        self.write_v_u32(inst, d, 'uint(cvt_flr_i32_f32(%s))' % self.src_f32(a, inst, 0))

    def op_v_cvt_f32_u32(self, inst):
        d, a = inst.ops
        self.write_v_f32(inst, d, 'float(%s)' % self.src_int(a, inst, 0))

    def op_v_cvt_f32_ubyte1(self, inst):
        d, a = inst.ops
        self.write_v_f32(inst, d, 'float((%s >> 8) & 0xffu)' % self.src_u32(a))

    def op_v_cvt_f32_ubyte2(self, inst):
        d, a = inst.ops
        self.write_v_f32(inst, d, 'float((%s >> 16) & 0xffu)' % self.src_u32(a))

    def op_v_cvt_f32_ubyte3(self, inst):
        d, a = inst.ops
        self.write_v_f32(inst, d, 'float(%s >> 24)' % self.src_u32(a))

    def op_v_bfi_b32(self, inst):
        d, a, c, e = inst.ops
        x, y, z = self.src_int(a, inst, 0), self.src_int(c, inst, 1), self.src_int(e, inst, 2)
        self.write_v_u32(inst, d, '((%s & %s) | (~%s & %s))' % (x, y, x, z))

    def op_v_bfe_u32(self, inst):
        d, a, c, e = inst.ops
        self.write_v_u32(inst, d, 'bfe_u32(%s, %s, %s)' % (self.src_int(a, inst, 0), self.src_int(c, inst, 1),
                                                         self.src_int(e, inst, 2)))

    def op_v_bfe_i32(self, inst):
        d, a, c, e = inst.ops
        self.write_v_u32(inst, d, 'bfe_i32(%s, %s, %s)' % (self.src_int(a, inst, 0), self.src_int(c, inst, 1),
                                                         self.src_int(e, inst, 2)))

    def op_v_min3_f32(self, inst):
        d, a, c, e = inst.ops
        self.write_v_f32(inst, d, 'min(min(%s, %s), %s)' % (self.src_f32(a, inst), self.src_f32(c, inst),
                                                           self.src_f32(e, inst)))

    def op_v_max3_f32(self, inst):
        d, a, c, e = inst.ops
        self.write_v_f32(inst, d, 'max(max(%s, %s), %s)' % (self.src_f32(a, inst), self.src_f32(c, inst),
                                                           self.src_f32(e, inst)))

    def op_v_med3_i32(self, inst):
        d, a, c, e = inst.ops
        x, y, z = ('int(%s)' % self.src_int(o, inst, k) for k, o in enumerate((a, c, e)))
        self.write_v_u32(inst, d, 'uint(max(min(%s, %s), min(max(%s, %s), %s)))' % (x, y, x, y, z))

    def op_v_fmaak_f32(self, inst):
        d, a, c, k = inst.ops
        self.write_v_f32(inst, d, 'fma(%s, %s, %s)' % (self.src_f32(a, inst), self.src_f32(c, inst), f32const(k.value)))

    def op_v_fmamk_f32(self, inst):
        d, a, k, c = inst.ops
        self.write_v_f32(inst, d, 'fma(%s, %s, %s)' % (self.src_f32(a, inst), f32const(k.value), self.src_f32(c, inst)))

    def cube(self, inst, fn):
        d, x, y, z = inst.ops
        self.write_v_f32(inst, d, '%s(%s, %s, %s)' % (fn, self.src_f32(x, inst), self.src_f32(y, inst),
                                                      self.src_f32(z, inst)))

    def op_v_cubeid_f32(self, inst):
        self.cube(inst, 'cube_id')

    def op_v_cubesc_f32(self, inst):
        self.cube(inst, 'cube_sc')

    def op_v_cubetc_f32(self, inst):
        self.cube(inst, 'cube_tc')

    def op_v_cubema_f32(self, inst):
        self.cube(inst, 'cube_ma')

    def permlane(self, inst, cross):
        """v_permlane16 / v_permlanex16: lane i of each 16-lane row reads lane sel[i] of its own row (x16: of the other
        row of its 32); the shuffle runs for every thread (outside the exec guard)"""
        d, src, lo, hi = inst.ops
        sel = '((%s < 8u ? (%s >> ((%s) * 4u)) : (%s >> ((%s - 8u) * 4u))) & 15u)' % (
            '(LANE & 15u)', self.sval(lo), '(LANE & 15u)', self.sval(hi), '(LANE & 15u)')
        row = '((LANE & 16u) ^ 16u)' if cross else '(LANE & 16u)'
        self.emit('{ uint pl_ = subgroupShuffle(%s, %s | %s); if (EX) %s = pl_; }' % (self.reg(src), row, sel, self.reg(d)),
                  valu=True)
        self.wave_points.add(inst.off)

    def op_v_permlane16_b32(self, inst):
        self.permlane(inst, False)

    def op_v_madak_f32(self, inst):
        d, a, c, k = inst.ops
        self.write_v_f32(inst, d, 'fma_unfused(%s, %s, %s)' % (self.src_f32(a, inst), self.src_f32(c, inst),
                                                               f32const(k.value)))

    # --- scalar
    def op_s_lshr_b32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = %s >> (%s & 31u); SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_ashr_i32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = uint(int(%s) >> (%s & 31u)); SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.sval(c),
                                                                        self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_pack_ll_b32_b16(self, inst):
        d, a, c = inst.ops
        self.emit('%s = (%s & 0xffffu) | (%s << 16);' % (self.reg(d), self.sval(a), self.sval(c)))
        self.set_kind(d, 'value')

    def op_s_mulk_i32(self, inst):
        d, k = inst.ops
        v = k.value & 0xffff
        v = v - 0x10000 if v & 0x8000 else v
        self.emit('%s = uint(int(%s) * %d);' % (self.reg(d), self.reg(d), v))
        self.set_kind(d, 'value')

    def op_s_bfe_i32(self, inst):
        d, a, c = inst.ops
        self.emit('{ uint b_ = %s; %s = bfe_i32(%s, b_ & 31u, (b_ >> 16) & 0x7fu); SCC = %s != 0u; }' % (
            self.sval(c), self.reg(d), self.sval(a), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_bfe_u32(self, inst):
        d, a, c = inst.ops
        self.emit('{ uint b_ = %s; %s = bfe_u32(%s, b_ & 31u, (b_ >> 16) & 0x7fu); SCC = %s != 0u; }' % (
            self.sval(c), self.reg(d), self.sval(a), self.reg(d)))
        self.set_kind(d, 'value')

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
        if a.neg or a.abs:
            # neg / abs on a move (SDWA): float modifiers, the sign bit
            plain = gcnisa.Operand(a.text.replace('-', '', 1).strip('|') if not a.text.startswith('0x') else a.text)
            bits = self.src_int(plain, inst, 0)
            if a.abs:
                bits = '(%s & 0x7fffffffu)' % bits
            if a.neg:
                bits = '(%s ^ 0x80000000u)' % bits
            self.write_v_u32(inst, d, bits)
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
            self.permlane(inst, True)
            return
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

    def code_table_name(self, start, num):
        """a module-scope const array of the code's dwords [start, start + num) (code_tables)"""
        if not hasattr(self, 'tables_decl'):
            self.tables_decl = {}
        name = 'CT_%x' % start
        if name not in self.tables_decl:
            words = struct.unpack('<%dI' % ((num + 3) // 4), self.code_bytes[start:start + ((num + 3) // 4) * 4].ljust(
                ((num + 3) // 4) * 4, b'\0'))
            self.tables_decl[name] = 'const uint %s[%d] = uint[](%s);' % (name, len(words), ', '.join(
                '0x%xu' % w for w in words))
        return name

    def op_dpp_int(self, inst, op):
        """v_op_dpp d, a, b row_shr:n with an integer op: lane i combines a from lane i-n of its 16-lane row with b of
        lane i; lanes whose source is outside the row keep d (bound_ctrl off)"""
        d, a, c = inst.ops
        shr = int(inst.mods.get('row_shr', '0'))
        if (not shr or inst.mods.get('row_mask', '0xf') != '0xf' or inst.mods.get('bank_mask', '0xf') != '0xf' or
                inst.mods.get('bound_ctrl')):
            self.unknown['dpp form %s' % inst.text] += 1
        self.emit('{ uint src_ = subgroupShuffleUp(%s, %du); bool ok_ = (gl_SubgroupInvocationID & 15u) >= %du; '
                  'if (EX && ok_) %s = src_ %s %s; }' % (self.src_u32(a), shr, shr, self.reg(d), op, self.src_u32(c)))
        self.wave_points.add(inst.off)

    def op_v_or_b32(self, inst):
        if inst.encoding == 'dpp':
            return self.op_dpp_int(inst, '|')
        d, a, c = inst.ops
        self.write_v_u32(inst, d, '(%s | %s)' % (self.src_int(a, inst, 0), self.src_int(c, inst, 1)))

    def op_v_fmac_f32(self, inst):
        d, a, c = inst.ops
        self.write_v_f32(inst, d, 'fma(%s, %s, uintBitsToFloat(%s))' % (
            self.src_f32(a, inst, 0), self.src_f32(c, inst, 1), self.reg(d)))

    def op_v_cvt_rpi_i32_f32(self, inst):
        d, a = inst.ops
        self.write_v_u32(inst, d, 'uint(cvt_i32_f32(floor(%s + 0.5)))' % self.src_f32(a, inst, 0))

    def op_v_cvt_off_f32_i4(self, inst):
        d, a = inst.ops
        self.write_v_f32(inst, d, '(float(bitfieldExtract(int(%s), 0, 4)) * 0.0625)' % self.src_u32(a))

    def op_v_writelane_b32(self, inst):
        """v_writelane_b32 d, s, lane: lane `lane` of d gets s (exec is ignored); lane pairs: lane l is invocation l & 31,
        half l >> 5"""
        d, a, lane = inst.ops
        l = self.src_u32(lane)
        self.emit('{ uint l_ = %s & 63u; if (gl_SubgroupInvocationID == (l_ & 31u)) { if (l_ < 32u) %s = %s; else %s = %s; } }'
                  % (l, self.vname(d, 0, 0), self.src_u32(a), self.vname(d, 0, 1), self.src_u32(a)))
        self.wave_points.add(inst.off)

    def op_v_fma_f32(self, inst):
        d, a, c, e = inst.ops
        self.write_v_f32(inst, d, 'fma(%s, %s, %s)' % (self.src_f32(a, inst), self.src_f32(c, inst),
                                                       self.src_f32(e, inst)))

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
            return '(%s & LMASK)' % self.reg(o, 0)
        if o.kind == 'const':
            lo = o.value & 0xffffffff
            hi = 0xffffffff if o.text.startswith('-') else 0
            if lo == 0xffffffff and hi == 0xffffffff:
                return 'LMASK'
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
                self.emit('{ uint r_ = (%s) & LMASK; EXM = r_; SCC = r_ != 0u; }' % r)
            else:
                self.emit('{ uint r_ = (%s) & LMASK; %s = r_; %s = 0u; SCC = r_ != 0u; }' % (r, self.reg(d, 0),
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

    def op_s_nand_b64(self, inst):
        self.logic64(inst, lambda x, y: '~(%s & %s)' % (x, y), lambda x, y: '~(%s & %s)' % (x, y))

    def op_s_xnor_b64(self, inst):
        self.logic64(inst, lambda x, y: '~(%s ^ %s)' % (x, y), lambda x, y: '~(%s ^ %s)' % (x, y))

    def op_s_nor_b64(self, inst):
        self.logic64(inst, lambda x, y: '~(%s | %s)' % (x, y), lambda x, y: '~(%s | %s)' % (x, y))

    def op_s_not_b64(self, inst):
        d, a = inst.ops
        if a.kind == 'reg' and (a.first == 126 or self.mask_kind.get(a.first) == 'mask'):
            self.emit('%s = ~%s & LMASK; %s = 0u; SCC = %s != 0u;' % (self.reg(d, 0), self.mask_val(a),
                                                                  self.reg(d, 1), self.reg(d, 0)))
            self.set_kind(d, 'mask')
            return
        self.emit('%s = ~%s; %s = ~%s; SCC = (%s | %s) != 0u;' % (self.reg(d, 0), self.reg(a, 0), self.reg(d, 1),
                                                                  self.reg(a, 1), self.reg(d, 0), self.reg(d, 1)))
        self.set_kind(d, 'value')

    def saveexec(self, inst, fn):
        d, a = inst.ops
        self.emit('{ uint old_ = EXM; %s = old_; %s = 0u; EXM = (%s) & LMASK; SCC = EXM != 0u; }' % (
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

    def op_s_or_b32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = %s | %s; SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_xor_b32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = %s ^ %s; SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_min_u32(self, inst):
        self.salu2(inst, '(%s < %s ? %s : %s)' % ('%s', '%s', self.sval(inst.ops[1]), self.sval(inst.ops[2])),
                   '%s < %s' % (self.sval(inst.ops[1]), self.sval(inst.ops[2])))

    def op_s_max_u32(self, inst):
        self.salu2(inst, '(%s > %s ? %s : %s)' % ('%s', '%s', self.sval(inst.ops[1]), self.sval(inst.ops[2])),
                   '%s > %s' % (self.sval(inst.ops[1]), self.sval(inst.ops[2])))

    def movrel_regs(self, first):
        """the VGPRs a relative (M0-indexed) access from `first` may reach: up to the program's highest VGPR"""
        regs = list(range(first, max(self.vgpr_limit, first + 1)))
        for n in regs:
            self.used_v.add(n)
        return regs

    def op_v_movrels_b32(self, inst):
        # D = VGPR[S + M0]
        d, a = inst.ops[:2]
        cases = ' '.join('case %du: r_ = v%d_%d; break;' % (n, n, self.h) for n in self.movrel_regs(a.first))
        self.emit('if (EX) { uint r_ = 0u; switch (%du + s124) { %s default: break; } %s = r_; }' % (
            a.first, cases, self.reg(d)))
        self.used_s.add(124)

    def op_v_movreld_b32(self, inst):
        # VGPR[D + M0] = S
        d, a = inst.ops[:2]
        x = self.src_u32(a)
        cases = ' '.join('case %du: v%d_%d = x_; break;' % (n, n, self.h) for n in self.movrel_regs(d.first))
        self.emit('if (EX) { uint x_ = %s; switch (%du + s124) { %s default: break; } }' % (x, d.first, cases))
        self.used_s.add(124)

    def op_v_movrelsd_b32(self, inst):
        # VGPR[D + M0] = VGPR[S + M0]
        d, a = inst.ops[:2]
        reads = ' '.join('case %du: r_ = v%d_%d; break;' % (n, n, self.h) for n in self.movrel_regs(a.first))
        writes = ' '.join('case %du: v%d_%d = r_; break;' % (n, n, self.h) for n in self.movrel_regs(d.first))
        self.emit('if (EX) { uint r_ = 0u; switch (%du + s124) { %s default: break; } switch (%du + s124) { %s '
                  'default: break; } }' % (a.first, reads, d.first, writes))
        self.used_s.add(124)

    def vertex_input(self, inst, d):
        """an embedded vertex fetch as Kyty's vertex input (Translate TranslateEmbeddedFetch): component c of the result
        is the attribute's component (dst_sel - 4) % the format's components, or 0 / 1 for dst_sel 0 / 1 (the V# Kyty
        built the pipeline with, from the record's fetch_resource line); False when the record does not cover it"""
        a = self.fetch_map[inst.off]
        g = self.gfx
        res = next((r for r in g.fetch_resources if r.get('attr') == str(a) and r.get('vsharp')), None)
        inp = next((x for x in g.inputs if x.get('name') == 'in_attr_%d' % a), None)
        if res is None or inp is None or inst.mods.get('d16'):
            return False
        loc = int(inp['location'])
        info = g.interface.get(('in', loc, 0))
        if info is None or info['array'] is not None:
            return False
        w3 = res['vsharp'][3]
        fmt = (w3 >> 12) & 0x7f
        entry = prelude.format_table()[fmt] if fmt < len(prelude.format_table()) else 0
        ncomp = (entry & 7) if entry else (3 if 30 <= fmt <= 43 else (4 if 44 <= fmt <= 55 else 0))
        ftype = (entry >> 9) if entry else (0 if fmt in (36, 43) else ((fmt - 30) % 7 + 1 if fmt <= 43 else
                                                                     (fmt - 44) % 6 + 1))
        integer = ftype in (5, 6)
        if ncomp == 0:
            return False
        comps = len(inst.base.rsplit('_', 1)[1])
        writes = []
        for c in range(comps):
            sel = (w3 >> (3 * c)) & 7
            if sel == 0:
                e = '0u'
            elif sel == 1:
                e = '1u' if integer else '0x3f800000u'
            elif 4 <= sel <= 7:
                m = (sel - 4) % ncomp
                if m >= info['n']:
                    return False
                comp = 'ATTR%d' % a if info['n'] == 1 else 'ATTR%d.%s' % (a, 'xyzw'[m])
                e = 'floatBitsToUint(%s)' % comp if info['type'] == 'float' else 'uint(%s)' % comp
            else:
                return False
            writes.append('%s = %s;' % (self.reg(d, c), e))
        self.vs_inputs[a] = (loc, info)
        self.emit('if (EX) { %s }' % ' '.join(writes))
        return True

    def op_v_bfrev_b32(self, inst):
        d, a = inst.ops[:2]
        self.write_v_u32(inst, d, 'bitfieldReverse(%s)' % self.src_int(a, inst, 0))

    def op_v_sad_u32(self, inst):
        d, a, c, e = inst.ops[:4]
        x, y = self.src_int(a, inst, 0), self.src_int(c, inst, 1)
        self.write_v_u32(inst, d, '((max(%s, %s) - min(%s, %s)) + %s)' % (x, y, x, y, self.src_int(e, inst, 2)))

    def op_s_not_b32(self, inst):
        d, a = inst.ops
        self.emit('%s = ~%s; SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_nor_b32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = ~(%s | %s); SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_nand_b32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = ~(%s & %s); SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_andn2_b32(self, inst):
        d, a, c = inst.ops
        self.emit('%s = %s & ~%s; SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_bfm_b64(self, inst):
        """s_bfm_b64 d, a, c: d = ((1 << a[5:0]) - 1) << c[5:0] (64 bits)"""
        d, a, c = inst.ops
        self.emit('{ uint64_t m_ = ((1ul << uint64_t(%s & 63u)) - 1ul) << uint64_t(%s & 63u); %s = uint(m_); %s = uint(m_ >> 32); }'
                  % (self.sval(a), self.sval(c), self.reg(d, 0), self.reg(d, 1)))
        self.set_kind(d, 'value')

    def op_s_min_i32(self, inst):
        self.salu2(inst, '(int(%s) < int(%s) ? %s : %s)' % ('%s', '%s', self.sval(inst.ops[1]), self.sval(inst.ops[2])),
                   'int(%s) < int(%s)' % (self.sval(inst.ops[1]), self.sval(inst.ops[2])))

    def op_s_max_i32(self, inst):
        self.salu2(inst, '(int(%s) > int(%s) ? %s : %s)' % ('%s', '%s', self.sval(inst.ops[1]), self.sval(inst.ops[2])),
                   'int(%s) > int(%s)' % (self.sval(inst.ops[1]), self.sval(inst.ops[2])))

    def op_s_abs_i32(self, inst):
        d, a = inst.ops
        self.emit('%s = uint(abs(int(%s))); SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.reg(d)))
        self.set_kind(d, 'value')

    def lshl_add(self, inst, n):
        """s_lshl<n>_add_u32: d = (a << n) + c, SCC = the carry out of 32 bits"""
        d, a, c = inst.ops
        self.emit('{ uint64_t r_ = (uint64_t(%s) << %dul) + uint64_t(%s); SCC = (r_ >> 32) != 0ul; %s = uint(r_); }' % (
            self.sval(a), n, self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_lshl1_add_u32(self, inst):
        self.lshl_add(inst, 1)

    def op_s_lshl2_add_u32(self, inst):
        self.lshl_add(inst, 2)

    def op_s_lshl3_add_u32(self, inst):
        self.lshl_add(inst, 3)

    def op_s_lshl4_add_u32(self, inst):
        self.lshl_add(inst, 4)

    def op_s_getpc_b64(self, inst):
        """the program counter: only as the base of constant tables in the code, which the translation embeds
        (code_tables); the pair itself is never read otherwise"""
        d = inst.ops[0]
        if inst.off not in getattr(self, 'getpc_ok', set()):
            self.unknown['s_getpc_b64 (other than constant tables)'] += 1
        self.emit('%s = 0u; %s = 0u;' % (self.reg(d, 0), self.reg(d, 1)))
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

    def write_s64(self, d, expr64):
        """a 64-bit scalar result (uint64_t expression); exec takes the thread's lane bits of it"""
        if d.kind == 'reg' and d.first == 126:
            self.emit('{ uint64_t t_ = %s; EXM = lane_bits2(uint(t_), uint(t_ >> 32)); SCC = t_ != 0ul; }' % expr64)
            return
        self.emit('{ uint64_t t_ = %s; %s = uint(t_); %s = uint(t_ >> 32); SCC = t_ != 0ul; }' % (
            expr64, self.reg(d, 0), self.reg(d, 1)))
        self.set_kind(d, 'value')

    def s64(self, o):
        """a 64-bit scalar source as uint64_t (inline constants sign-extend)"""
        if o.kind == 'const':
            hi = 0xffffffff if o.text.startswith('-') else 0
            return '0x%08x%08xul' % (hi, o.value & 0xffffffff)
        if o.kind == 'reg' and o.first == 126:
            raise ValueError('exec as a 64-bit value source')
        return 'packUint2x32(uvec2(%s, %s))' % (self.sval(o, 0), self.sval(o, 1))

    def op_s_lshr_b64(self, inst):
        d, a, c = inst.ops
        self.write_s64(d, '(%s >> (%s & 63u))' % (self.s64(a), self.sval(c)))

    def op_s_bfe_u64(self, inst):
        d, a, c = inst.ops
        # src1: offset [5:0], width [22:16]
        self.write_s64(d, 'bfe_u64(%s, %s)' % (self.s64(a), self.sval(c)))

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

    def lane_mask_operand(self, o):
        """an SGPR (or exec) operand that holds a lane mask: (expression of the thread's two bits, word 0/1)"""
        if o.kind != 'reg' or o.file != 's':
            return None
        even = o.first & ~1
        if even == 126:
            return 'EXM', o.first & 1
        if self.mask_kind.get(even) == 'mask':
            return '(%s & LMASK)' % ('s%d' % even), o.first & 1
        return None

    def mbcnt_ballot(self, inst):
        """v_mbcnt with a lane mask operand counts the wave's bits: the ballot runs for every thread (scalar
        statement before the per-half ones)"""
        m = self.lane_mask_operand(inst.ops[1])
        if m is None:
            return
        self.h = None
        self.ballot_uses.add(inst.off)
        self.emit('uvec2 mbm_%x_ = wave_ballot2((%s & 1u) != 0u, (%s & 2u) != 0u);' % (inst.off, m[0], m[0]))

    def mbcnt_word(self, inst, o, word):
        m = self.lane_mask_operand(o)
        if m is not None:
            return 'mbm_%x_.%s' % (inst.off, 'xy'[m[1]])
        if o.kind == 'const':
            v = o.value & 0xffffffff
            if word == 1 and o.text.startswith('-'):
                v = 0xffffffff
            return '0x%08xu' % v
        return self.src_int(o, inst, 0)

    def op_v_mbcnt_lo_u32_b32(self, inst):
        d, m, c = inst.ops
        self.write_v_u32(inst, d, '(uint(bitCount(%s & (LANE < 32u ? ((1u << LANE) - 1u) : 0xffffffffu))) + %s)' % (
            self.mbcnt_word(inst, m, 0), self.src_int(c, inst, 1)))

    def op_v_mbcnt_hi_u32_b32(self, inst):
        d, m, c = inst.ops
        self.write_v_u32(inst, d, '(uint(bitCount(%s & (LANE < 32u ? 0u : ((1u << (LANE - 32u)) - 1u)))) + %s)' % (
            self.mbcnt_word(inst, m, 1), self.src_int(c, inst, 1)))

    def op_s_bcnt1_i32_b64(self, inst):
        d, a = inst.ops
        m = self.lane_mask_operand(a)
        if m is not None:
            self.ballot_uses.add(inst.off)
            self.emit('{ uvec2 b_ = wave_ballot2((%s & 1u) != 0u, (%s & 2u) != 0u); %s = uint(bitCount(b_.x) + '
                      'bitCount(b_.y)); SCC = %s != 0u; }' % (m[0], m[0], self.reg(d), self.reg(d)))
        else:
            hi = self.sval(a, 1) if a.kind == 'reg' else ('0xffffffffu' if a.text.startswith('-') else '0u')
            self.emit('%s = uint(bitCount(%s) + bitCount(%s)); SCC = %s != 0u;' % (
                self.reg(d), self.sval(a, 0), hi, self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_bcnt1_i32_b32(self, inst):
        d, a = inst.ops
        self.emit('%s = uint(bitCount(%s)); SCC = %s != 0u;' % (self.reg(d), self.sval(a), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_brev_b32(self, inst):
        d, a = inst.ops
        self.emit('%s = bitfieldReverse(%s);' % (self.reg(d), self.sval(a)))
        self.set_kind(d, 'value')

    def op_s_bitcmp0_b32(self, inst):
        a, c = inst.ops
        self.emit('SCC = ((%s >> (%s & 31u)) & 1u) == 0u;' % (self.sval(a), self.sval(c)))

    def op_s_add_u32(self, inst):
        d, a, c = inst.ops
        self.emit('{ uint x_ = %s, y_ = %s; %s = x_ + y_; SCC = uint64_t(x_) + uint64_t(y_) > 0xfffffffful; }' % (
            self.sval(a), self.sval(c), self.reg(d)))
        self.set_kind(d, 'value')

    def op_s_addc_u32(self, inst):
        d, a, c = inst.ops
        self.emit('{ uint x_ = %s, y_ = %s, c_ = SCC ? 1u : 0u; %s = x_ + y_ + c_; '
                  'SCC = uint64_t(x_) + uint64_t(y_) + uint64_t(c_) > 0xfffffffful; }' % (
                      self.sval(a), self.sval(c), self.reg(d)))
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

    def op_scmpk(self, inst):
        """s_cmpk_<rel>_<type> s, simm16: SCC = s rel the 16-bit constant (sign-extended for i32, zero for u32)"""
        m = re.match(r's_cmpk_(eq|lg|lt|le|gt|ge)_(u32|i32)$', inst.base)
        if m is None:
            self.unknown[inst.op] += 1
            return
        a, k = inst.ops
        v = k.value & 0xffff
        if m.group(2) == 'i32':
            v = v - 0x10000 if v & 0x8000 else v
            x, y = 'int(%s)' % self.sval(a), '%d' % v
        else:
            x, y = self.sval(a), '%du' % v
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
        ga = '%s + %s' % (addr, offset)
        if self.stage != 'cs':
            # graphics: one guest lane per invocation, so the scalar address is the same in every invocation of the
            # subgroup (one draw's user data); saying so lets the compiler use uniform loads
            ga = 'uniform64(%s)' % ga
        xl = ('sxlate_k(%du, %s, %du)' % (node, ga, 4 * n) if node is not None else 'xlate(%s)' % ga)
        loads = ' '.join('%s = sld32(d_ == 0ul ? 0ul : d_ + %dul);' % (self.reg(d, k), 4 * k) for k in range(n))
        if self.images is not None and inst.off in self.images.heap_loads:
            loads += ' TADDR_%x = %s + %s;' % (inst.off, addr, offset)
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
        if self.images is not None and inst.off in self.images.heap_loads:
            loads += ' TADDR_%x = packUint2x32(uvec2(%s, %s & 0xffffu)) + uint64_t(o_);' % (inst.off, w[0], w[1])
        if self.stage != 'cs':
            # graphics: the V# and the offset are the same in every invocation of the subgroup (see op_sload)
            w = ['subgroupBroadcastFirst(%s)' % x for x in w]
            soff = 'subgroupBroadcastFirst(%s)' % soff
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
        w = [self.reg(vs, k) for k in range(4)]
        if self.stage != 'cs':
            # graphics: the V# and the scalar offset are the same in every invocation of the subgroup (op_sload); the
            # descriptor's translation through the page table becomes a uniform load
            w = ['subgroupBroadcastFirst(%s)' % x for x in w]
            if soff.kind == 'reg':
                so = 'subgroupBroadcastFirst(%s)' % so
        return '%s, %s, %s, %s, %s, %s + %s + %du' % (w[0], w[1], w[2], w[3], idx, voff, so, imm)

    def op_bufload(self, inst):
        d, vaddr, vs, soff = inst.ops
        b = inst.base
        table = getattr(self, 'code_tables', {}).get(inst.off)
        if table is not None:
            # a constant table in the code (code_tables): embedded, bounds as the raw buffer's (stride 0)
            start, num = table
            name = self.code_table_name(start, num)
            imm = int(inst.mods.get('offset', '0'), 0)
            voff = self.reg(vaddr, 0) if inst.mods.get('offen') else '0u'
            so = '0u' if soff.kind in ('const', 'null') and soff.value == 0 else self.src_u32(soff)
            self.emit('if (EX) { uint o_ = %s + %s + %du; %s }' % (voff, so, imm, ' '.join(
                '%s = o_ + %du < %du ? %s[(o_ >> 2) + %du] : 0u;' % (self.reg(d, k), 4 * k + 3, num, name, k)
                for k in range(d.count))))
            return
        a = self.mubuf_addr(inst, vaddr, vs, soff)
        if b.startswith('buffer_load_dword'):
            load = 'buf_load32_glc' if inst.mods.get('glc') or inst.mods.get('dlc') else 'buf_load32'
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); %s }' % (a, ' '.join(
                '%s = %s(ba_, %d);' % (self.reg(d, k), load, 4 * k) for k in range(d.count))))
        elif b.startswith('buffer_load_format_') and inst.off in self.fetch_map and self.vertex_input(inst, d):
            pass
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
        elif b == 'buffer_load_ushort':
            self.emit('if (EX) %s = buf_load16(buf_addr(%s));' % (self.reg(d), a))
        elif b == 'buffer_load_ubyte':
            self.emit('if (EX) %s = buf_load8_at(buf_addr(%s), 0);' % (self.reg(d), a))
        elif b == 'buffer_load_sbyte':
            self.emit('if (EX) %s = uint(bitfieldExtract(int(buf_load8_at(buf_addr(%s), 0)), 0, 8));' % (self.reg(d), a))
        else:
            self.unknown[inst.op] += 1
            self.emit('/* UNHANDLED %s */' % inst.text)

    def op_bufstore(self, inst):
        data, vaddr, vs, soff = inst.ops
        b = inst.base
        a = self.mubuf_addr(inst, vaddr, vs, soff)
        if b.startswith('buffer_store_format_'):
            comps = len(b.rsplit('_', 1)[1])
            regs = [self.reg(data, k) if k < comps else '0u' for k in range(4)]
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); buf_store_format(ba_, uvec4(%s), %du); }' % (
                a, ', '.join(regs), comps))
            return
        if b.startswith('buffer_store_dword'):
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); %s }' % (a, ' '.join(
                'buf_store32(ba_, %d, %s);' % (4 * k, self.reg(data, k)) for k in range(data.count))))
        elif b == 'buffer_store_short':
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); buf_store16(ba_, %s & 0xffffu); }' % (
                a, self.reg(data)))
        elif b == 'buffer_store_short_d16_hi':
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); buf_store16(ba_, %s >> 16); }' % (
                a, self.reg(data)))
        elif b == 'buffer_store_byte':
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); buf_store8(ba_, %s & 0xffu); }' % (
                a, self.reg(data)))
        elif b == 'buffer_store_byte_d16_hi':
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); buf_store8(ba_, (%s >> 16) & 0xffu); }' % (
                a, self.reg(data)))
        else:
            self.unknown[inst.op] += 1

    def op_bufatomic(self, inst):
        """buffer_atomic_<op> vdata, vaddr, V#, soffset [glc]: per lane, on the buffer's device memory; glc returns the
        old value in vdata"""
        data, vaddr, vs, soff = inst.ops
        b = inst.base
        fn = {'buffer_atomic_add': 'atomicAdd', 'buffer_atomic_sub': None, 'buffer_atomic_umin': 'atomicMin',
              'buffer_atomic_umax': 'atomicMax', 'buffer_atomic_and': 'atomicAnd', 'buffer_atomic_or': 'atomicOr',
              'buffer_atomic_xor': 'atomicXor', 'buffer_atomic_swap': 'atomicExchange'}.get(b)
        if b in ('buffer_atomic_fmin', 'buffer_atomic_fmax') and data.count == 1:
            # float min / max (IEEE minNum / maxNum: a NaN operand loses) by compare-swap; the first read may be stale,
            # the swap then fails and returns the current value
            a = self.mubuf_addr(inst, vaddr, vs, soff)
            op = 'fmin_ieee' if b.endswith('fmin') else 'fmax_ieee'
            ret = ('%s = o_; ' % self.reg(data)) if inst.mods.get('glc') else ''
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); uint o_ = 0u; if (ba_.ok) { o_ = C32(ba_.dev).v; '
                      'for (uint g_ = 0u; g_ < LOOP_LIMIT; g_++) { uint n_ = floatBitsToUint(%s(uintBitsToFloat(o_), '
                      'uintBitsToFloat(%s))); uint p_ = atomicCompSwap(W32(ba_.dev).v, o_, n_); if (p_ == o_) break; o_ = p_; } '
                      '} %s}' % (a, op, self.reg(data), ret))
            return
        if b == 'buffer_atomic_cmpswap' and data.count == 2:
            # data[0]: the value to store, data[1]: the value to compare with; glc: the old value into data[0]
            a = self.mubuf_addr(inst, vaddr, vs, soff)
            # out of range (or a page the cache does not hold): no swap, and glc returns 0 as the hardware does - a
            # compare-swap retry loop then sees 0 == its 0 load and ends instead of spinning to LOOP_LIMIT
            if inst.mods.get('glc'):
                self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); %s = ba_.ok ? atomicCompSwap(W32(ba_.dev).v, '
                          '%s, %s) : 0u; }' % (a, self.reg(data, 0), self.reg(data, 1), self.reg(data, 0)))
            else:
                self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); if (ba_.ok) atomicCompSwap(W32(ba_.dev).v, '
                          '%s, %s); }' % (a, self.reg(data, 1), self.reg(data, 0)))
            return
        if fn is None or data.count != 1:
            self.unknown[inst.op] += 1
            self.emit('/* UNHANDLED %s */' % inst.text)
            return
        a = self.mubuf_addr(inst, vaddr, vs, soff)
        if inst.mods.get('glc'):     # out of range: no operation, glc returns 0
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); %s = ba_.ok ? %s(W32(ba_.dev).v, %s) : 0u; }' % (
                a, self.reg(data), fn, self.reg(data)))
        else:
            self.emit('if (EX) { BufAddr ba_ = buf_addr(%s); probe_store(ba_); if (ba_.ok) %s(W32(ba_.dev).v, %s); }' % (
                a, fn, self.reg(data)))

    # --- images (against the images Kyty binds for the program's specialisation, see images.py)
    def img_addr(self, o, k):
        if o.kind == 'vlist':
            return 'v%d_%d' % (o.list[k], self.h)
        return self.reg(o, k)

    def image_emit(self, heap, vec, e, results, srgb=False):
        """the statement of an image read: e reads through var (HV_ for a heap image: Kyty's view lookup, then the
        typed heap array of the slot's kind)"""
        if srgb:
            e = 'srgb_decode(%s)' % e
        if heap is None:
            self.emit('if (EX) { %s4 r_ = %s; %s }' % (vec, e, results))
            return
        rank, m = heap['rank'], heap['mapping']
        # the table base Kyty's view uses: the user slot, dwords [64, 96) geometry stage, [96, 128) pixel stage, one
        # uint64 per heap root (rank among the module's roots)
        slot = (64 if self.stage != 'ps' else 96) + 2 * rank
        chain = ' else '.join('if (k_ == %du) r_ = %s;' % (k, e.replace('HV_', 'HEAP_%d[nonuniformEXT(el_)]' % kind['index']))
                              for k, kind in enumerate(heap['kinds']) if kind is not None)
        self.emit('if (EX) { UserTable ut_ = UserTable(pc.user_slot); '
                  'uint64_t hb_ = packUint2x32(uvec2(ut_.v[%du], ut_.v[%du])); uint key_ = uint(TADDR_%x + %dul - hb_); '
                  'uint cnt_ = fsrt.w[%du]; uint64_t sl_ = packUint2x32(uvec2(fsrt.w[%du], fsrt.w[%du])); '
                  'uint e_ = (hb_ == 0ul || sl_ == 0ul) ? 0xffffffffu : W32(sl_ + uint64_t(min(key_ >> 2, cnt_)) * 4ul).v; '
                  'uint k_ = e_ >> 20; uint el_ = e_ & 0xfffffu; %s4 r_ = %s4(0); %s %s }' % (
                      slot, slot + 1, heap['load_pc'], 4 * heap['dword'], m, m + 1, m + 2, vec, vec, chain, results))
        self.heap_addrs.add(heap['load_pc'])

    def op_image(self, inst):
        m = self.images.map.get(inst.off) if getattr(self, 'images', None) is not None else None
        if m is None:
            self.unknown[inst.op] += 1
            self.emit('/* UNHANDLED %s */' % inst.text)
            return
        img, smp = m
        b = inst.base
        vdata, vaddr, t, s = images.operands(inst)
        dmask = int(inst.mods.get('dmask', '0x1'), 0)
        d16 = 'd16' in inst.mods
        dim = inst.mods.get('dim', '')
        if dim not in images.GUEST_DIM:
            raise images.Unsupported('%s: dimension %s' % (inst, dim))
        ncoord, layered = images.GUEST_DIM[dim]
        srgb = False
        if img.get('srgb', 0) and not img.get('conversion', 0) and self.gfx is not None:
            # Kyty's in-shader decode (when its module has it): the texel through the sRGB curve
            if self.gfx.srgb_decode:
                srgb = True
        elif img.get('conversion', 0) or img.get('srgb', 0):
            raise images.Unsupported('%s: image %d needs a format conversion' % (inst, img['index']))
        heap = self.images.heap.get(inst.off)
        var = 'HV_' if heap is not None else 'IMG%d[%d]' % (img['binding'], img['element'])
        numeric = img['numeric']
        vec = {images.NUMERIC_FLOAT: 'vec', images.NUMERIC_UINT: 'uvec', images.NUMERIC_SINT: 'ivec'}[numeric]
        fam = images.family(b)
        comps = [c for c in range(4) if (dmask >> c) & 1]
        swz = img.get('swizzle', 0xfac)
        if img['class'] == images.CLASS_STORAGE and swz != 0xfac:
            raise images.Unsupported('%s: storage image %d with swizzle %#x' % (inst, img['index'], swz))

        def bits(e):
            return {images.NUMERIC_FLOAT: 'floatBitsToUint(%s)', images.NUMERIC_UINT: '%s',
                    images.NUMERIC_SINT: 'uint(%s)'}[numeric] % e

        def results(r):
            """the VGPR writes for the components dmask selects (d16: two per register)"""
            if not d16:
                return ' '.join('%s = %s;' % (self.reg(vdata, k), bits('%s.%s' % (r, 'xyzw'[c])))
                                for k, c in enumerate(comps))
            out = []
            for k in range(0, len(comps), 2):
                pair = comps[k:k + 2]
                if numeric == images.NUMERIC_FLOAT:
                    halves = ['uint(float16BitsToUint16(float16_t(%s.%s)))' % (r, 'xyzw'[c]) for c in pair]
                else:
                    halves = ['(uint(%s.%s) & 0xffffu)' % (r, 'xyzw'[c]) for c in pair]
                e = halves[0] if len(halves) == 1 else '(%s | (%s << 16))' % (halves[0], halves[1])
                out.append('%s = %s;' % (self.reg(vdata, k // 2), e))
            return ' '.join(out)

        kn = images.KYTY_COORDS.get(img['dim'])
        if kn is None or img['dim'] in (images.DIM_2D_MSAA, images.DIM_2D_MSAA_ARRAY):
            raise images.Unsupported('%s: image dimension %d' % (inst, img['dim']))

        def coords(conv, scalar, k0=0, sample=False):
            """the bound image's coordinates from the instruction's address; Kyty's conventions: a 1D image is a 2D
            view of one row (row centre 0.5 when sampled, row 0 when fetched); a cube is a 2D array of faces
            (guest s, t in [1, 2]; guest layer = slice * 8 + face)"""
            c = [conv % self.img_addr(vaddr, k0 + k) if k < ncoord else conv % '0u' for k in range(kn)]
            if img.get('cube') and sample and kn == 3:
                c[0], c[1] = '(%s - 1.0)' % c[0], '(%s - 1.0)' % c[1]
                c[2] = 'float(cube_layer(uint(%s)))' % c[2]
            if img['dim'] in images.ONE_DIMENSIONAL:
                c.insert(1, '0.5' if sample else '0')
            n = len(c)
            return c[0] if n == 1 else '%s%d(%s)' % (scalar, n, ', '.join(c))

        if fam == 'load':
            if b != 'image_load':
                raise images.Unsupported('%s' % inst)
            coord = coords('int(%s)', 'ivec')
            if img['class'] == images.CLASS_SAMPLED:
                e = 'texelFetch(%s, %s, 0)' % (var, coord)
            else:
                e = 'imageLoad(%s, %s)' % (var, coord)
            self.image_emit(heap, vec, e, results('r_'), srgb)
            return
        if fam == 'store':
            if b != 'image_store' or d16:
                raise images.Unsupported('%s' % inst)
            coord = coords('int(%s)', 'ivec')
            val = {images.NUMERIC_FLOAT: 'uintBitsToFloat(%s)', images.NUMERIC_UINT: '%s',
                   images.NUMERIC_SINT: 'int(%s)'}[numeric]
            zero = {images.NUMERIC_FLOAT: '0.0', images.NUMERIC_UINT: '0u', images.NUMERIC_SINT: '0'}[numeric]
            data, k = [], 0
            for ch in range(4):
                if (dmask >> ch) & 1:
                    data.append(val % self.reg(vdata, k))
                    k += 1
                else:
                    data.append(zero)
            self.emit('if (EX) imageStore(%s, %s, %s4(%s));' % (var, coord, vec, ', '.join(data)))
            return
        if b == 'image_get_resinfo':
            # vaddr: mip level; dmask: x width, y height, z depth / layers, w mip count
            if img['class'] != images.CLASS_SAMPLED:
                raise images.Unsupported('%s: size of a storage image' % inst)
            kdim = img['dim']
            n = {images.DIM_1D: 1, images.DIM_1D_ARRAY: 2, images.DIM_2D: 2, images.DIM_2D_ARRAY: 3,
                 images.DIM_3D: 3}.get(kdim)
            if n is None:
                raise images.Unsupported('%s: dimension %d' % (inst, kdim))
            if heap is not None:
                raise images.Unsupported('%s: size of a heap image' % inst)
            size = 'textureSize(%s, int(%s))' % (var, self.img_addr(vaddr, 0))
            vals = ['uint(sz_)' if n == 1 else 'uint(sz_.%s)' % 'xyz'[c] for c in range(n)]
            while len(vals) < 3:
                vals.append('1u')
            vals.append('uint(textureQueryLevels(%s))' % var)
            self.emit('if (EX) { %s sz_ = %s; %s }' % ('int' if n == 1 else 'ivec%d' % n, size, ' '.join(
                '%s = %s;' % (self.reg(vdata, k), vals[c]) for k, c in enumerate(comps))))
            return
        if fam == 'sample':
            # gfx10 address order: offset (_o), bias (_b), z compare (_c), derivatives (_d), coordinates, lod (_l)
            offs = b.endswith('_o')
            core = b[:-2] if offs else b
            if core not in ('image_sample_lz', 'image_sample_l', 'image_gather4_lz', 'image_sample_c_lz',
                            'image_sample', 'image_sample_b', 'image_sample_d', 'image_get_lod'):
                raise images.Unsupported('%s' % inst)
            if img['class'] != images.CLASS_SAMPLED:
                raise images.Unsupported('%s: sampling a storage image' % inst)
            k = 0
            offset = None
            ref = None
            bias = None
            grads = None
            if offs:
                # texel offsets: 6-bit signed fields at bits 0, 8, 16 (non-constant: Kyty's Offset operand, allowed
                # by the plug-in's validator and maintenance8)
                o_ = self.img_addr(vaddr, 0)
                nofs = 3 if img['dim'] == images.DIM_3D else (1 if img['dim'] in images.ONE_DIMENSIONAL else 2)
                fields = ['bitfieldExtract(int(%s), %d, 6)' % (o_, 8 * j) for j in range(nofs)]
                if img['dim'] in images.ONE_DIMENSIONAL:
                    fields.append('0')
                    nofs = 2
                offset = fields[0] if nofs == 1 else 'ivec%d(%s)' % (nofs, ', '.join(fields))
                k = 1
            if core == 'image_sample_c_lz':
                ref = 'uintBitsToFloat(%s)' % self.img_addr(vaddr, k)
                k += 1
            if core == 'image_sample_b':
                bias = 'uintBitsToFloat(%s)' % self.img_addr(vaddr, k)
                k += 1
            if core == 'image_sample_d':
                # derivatives (x: ds/dh, dt/dh, y: ds/dv, dt/dv for 2D), then the coordinates
                if img['dim'] not in (images.DIM_2D, images.DIM_2D_ARRAY) or ncoord - (1 if layered else 0) != 2:
                    raise images.Unsupported('%s: gradients of a non-2D image' % inst)
                g_ = ['uintBitsToFloat(%s)' % self.img_addr(vaddr, k + j) for j in range(4)]
                grads = ('vec2(%s, %s)' % (g_[0], g_[1]), 'vec2(%s, %s)' % (g_[2], g_[3]))
                k += 4
            implicit = core in ('image_sample', 'image_sample_b', 'image_get_lod')
            if implicit and self.stage != 'ps':
                raise images.Unsupported('%s: implicit derivatives outside the pixel stage' % inst)
            lod = 'uintBitsToFloat(%s)' % self.img_addr(vaddr, k + ncoord) if core == 'image_sample_l' else '0.0'
            kdim = images.DIM_SUFFIX[img['dim']]
            pre = {images.NUMERIC_FLOAT: '', images.NUMERIC_UINT: 'u', images.NUMERIC_SINT: 'i'}[numeric]
            smpvar = 'SMP[%d]' % smp['index']
            if ref is not None:
                raise images.Unsupported('%s: depth compare' % inst)
            if offset is not None and img.get('cube'):
                raise images.Unsupported('%s: offsets on a cube' % inst)
            comb = '%ssampler%s(%s, %s)' % (pre, kdim, var, smpvar)
            coord = coords('uintBitsToFloat(%s)', 'vec', k, sample=True)
            if offset is not None:
                self.needs_offset_ext = True
            if core == 'image_get_lod':
                # Kyty: OpImageQueryLod, its two components as the first two results
                if offset is not None or numeric != images.NUMERIC_FLOAT:
                    raise images.Unsupported('%s' % inst)
                e = 'vec4(textureQueryLod(%s, %s), 0.0, 0.0)' % (comb, coord)
                comps = [c for c in range(4) if (dmask >> c) & 1]
                self.image_emit(heap, vec, e, results('r_'))
                return
            if core == 'image_gather4_lz':
                if len(comps) != 1:
                    raise images.Unsupported('%s: gather of %d components' % (inst, len(comps)))
                if offset is not None:
                    e = 'textureGatherOffset(%s, %s, %s, %d)' % (comb, coord, offset, comps[0])
                else:
                    e = 'textureGather(%s, %s, %d)' % (comb, coord, comps[0])
                comps = [0, 1, 2, 3]
            elif grads is not None:
                e = ('textureGradOffset(%s, %s, %s, %s, %s)' % (comb, coord, grads[0], grads[1], offset)
                     if offset is not None else 'textureGrad(%s, %s, %s, %s)' % (comb, coord, grads[0], grads[1]))
            elif implicit:
                if offset is not None:
                    e = 'textureOffset(%s, %s, %s%s)' % (comb, coord, offset, ', ' + bias if bias else '')
                else:
                    e = 'texture(%s, %s%s)' % (comb, coord, ', ' + bias if bias else '')
            else:
                e = ('textureLodOffset(%s, %s, %s, %s)' % (comb, coord, lod, offset) if offset is not None
                     else 'textureLod(%s, %s, %s)' % (comb, coord, lod))
            self.image_emit(heap, vec, e, results('r_'), srgb)
            return
        raise images.Unsupported('%s' % inst)

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

    def ds_swizzle(self, inst):
        """ds_swizzle_b32: a lane permutation within each 32 lanes (one half of a lane pair = one warp); the shuffle
        runs for every thread (outside the exec guard), the write only for active lanes"""
        d, src = inst.ops
        off = inst.mods.get('offset', '0')
        m = re.match(r'swizzle\((\w+)(?:,(.*))?\)$', off)
        lane = 'LANE & 31u'
        if m:
            mode, args = m.group(1), [a.strip().strip('"') for a in (m.group(2) or '').split(',') if a.strip()]
            if mode == 'BITMASK_PERM':
                and_m = or_m = xor_m = 0
                for k, ch in enumerate(args[0]):
                    bit = 1 << (4 - k)
                    if ch == '1':
                        or_m |= bit
                    elif ch == 'p':
                        and_m |= bit
                    elif ch == 'i':
                        and_m |= bit
                        xor_m |= bit
                src_lane = '((((%s) & %du) | %du) ^ %du)' % (lane, and_m, or_m, xor_m)
            elif mode == 'SWAP':
                src_lane = '((%s) ^ %du)' % (lane, int(args[0]))
            elif mode == 'REVERSE':
                src_lane = '((%s) ^ %du)' % (lane, int(args[0]) - 1)
            elif mode == 'BROADCAST':
                src_lane = '(((%s) & %du) | %du)' % (lane, ~(int(args[0]) - 1) & 31, int(args[1]))
            elif mode == 'QUAD_PERM':
                q = sum(int(a) << (2 * k) for k, a in enumerate(args))
                src_lane = '(((%s) & 28u) | ((0x%02xu >> (((%s) & 3u) * 2u)) & 3u))' % (lane, q, lane)
            else:
                raise ValueError('ds_swizzle mode %s' % mode)
        else:
            v = int(off, 0)
            if v & 0x8000:
                q = v & 0xff
                src_lane = '(((%s) & 28u) | ((0x%02xu >> (((%s) & 3u) * 2u)) & 3u))' % (lane, q, lane)
            else:
                src_lane = '((((%s) & %du) | %du) ^ %du)' % (lane, v & 31, (v >> 5) & 31, (v >> 10) & 31)
        self.emit('{ uint sw_ = subgroupShuffle(%s, %s); if (EX) %s = sw_; }' % (self.reg(src), src_lane, self.reg(d)),
                  valu=True)
        self.wave_points.add(inst.off)

    def op_ds(self, inst):
        b = inst.base
        if b == 'ds_swizzle_b32':
            self.ds_swizzle(inst)
            return
        o0 = int(inst.mods.get('offset0', '0'), 0)
        o1 = int(inst.mods.get('offset1', '0'), 0)
        off = int(inst.mods.get('offset', '0'), 0)
        self.lds_points.add(inst.off)
        if b in ('ds_read_addtid_b32', 'ds_write_addtid_b32'):
            # address: M0[15:0] + offset + the lane's index in the wave * 4
            a = '((s124 & 0xffffu) + %du + LANE * 4u) >> 2' % off
            if b == 'ds_read_addtid_b32':
                self.emit('if (EX) %s = LDS[%s];' % (self.reg(inst.ops[0]), a))
                self.lds_reads.add(inst.off)
            else:
                self.emit('if (EX) LDS[%s] = %s;' % (a, self.reg(inst.ops[0])))
                self.lds_writes.add(inst.off)
            return
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
        elif b.replace('_rtn', '') in LDS_ATOMICS or b in LDS_ATOMICS:
            fn = LDS_ATOMICS.get(b.replace('_rtn', '')) or LDS_ATOMICS[b]
            if '_rtn' in b:
                d, addr, data = inst.ops
                self.emit('if (EX) %s = %s(LDS[(%s + %du) >> 2], %s);' % (self.reg(d), fn, self.reg(addr), off,
                                                                         self.reg(data)))
                self.lds_reads.add(inst.off)
            else:
                addr, data = inst.ops
                self.emit('if (EX) %s(LDS[(%s + %du) >> 2], %s);' % (fn, self.reg(addr), off, self.reg(data)))
            self.lds_writes.add(inst.off)
        elif b in ('ds_min_f32', 'ds_max_f32'):
            addr, data = inst.ops
            self.emit('if (EX) lds_atomic_%s_f32((%s + %du) >> 2, %s);' % (b[3:6], self.reg(addr), off,
                                                                           self.reg(data)))
            self.lds_writes.add(inst.off)
        else:
            self.unknown[inst.op] += 1


# LDS atomics on the uint LDS array (signed min/max are not here: they would need the int view)
LDS_ATOMICS = {'ds_add_u32': 'atomicAdd', 'ds_max_u32': 'atomicMax', 'ds_min_u32': 'atomicMin', 'ds_or_b32': 'atomicOr',
               'ds_and_b32': 'atomicAnd', 'ds_xor_b32': 'atomicXor', 'ds_wrxchg_rtn_b32': 'atomicExchange'}

FLOAT2 = {
    'v_add_f32': '(%s + %s)', 'v_sub_f32': '(%s - %s)', 'v_mul_f32': '(%s * %s)',
}
FLOAT1 = {
    'v_rcp_f32': 'rcp_f32(%s)', 'v_rcp_iflag_f32': 'rcp_f32(%s)', 'v_rsq_f32': 'rsq_f32(%s)',
    'v_sqrt_f32': 'sqrt(%s)', 'v_exp_f32': 'exp2(%s)', 'v_log_f32': 'log2(%s)',
    'v_sin_f32': 'sin_rev(%s)', 'v_cos_f32': 'cos_rev(%s)', 'v_fract_f32': 'fract_f32(%s)',
    'v_ceil_f32': 'ceil(%s)', 'v_floor_f32': 'floor(%s)', 'v_rndne_f32': 'roundEven(%s)',
    'v_trunc_f32': 'trunc(%s)',
}
INT2 = {
    'v_add_nc_u32': '(%s + %s)', 'v_sub_nc_u32': '(%s - %s)', 'v_and_b32': '(%s & %s)',
    'v_xor_b32': '(%s ^ %s)', 'v_min_u32': 'min(%s, %s)', 'v_max_u32': 'max(%s, %s)',
    'v_bcnt_u32_b32': '(uint(bitCount(%s)) + %s)',
    'v_cvt_pk_u16_u32': '((%s & 0xffffu) | (%s << 16))',
    'v_cvt_pknorm_u16_f32': 'packUnorm2x16(vec2(uintBitsToFloat(%s), uintBitsToFloat(%s)))',
    'v_cvt_pknorm_i16_f32': 'packSnorm2x16(vec2(uintBitsToFloat(%s), uintBitsToFloat(%s)))',
    'v_min_i32': 'uint(min(int(%s), int(%s)))',
    'v_mul_lo_u32': '(%s * %s)',
    'v_mul_u32_u24': '((%s & 0xffffffu) * (%s & 0xffffffu))', 'v_max_i32': 'uint(max(int(%s), int(%s)))',
    'v_mul_i32_i24': 'uint(bitfieldExtract(int(%s), 0, 24) * bitfieldExtract(int(%s), 0, 24))',
}
INT3 = {
    'v_add3_u32': '(%s + %s + %s)', 'v_lshl_add_u32': '((%s << (%s & 31u)) + %s)',
    'v_mad_u32_u24': '((%s & 0xffffffu) * (%s & 0xffffffu) + %s)',
    'v_lshl_or_b32': '((%s << (%s & 31u)) | %s)', 'v_and_or_b32': '((%s & %s) | %s)',
    'v_or3_b32': '(%s | %s | %s)', 'v_add_lshl_u32': '((%s + %s) << (%s & 31u))',
    'v_xad_u32': '((%s ^ %s) + %s)', 'v_med3_u32': 'med3_u32(%s, %s, %s)',
    'v_med3_i32': 'uint(med3_i32(int(%s), int(%s), int(%s)))',
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

    def shift16(fmt):
        # 16-bit shifts (VOP2 / VOP3 without op_sel): the low half of src1 shifted by src0[3:0]; the result's high half
        # is zero, as the other 16-bit results here
        def op(inst):
            d, a, c = inst.ops
            if 'op_sel' in inst.text:
                tr.unknown[inst.base + ' op_sel'] += 1
                return
            tr.write_v_u32(inst, d, fmt % (tr.src_int(c, inst, 1), tr.src_int(a, inst, 0)))
        return op

    tr.op_v_lshrrev_b16 = shift16('((%s & 0xffffu) >> (%s & 15u))')
    tr.op_v_lshlrev_b16 = shift16('((%s << (%s & 15u)) & 0xffffu)')
    tr.op_v_ashrrev_i16 = shift16('(uint(bitfieldExtract(int(%s), 0, 16) >> (%s & 15u)) & 0xffffu)')
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
    bound = {}         # SGPR -> the largest value it can hold (scalar_bound)
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
            reg_bound = bound.get(off.first) if off.kind == 'reg' and off.file == 's' and off.count == 1 else None
            for r in d.regs():
                holder.pop(int(r[1:]), None)
                bound.pop(int(r[1:]), None)
                redefined.add(int(r[1:]))
            if n is not None and offset is None and reg_bound is not None:
                # an SGPR offset with a known bound (a masked table index): the extent up to the bound
                nodes[n][3] = max(nodes[n][3], reg_bound + imm + 4 * d.count)
                continue
            if n is None or offset is None:
                continue
            nodes[n][3] = max(nodes[n][3], offset + 4 * d.count)
            for k in range(d.count):
                holder[d.first + k] = (n, offset + 4 * k)
            continue
        nb = scalar_bound(i, bound)
        for r in all_sgpr_defs(i):
            holder.pop(int(r[1:]), None)
            bound.pop(int(r[1:]), None)
            redefined.add(int(r[1:]))
        if nb is not None:
            bound[nb[0]] = nb[1]
    return [tuple(n) for n in nodes]


def scalar_bound(i, bound):
    """(SGPR, the largest value it can hold) after a scalar instruction whose result has a bound (program order):
    s_and_b32 with a constant, s_bfe_u32, s_lshl_b32 / s_lshr_b32 of a bounded value by a constant, s_mov_b32 of a
    constant; None otherwise"""
    b = i.base
    if not i.ops or i.ops[0].kind != 'reg' or i.ops[0].file != 's' or i.ops[0].count != 1:
        return None
    d = i.ops[0].first

    def val(o):
        if o.kind == 'const':
            return o.value
        if o.kind == 'reg' and o.file == 's' and o.count == 1:
            return bound.get(o.first)
        return None
    if b == 's_mov_b32' and i.ops[1].kind == 'const':
        return d, i.ops[1].value
    if b == 's_and_b32':
        cands = [o.value for o in i.ops[1:3] if o.kind == 'const']
        other = [val(o) for o in i.ops[1:3] if o.kind != 'const']
        if cands:
            return d, min(cands + [x for x in other if x is not None])
        return None
    if b == 's_bfe_u32' and i.ops[2].kind == 'const':
        w = (i.ops[2].value >> 16) & 0x7f
        return (d, (1 << min(w, 32)) - 1) if w < 32 else None
    if b in ('s_lshl_b32', 's_lshr_b32') and i.ops[2].kind == 'const':
        x = val(i.ops[1])
        if x is None:
            return None
        sh = i.ops[2].value & 31
        r = (x << sh) if b == 's_lshl_b32' else (x >> sh)
        return (d, r) if r < (1 << 32) else None
    return None


def reaching_sgpr_defs(insts, user_sgprs):
    """Reaching definitions of the SGPRs over the program's control flow: for each instruction, SGPR -> the set of
    instructions whose write of it may reach the instruction (-1: the user data at entry, -2: undefined). None for a
    program with indirect branches."""
    n = len(insts)
    index = {i.off: k for k, i in enumerate(insts)}
    succ = []
    for k, i in enumerate(insts):
        b = i.base
        if b.startswith(('s_setpc', 's_swappc', 's_rfe')):
            return None
        if b == 's_endpgm':
            succ.append(())
            continue
        out = []
        if i.target is not None:
            t = index.get(i.target)
            if t is not None:
                out.append(t)
            if b == 's_branch':
                succ.append(tuple(out))
                continue
        if k + 1 < n:
            out.append(k + 1)
        succ.append(tuple(out))
    writes = [tuple(set(int(r[1:]) for r in all_sgpr_defs(i))) for i in insts]
    state_in = [None] * n
    state_in[0] = tuple(frozenset([-1]) if r < user_sgprs else frozenset([-2]) for r in range(128))
    work = [0]
    while work:
        k = work.pop()
        out = list(state_in[k])
        for r in writes[k]:
            if r < 128:
                out[r] = frozenset([k])
        out = tuple(out)
        for t in succ[k]:
            cur = state_in[t]
            if cur is None:
                state_in[t] = out
                work.append(t)
                continue
            if all(a <= c for a, c in zip(out, cur)):
                continue
            state_in[t] = tuple(a | c for a, c in zip(out, cur))
            work.append(t)
    return state_in


def code_tables(insts, user_sgprs, code=None):
    """Constant tables in the program's code: buffer loads through a V# built from s_getpc_b64 (base = the address
    after the s_getpc + a constant: s_add_u32 / s_addc_u32 0) and constant words 2-3 (s_mov). Returns
    ({load offset: (table byte offset in the code, num_records)}, {s_getpc offsets whose pair only feeds them}); the
    translation embeds the tables (the code bytes the emulator matches cover them)."""
    state = reaching_sgpr_defs(insts, user_sgprs)
    if state is None:
        return {}, set()
    index = {id(i): k for k, i in enumerate(insts)}

    def single(k, r):
        d = state[k][r]
        return next(iter(d)) if len(d) == 1 else None

    def const_of(k, r):
        """the constant a single s_mov / s_movk wrote into s[r] before instruction k"""
        d = single(k, r)
        if d is None or d < 0:
            return None
        x = insts[d]
        if x.base in ('s_mov_b32', 's_movk_i32') and x.ops[1].kind == 'const':
            v = x.ops[1].value
            if x.base == 's_movk_i32':
                v = v - 0x10000 if v & 0x8000 else v
            return v & 0xffffffff
        return None

    tables, feeding, other = {}, {}, set()
    for k, i in enumerate(insts):
        if not i.base.startswith('buffer_load_dword'):
            continue
        rsrc = i.ops[2]
        if rsrc.kind != 'reg' or rsrc.file != 's' or rsrc.count != 4:
            continue
        lo, hi = single(k, rsrc.first), single(k, rsrc.first + 1)
        if lo is None or hi is None or lo < 0 or hi < 0:
            continue
        add, addc = insts[lo], insts[hi]
        if add.base != 's_add_u32' or addc.base != 's_addc_u32':
            continue
        ca = [o for o in add.ops[1:] if o.kind == 'const']
        ra = [o for o in add.ops[1:] if o.kind == 'reg' and o.file == 's']
        cc = [o for o in addc.ops[1:] if o.kind == 'const']
        rc = [o for o in addc.ops[1:] if o.kind == 'reg' and o.file == 's']
        if len(ca) != 1 or len(ra) != 1 or len(cc) != 1 or cc[0].value != 0 or len(rc) != 1:
            continue
        g_lo, g_hi = single(lo, ra[0].first), single(hi, rc[0].first)
        if g_lo is None or g_lo != g_hi or g_lo < 0 or insts[g_lo].base != 's_getpc_b64':
            continue
        g = insts[g_lo]
        if ra[0].first != g.ops[0].first or rc[0].first != g.ops[0].first + 1:
            continue
        num, w3 = const_of(k, rsrc.first + 2), const_of(k, rsrc.first + 3)
        if num is None or w3 is None or (w3 >> 23) & 1:        # ADD_TID: not a plain table
            continue
        tables[i.off] = (g.off + 4 + ca[0].value, num)
        feeding.setdefault(g.off, set()).update((add.off, addc.off, i.off))
    # an s_getpc whose pair (or the add / addc results) is read anywhere else is not handled
    users = {}
    for k, i in enumerate(insts):
        for o in i.ops[1:] if i.ops else []:
            if o.kind != 'reg' or o.file != 's':
                continue
            for r in range(o.first, o.first + o.count):
                for d in state[k][r]:
                    if d >= 0 and insts[d].base in ('s_getpc_b64', 's_add_u32', 's_addc_u32'):
                        users.setdefault(insts[d].off, set()).add(i.off)
    ok = set()
    for g_off, allowed in feeding.items():
        chain = [o for o in allowed if any(x.off == o and x.base in ('s_add_u32', 's_addc_u32') for x in insts)]
        if users.get(g_off, set()) <= allowed and all(users.get(c, set()) <= allowed for c in chain):
            ok.add(g_off)
    tables = {off: t for off, t in tables.items() if any(off in feeding[g] and g in ok for g in feeding)}
    return tables, ok


def store_vsharps(insts, user_sgprs, loads=False):
    """For the emulator's bypass (<name>.bypass: a dispatch batched without the emulator's own preparation, which must
    still learn what the dispatch writes): where the V# of each buffer store and atomic comes from, as a chain from
    the user data (sgpr, o1, ..., on): p = the pointer in s[sgpr:sgpr+1]; p = *(p + o) & (2^48 - 1) for o1..o(n-1)
    (pointers, and V# bases: a V#'s first qword holds its base); the V# is at p + on. Exact chains only: at the store
    each SGPR of the V# is reached by one definition, the same s_load / s_buffer_load with a constant offset, whose
    base is again such a chain (down to user SGPRs no instruction wrote before). Returns [(instruction offset, chain
    or None)]; None: the program may write where the emulator could not tell (no bypass). loads: the same for the
    V#s the program reads through (None: reads the emulator cannot sync ahead of the dispatch)."""
    state = reaching_sgpr_defs(insts, user_sgprs)

    def single(k, regs):
        if state is None or state[k] is None:
            return None
        ds = set()
        for r in regs:
            d = state[k][r]
            if len(d) != 1:
                return None
            ds |= d
        return next(iter(ds)) if len(ds) == 1 else None

    def const_offset(x, off):
        imm = int(x.mods.get('offset', '0'), 0)
        if off.kind == 'null':
            return imm
        return off.value + imm if off.kind == 'const' else None

    def loaded_from(k, first, count, depth):
        """the chain to the address s[first:first+count) was loaded from (before instruction k): (chain of the
        base pointer, byte offset), or None"""
        d = single(k, range(first, first + count))
        if d is None or d < 0 or depth > 8:
            return None
        x = insts[d]
        if x.base.startswith('s_load_dword'):
            dst, base, off = x.ops
            offset = const_offset(x, off)
            if offset is None or base.kind != 'reg' or base.file != 's' or base.count < 2:
                return None
            chain = pointer(d, base.first, depth + 1)
        elif x.base.startswith('s_buffer_load_dword'):
            dst, vs, off = x.ops
            offset = const_offset(x, off)
            if offset is None or vs.kind != 'reg' or vs.file != 's' or vs.count != 4:
                return None
            at = loaded_from(d, vs.first, 4, depth + 1)          # the V#; its base is the buffer's address
            chain = None if at is None else at[0] + (at[1],)
        else:
            return None
        return None if chain is None else (chain, offset + 4 * (first - dst.first))

    def pointer(k, first, depth=0):
        """the chain of the pointer in s[first:first+1] before instruction k"""
        if single(k, (first, first + 1)) == -1:
            return (first,) if first + 1 < user_sgprs else None
        at = loaded_from(k, first, 2, depth)
        return None if at is None else at[0] + (at[1],)

    def chain_of(k, rsrc):
        at = (loaded_from(k, rsrc.first, 4, 0) if rsrc.kind == 'reg' and rsrc.file == 's' and rsrc.count == 4
              else None)
        return None if at is None else at[0] + (at[1],)

    out = []
    for k, i in enumerate(insts):
        b = i.base
        if loads:
            # what the program reads through the page table: buffer loads (and atomics) through V#s, global loads,
            # and s_buffer_loads (their V#s' ranges; snapshotted when small, see scalar_reach)
            if b.startswith(('buffer_load', 'tbuffer_load', 'buffer_atomic')):
                out.append((i.off, chain_of(k, i.ops[2])))
            elif b.startswith('s_buffer_load'):
                out.append((i.off, chain_of(k, i.ops[1])))
            elif b.startswith(('global_load', 'flat_load', 'global_atomic', 'flat_atomic', 'image_')):
                out.append((i.off, None))
            continue
        if b.startswith(('buffer_store', 'buffer_atomic', 'tbuffer_store')):
            out.append((i.off, chain_of(k, i.ops[2])))
        elif b.startswith(('global_store', 'global_atomic', 'flat_store', 'flat_atomic', 'scratch_store', 's_store',
                           's_buffer_store', 'image_store', 'image_atomic')):
            out.append((i.off, None))
    return out


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


# ---------------------------------------------------------------------------------------------- exec guards
# instructions whose register operands are all read (stores: their data is operand 0; atomics: read conservatively)
GUARD_STORE = ('buffer_store', 'ds_write', 'global_store', 'flat_store', 'scratch_store', 'v_cmp_', 'v_cmpx', 'ds_min',
               'ds_max', 'ds_add', 'ds_sub', 'ds_and', 'ds_or', 'ds_xor', 'ds_inc', 'ds_dec', 'buffer_atomic',
               'global_atomic', 'flat_atomic', 'image_store', 'image_atomic', 'tbuffer_store')


def guard_vregs(o):
    if o is not None and o.kind == 'vlist':
        return list(o.list)
    return [o.first + q for q in range(o.count)] if o is not None and o.kind == 'reg' and o.file == 'v' else []


def guard_vdefs(i):
    """VGPRs an instruction writes per lane"""
    b = i.base
    if not i.ops or b.startswith(GUARD_STORE + ('s_',)) or b.startswith(('v_readlane', 'v_readfirstlane')):
        return []
    return guard_vregs(i.ops[0])


def guard_vuses(i):
    """VGPRs an instruction reads (its destination too when it keeps part of the old value)"""
    b, ops = i.base, i.ops
    if not ops:
        return []
    out = []
    for o in (ops if b.startswith(GUARD_STORE) else ops[1:]):
        out += guard_vregs(o)
    # the destination is read too: multiply-accumulate, 16-bit writes that keep the other half (d16 loads, 16-bit
    # ALU with op_sel), SDWA with UNUSED_PRESERVE, DPP (lanes without a valid source keep the old value)
    if (b.startswith(('v_mac_', 'v_fmac_', 'v_pk_fmac_', 'v_fma_mixlo', 'v_fma_mixhi')) or
            'd16' in b or b.startswith('v_writelane') or 'UNUSED_PRESERVE' in i.text or 'dpp' in i.op or
            (b.endswith(('_f16', '_u16', '_i16', '_b16')) and 'op_sel' in i.text)):
        out += guard_vregs(ops[0])
    return out


def guard_pure_alu(i):
    """a VALU instruction whose write may run for inactive lanes (no memory access, no cross-lane write)"""
    return (i.base.startswith('v_') and not i.base.startswith(('v_readlane', 'v_readfirstlane', 'v_writelane',
                                                                 'v_cmp', 'v_interp')))


def exec_guards_needed(insts):
    """The VGPR writes that must keep the exec guard: a lane that is inactive at the write keeps its old value, which
    matters only if that lane reads the register later before writing it itself.

    Exec values are symbols in a tree (a child's lanes are a subset of its parent's; 0 = the lanes the wave started
    with): a narrowing (v_cmpx, s_and_saveexec, s_and/andn2 into exec) creates a child, a restore from a saved copy
    reuses the saved symbol, control-flow merges of different symbols and unknown exec writes create fresh symbols.
    A symbol created inside a loop stands for different lanes in each iteration, so the backward pass lifts pending
    readers to the parent symbol where it crosses the point that creates their symbol. A write with symbol X covers
    the readers whose symbol lies in X's subtree; it needs the guard when a reader outside remains (cross-lane reads
    read every lane: symbol 0). Returns the set of instruction indices whose VGPR writes need the guard."""
    n = len(insts)
    by_off = {i.off: k for k, i in enumerate(insts)}
    succ = []
    for k, i in enumerate(insts):
        s = []
        if i.op != 's_endpgm':
            if i.op != 's_branch' and k + 1 < n:
                s.append(k + 1)
            if i.target is not None and i.target in by_off:
                s.append(by_off[i.target])
        succ.append(s)
    pred = [[] for _ in range(n)]
    for k, s in enumerate(succ):
        for t in s:
            pred[t].append(k)
    parent = [None]
    created = [None]
    memo = {}

    def new_sym(key, par, at):
        if key not in memo:
            memo[key] = len(parent)
            parent.append(par)
            created.append(at)
        else:
            parent[memo[key]] = par
        return memo[key]

    def anc(x):
        out = []
        while x is not None:
            out.append(x)
            x = parent[x]
            if len(out) > 100000:
                raise RuntimeError('exec symbol cycle')
        return out

    def within(y, x):
        return x in anc(y)

    def lca(syms):
        common = set(anc(syms[0]))
        for x in syms[1:]:
            common &= set(anc(x))
        for a in anc(syms[0]):
            if a in common:
                return a
        return 0

    def pair_of(o):
        return o.first if o.kind == 'reg' and o.file == 's' and o.count == 2 else None

    EXEC = 126
    ex_in, ex_out, pairs_in, pairs_out = [None] * n, [None] * n, [None] * n, [None] * n
    ex_in[0], pairs_in[0] = 0, {}
    work, inq = collections.deque([0]), {0}
    visits = 0
    while work:
        k = work.popleft()
        inq.discard(k)
        visits += 1
        if visits > 50 * n + 100000:
            raise RuntimeError('exec symbols: no fixpoint')
        if k != 0:
            ins = [q for q in pred[k] if ex_out[q] is not None]
            if not ins:
                continue
            syms = [ex_out[q] for q in ins]
            if all(x == syms[0] for x in syms):
                ex_in[k] = syms[0]
            else:
                m = memo.get(('merge', k))
                par = lca(syms)
                if m is not None and within(par, m):
                    par = 0
                ex_in[k] = new_sym(('merge', k), par, ('merge', k))
            pm = dict(pairs_out[ins[0]])
            for q in ins[1:]:
                pm = {r: x for r, x in pm.items() if pairs_out[q].get(r) == x}
            pairs_in[k] = pm
        ex, pairs = ex_in[k], dict(pairs_in[k])
        i = insts[k]
        b, ops = i.base, i.ops
        dst = ops[0] if ops else None

        def sym_of(o):
            if o is None:
                return None
            if o.kind == 'const' and o.value == 0xffffffff:
                return 0
            if o.kind == 'reg' and o.file == 's' and o.first == EXEC and o.count == 2:
                return ex
            p = pair_of(o)
            return pairs.get(p) if p is not None else None

        nex = ex
        kept = None
        if b.startswith('v_cmpx'):
            nex = new_sym(('n', k, ex), ex, k)
        elif 'saveexec' in b:
            s0 = sym_of(ops[1]) if len(ops) > 1 else None
            if b in ('s_and_saveexec_b64', 's_andn1_saveexec_b64'):
                nex = new_sym(('n', k, ex), ex, k)
            elif b == 's_orn2_saveexec_b64' and s0 is not None and within(ex, s0):
                nex = 0                 # S0 | ~EXEC with EXEC within S0: every lane (the whole-wave idiom)
            elif b == 's_or_saveexec_b64' and s0 is not None:
                nex = s0 if within(ex, s0) else new_sym(('u', k), lca([s0, ex]), k)
            else:
                nex = new_sym(('u', k), 0, k)
            if dst is not None and pair_of(dst) is not None:
                pairs[pair_of(dst)] = ex
                kept = pair_of(dst)
        elif dst is not None and dst.kind == 'reg' and dst.file == 's' and dst.first == EXEC:
            s0 = sym_of(ops[1]) if len(ops) > 1 else None
            s1 = sym_of(ops[2]) if len(ops) > 2 else None
            if b == 's_mov_b64':
                nex = s0 if s0 is not None else new_sym(('u', k), 0, k)
            elif b in ('s_and_b64', 's_andn2_b64'):
                base = s0 if s0 is not None else (s1 if b == 's_and_b64' else None)
                nex = new_sym(('n', k, base), base, k) if base is not None else new_sym(('u', k), 0, k)
            elif b == 's_or_b64' and s0 is not None and s1 is not None:
                nex = s1 if within(s0, s1) else (s0 if within(s1, s0) else new_sym(('u', k), lca([s0, s1]), k))
            else:
                nex = new_sym(('u', k), 0, k)
        elif dst is not None and pair_of(dst) is not None and b in ('s_mov_b64', 's_and_b64', 's_andn2_b64'):
            s0 = sym_of(ops[1]) if len(ops) > 1 else None
            s1 = sym_of(ops[2]) if b == 's_and_b64' and len(ops) > 2 else None
            if b == 's_mov_b64':
                v = s0
            else:
                base = s0 if s0 is not None else s1
                v = new_sym(('m', k, base), base, k) if base is not None else None
            if v is not None:
                pairs[pair_of(dst)] = v
                kept = pair_of(dst)
        for r in all_sgpr_defs(i):
            ri = int(r[1:])
            if ri in (EXEC, EXEC + 1) or (kept is not None and ri in (kept, kept + 1)):
                continue
            for p in (ri, ri - 1):
                pairs.pop(p, None)
        changed = ex_out[k] != nex or pairs_out[k] != pairs
        ex_out[k], pairs_out[k] = nex, pairs
        if changed:
            for t in succ[k]:
                if t not in inq:
                    work.append(t)
                    inq.add(t)

    def lift(pend, sym, to):
        return {r: frozenset(to if within(y, sym) else y for y in ys) for r, ys in pend.items()}

    defs = [guard_vdefs(i) for i in insts]
    uses = [guard_vuses(i) for i in insts]
    xlane = [i.base.startswith(('v_readlane', 'v_readfirstlane', 'v_permlane', 'ds_swizzle', 'ds_permute',
                                'ds_bpermute')) or 'dpp' in i.op for i in insts]
    pend_in = [dict() for _ in range(n)]
    need = set()
    changed, rounds = True, 0
    while changed:
        changed = False
        rounds += 1
        if rounds > 200:
            raise RuntimeError('exec guards: no fixpoint')
        for k in range(n - 1, -1, -1):
            if ex_in[k] is None:
                continue
            acc = collections.defaultdict(set)
            for t in succ[k]:
                p = pend_in[t]
                if ex_in[t] is not None and ex_in[t] != ex_out[k] and created[ex_in[t]] == ('merge', t):
                    p = lift(p, ex_in[t], ex_out[k])
                for r, ys in p.items():
                    acc[r] |= ys
            cur = {r: frozenset(ys) for r, ys in acc.items()}
            if ex_out[k] != ex_in[k] and created[ex_out[k]] == k:
                cur = lift(cur, ex_out[k], parent[ex_out[k]])
            x = ex_in[k]
            for r in defs[k]:
                ys = cur.get(r, frozenset())
                if any(not within(y, x) for y in ys):
                    need.add(k)
                cur[r] = frozenset(y for y in ys if not within(y, x))
            y = 0 if xlane[k] else x
            for r in uses[k]:
                cur[r] = cur.get(r, frozenset()) | {y}
            cur = {r: ys for r, ys in cur.items() if ys}
            if cur != pend_in[k]:
                pend_in[k] = cur
                changed = True
    # unreached instructions keep their guards
    need.update(k for k in range(n) if ex_in[k] is None)
    return need


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


def translate_shader(path, threads, lds_bytes, user_sgprs, tgid_sgpr, probes=False, resources=None, shape=None,
                     tidig=0, stage='cs', gfx_if=None):
    insts = gcnisa.decode(path)
    if stage != 'cs':
        # graphics: the user SGPRs follow the stage's system SGPRs (s[base, base + count)); the analyses see them all
        # as known at the start
        user_sgprs = gfx_if.user_data_base + user_sgprs
        lds_bytes = 4
    tr = Translator(insts, threads, lds_bytes, user_sgprs, tgid_sgpr)
    tr.vgpr_limit = max([o.first + o.count for i in insts for o in i.ops if o.kind == 'reg' and o.file == 'v'] + [1])
    if stage != 'cs':
        tr.stage, tr.halves, tr.gfx = stage, (0,), gfx_if
        if stage == 'vs' and not __import__('os').environ.get('SHADER_SR_NO_VERTEX_INPUT'):
            tr.fetch_map = embedded_fetches(insts, gfx_if)
    res = images.load_resources(resources) if resources else None
    if res is None and gfx_if is not None and (gfx_if.images or gfx_if.samplers):
        res = images.load_resources_gres(gfx_if.path)
    tr.images = images.ImageMap(insts, res, all_sgpr_defs, user_sgprs, heap_ok=gfx_if is not None) if res else None
    install_reversed(tr)
    tr.wave_points, tr.lds_points, tr.lds_reads, tr.lds_writes = set(), set(), set(), set()
    tr.ballot_index = set()
    tr.uniform_branch = {}
    st = Structure(insts)
    tr.sload_node = {}
    tr.reach = scalar_reach(insts, user_sgprs, tr.sload_node)
    tr.code_tables, tr.getpc_ok = code_tables(insts, user_sgprs)
    tr.code_bytes = open(path, 'rb').read()
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
    # VGPR writes that keep the exec guard (SHADER_SR_ALL_GUARDS=1: every write keeps it)
    import os
    guard_need = set(range(len(insts))) if os.environ.get('SHADER_SR_ALL_GUARDS') else exec_guards_needed(insts)
    pure_writes = [k for k, i in enumerate(insts) if guard_vdefs(i) and guard_pure_alu(i)]
    tr.notes.append('exec guards: %d of %d VALU writes need none' % (
        sum(1 for k in pure_writes if k not in guard_need), len(pure_writes)))
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
                    e = 'wave_any((s106 & LMASK) != 0u)' if w else '((s106 & LMASK) != 0u)'
                return ('!' + e) if i.op == 's_cbranch_vccz' else e
            raise ValueError(i)

        flag = lambda b: 'F%05x' % insts[b].off
        n = len(insts)
        out = []
        used_flags = set()
        loop_guards = set()
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
                put('LG_%x = 0u;' % insts[h].off)
                loop_guards.add(insts[h].off)
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
                if guard and valu and k not in guard_need and guard_pure_alu(i) and not text.startswith(('if (EXB', '{')):
                    guard = None            # no lane that is inactive here reads the old value (exec_guards_needed)
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
                # SHADER_SR_STOP_AT=<hex pc>: end the program at this barrier (debugging: the memory state there)
                if os.environ.get('SHADER_SR_STOP_AT') and int(os.environ['SHADER_SR_STOP_AT'], 16) == i.off:
                    put('memoryBarrierBuffer(); return;')
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
                put('if ((%s) && ++LG_%x < LOOP_LIMIT) continue;  // -> %05x' % (
                    cond(k), insts[st.latches[k]].off, insts[st.latches[k]].off))
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
    # loop watchdog (every back edge counts; a loop that would hang the GPU ends after LOOP_LIMIT iterations)
    flags_decl += ''.join('\n    uint LG_%x = 0u;' % off for off in sorted(loop_guards))
    if stage != 'cs':
        return graphics_shader(tr, stage, gfx_if, flags_decl, body, nvgpr, user_sgprs), tr
    lines = [prelude.prelude(threads, tr.lds_dwords, nvgpr, probes)]
    if tr.images is not None:
        lines.append(tr.images.declarations())
    for decl in getattr(tr, 'tables_decl', {}).values():
        lines.append(decl)
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
    # the thread id VGPRs as the hardware sets them (COMPUTE_PGM_RSRC2.TIDIG_COMP_CNT): v0 = x, v1 = y (1+), v2 = z (2+)
    # of the lane's position in the workgroup; lanes are numbered x fastest (wave * 64 + lane)
    tx, ty, tz = shape if shape else (threads, 1, 1)

    def tid(n, h):
        flat = 'gl_SubgroupID * 64u + LANE%d' % h
        if ty == 1 and tz == 1:
            return flat if n == 0 else None
        if n == 0:
            return '(%s) %% %du' % (flat, tx)
        if n == 1 and tidig >= 1:
            return '((%s) / %du) %% %du' % (flat, tx, ty)
        if n == 2 and tidig >= 2:
            return '(%s) / %du' % (flat, tx * ty)
        return None
    for n in sorted(tr.used_v):
        for h in (0, 1):
            lines.append('    uint v%d_%d = %s;' % (n, h, tid(n, h) or '0u'))
    if threads < 64:
        # a workgroup smaller than a wave: one warp, the lanes from the workgroup size on are inactive (the
        # hardware's initial exec)
        lines.append('    EXM = (LANE0 < %du ? 1u : 0u) | (LANE1 < %du ? 2u : 0u); SCC = false;' % (threads, threads))
    else:
        lines.append('    EXM = LMASK; SCC = false;')
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


def embedded_fetches(insts, g):
    """Kyty's embedded vertex fetch (ShaderRecompiler.cpp DetectEmbeddedVertexFetch): the attribute table pointer is
    in s[attrib_reg + 8]; an s_load from it at byte offset R gives attribute (R + 4 k) / 16 for its dword k; the
    buffer_load_format whose V# depends on one attribute's dwords (program order, through scalar instructions and
    SCC) is that attribute's fetch, which Kyty turns into its vertex input. Returns {instruction offset: attribute}."""
    if g is None or g.fetch.get('dynamic_fetch') != '1':
        return {}
    ar = int(g.fetch.get('attrib_reg', '-1'))
    if ar < 0:
        return {}
    tbl = g.user_data_base + ar
    taint, redefined, out = {}, set(), {}
    scc = frozenset()

    def regs_taint(o):
        t = set()
        if o.kind == 'reg' and o.file == 's':
            for k in range(o.count):
                t |= taint.get(o.first + k, frozenset())
        return t
    for i in insts:
        b = i.base
        if b.startswith('s_load_dword') and i.ops and i.ops[0].kind == 'reg':
            d, base, off = i.ops
            imm = int(i.mods.get('offset', '0'), 0)
            if (base.kind == 'reg' and base.first == tbl and tbl not in redefined and tbl + 1 not in redefined and
                    off.kind in ('const', 'null')):
                r = imm + (off.value if off.kind == 'const' else 0)
                for k in range(d.count):
                    taint[d.first + k] = frozenset([(r + 4 * k) // 16])
                    redefined.add(d.first + k)
                continue
            src = regs_taint(base) | regs_taint(off)
            for k in range(d.count):
                taint[d.first + k] = frozenset(src)
                redefined.add(d.first + k)
            continue
        if b.startswith('buffer_load_format_'):
            vs, soff = i.ops[2], i.ops[3]
            src = regs_taint(vs) | regs_taint(soff)
            if len(src) == 1:
                out[i.off] = next(iter(src))
            continue
        if b.startswith('s_'):
            src = set(scc) if b.startswith(('s_cselect', 's_addc', 's_subb', 's_cbranch_scc')) else set()
            for o in i.ops[1:]:
                src |= regs_taint(o)
            if b.startswith(('s_cmp', 's_bitcmp')):
                scc = frozenset(src | regs_taint(i.ops[0]) if i.ops else src)
                continue
            for r in all_sgpr_defs(i):
                n = int(r[1:])
                taint[n] = frozenset(src)
                redefined.add(n)
            continue
        for r in all_sgpr_defs(i):
            n = int(r[1:])
            taint[n] = frozenset()
            redefined.add(n)
    return out


def graphics_shader(tr, stage, g, flags_decl, body, nvgpr, user_end):
    """the GLSL of a graphics stage: Kyty's interface of the module (outputs, builtins), the initial registers as Kyty
    sets them (frontend/translate/Translate.cpp, the stage's entry), the body, and the exports"""
    lines = [prelude.prelude(64, 1, nvgpr, False, stage)]
    if tr.images is not None:
        lines.append(tr.images.declarations())
        for off in sorted(tr.images.heap_loads):
            lines.append('uint64_t TADDR_%x = 0ul;   // the guest address of a heap image\'s T# (s_load at %05x)' % (off, off))
    for decl in getattr(tr, 'tables_decl', {}).values():
        lines.append(decl)
    base = g.user_data_base
    if stage == 'vs':
        clip = 'ClipDistance' in g.builtins_out
        lines.append('out gl_PerVertex { vec4 gl_Position;%s };' % (' float gl_ClipDistance[1];' if clip else ''))
        for n, loc in sorted(tr.param_outputs):
            lines.append('layout(location = %d) out vec4 P%d;' % (loc, n))
        for a, (loc, info) in sorted(tr.vs_inputs.items()):
            t = {'float': 'vec', 'uint': 'uvec', 'int': 'ivec'}[info['type']]
            lines.append('layout(location = %d) in %s ATTR%d;' % (
                loc, t + str(info['n']) if info['n'] > 1 else {'vec': 'float', 'uvec': 'uint', 'ivec': 'int'}[t], a))
        # Kyty: a vertex at the origin (all four components 0, the guest's culled vertex) is clipped
        lines.append('void exp_pos0(uvec4 v) {\n    vec4 p = uintBitsToFloat(v);\n    gl_Position = p;%s\n}' % (
            '\n    gl_ClipDistance[0] = all(equal(p, vec4(0.0))) ? -1.0 : 0.0;' if clip else ''))
        user_count, user_inline, user_off = 'pc.geometry_user_count', 'pc.geometry_user', 0
    elif stage == 'ps':
        quals = {'Flat': 'flat', 'NoPerspective': 'noperspective', 'Centroid': 'centroid', 'Sample': 'sample'}
        scalar = {'vec': 'float', 'uvec': 'uint', 'ivec': 'int'}
        for sloc, info in sorted(tr.ps_inputs.items()):
            q = ' '.join(quals[d] for d in sorted(info['decorations']) if d in quals)
            t = {'float': 'vec', 'uint': 'uvec', 'int': 'ivec'}[info['type']]
            if info['array'] is not None:
                lines.append('layout(location = %d) pervertexEXT in %s IN%d[%d];' % (
                    sloc, t + str(info['n']) if info['n'] > 1 else scalar[t], sloc, info['array']))
                continue
            lines.append('layout(location = %d) %sin %s IN%d;' % (sloc, q + ' ' if q else '',
                                                                  t + str(info['n']) if info['n'] > 1 else scalar[t],
                                                                  sloc))
        for n, info in sorted(tr.ps_outputs.items()):
            t = {'float': 'vec', 'uint': 'uvec', 'int': 'ivec'}[info['type']]
            lines.append('layout(location = %d) out %s OUT%d;' % (n, t + str(info['n']) if info['n'] > 1 else
                                                                  scalar[t], n))
        lines.append('bool PS_VALID = true;    // the pixel valid mask (exp ... vm): the lane is killed when false')
        lines.append('uint quad_any(uint b) {\n'
                     '    return (b | subgroupQuadSwapHorizontal(b) | subgroupQuadSwapVertical(b) | '
                     'subgroupQuadSwapDiagonal(b)) & LMASK;\n}')
        user_count, user_inline, user_off = 'pc.pixel_user_count', 'pc.pixel_user', 32
    else:
        raise ValueError('stage %s' % stage)
    # system SGPRs (NGG vertex shader: s2 = wave size << 12, s3 = merged wave info: every lane of the wave a vertex
    # and a primitive (the guest computes its export masks from it), first wave of the group)
    indirect_v, indirect_i = g.indirect_offsets if g.indirect_draw else (255, 255)
    special = ''.join('\n        if (k == %du) return uint(%s);' % (slot, b) for slot, b in
                      ((indirect_v, 'gl_BaseVertex'), (indirect_i, 'gl_BaseInstance')) if slot != 255)
    lines.append('uint initial_sgpr(uint n) {\n'
                 '    if (n >= %du && n - %du < %s) {\n'
                 '        uint k = n - %du;%s\n'
                 '        return k < %du ? %s[k] : UserTable(pc.user_slot).v[%du + k];\n'
                 '    }\n'
                 '%s'
                 '    return 0u;\n'
                 '}' % (base, base, user_count, base, special, 10 if stage == 'vs' else 8, user_inline, user_off,
                        '    if (n == 2u) return 64u << 12;\n    if (n == 3u) return 64u | (1u << 28);\n'
                        if stage == 'vs' else ''))
    lines.append('void body() {')
    lines.append('    LANE0 = gl_SubgroupInvocationID; LANE1 = LANE0 + 32u;')
    for n in sorted(tr.used_s):
        lines.append('    uint s%d = %s;' % (n, 'initial_sgpr(%du)' % n if n < max(user_end, 16) else '0u'))
    vinit = {}
    if stage == 'vs':
        vinit[5] = 'uint(gl_VertexIndex - gl_BaseVertex)' if g.indirect_draw else 'uint(gl_VertexIndex)'
        vinit[8] = 'uint(gl_InstanceIndex - gl_BaseInstance)' if g.indirect_draw else 'uint(gl_InstanceIndex)'
    exm = 'LMASK'
    if stage == 'ps':
        # Translate.cpp: the barycentric pairs (only read by v_interp here, which reads the inputs instead), then from
        # system_input_base: FragCoord x, y, z, 1 / FragCoord.w (the components the program enabled), front face,
        # ancillary; exec starts as the live mask (no helper lanes)
        px = g.pixel
        reg = int(px.get('system_input_base', '0'))
        for c in px.get('pos', ''):
            vinit[reg] = {'x': 'floatBitsToUint(gl_FragCoord.x)', 'y': 'floatBitsToUint(gl_FragCoord.y)',
                          'z': 'floatBitsToUint(gl_FragCoord.z)', 'w': 'floatBitsToUint(1.0 / gl_FragCoord.w)'}[c]
            reg += 1
        exm = '(gl_HelperInvocation ? 0u : 1u)'
    for n in sorted(tr.used_v):
        lines.append('    uint v%d_0 = %s;' % (n, vinit.get(n, '0u')))
        lines.append('    uint v%d_1 = 0u;' % n)
    lines.append('    EXM = %s; SCC = false;' % exm)
    lines.append(flags_decl)
    lines.append(body)
    lines.append('}')
    lines.append('void main() {')
    lines.append('    FAULT_PAGE = NUM_PAGES;')
    lines.append('    PROBE_N = 0u;')
    lines.append('    SCALARS = %s;' % ('pc.geometry_scalars' if stage == 'vs' else 'pc.pixel_scalars'))
    lines.append('    body();')
    lines.append('    record_faults();')
    if stage == 'ps':
        lines.append('    if (!PS_VALID) discard;')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('code')
    ap.add_argument('out')
    ap.add_argument('--threads', type=int, default=64)
    ap.add_argument('--lds-bytes', type=int, default=2048)
    ap.add_argument('--user-sgprs', type=int, default=2)
    ap.add_argument('--tgid-sgpr', type=int, default=2)
    ap.add_argument('--reach-out', help='write the scalar load chains (scalar_reach) to this file')
    ap.add_argument('--bypass-out', help="write the emulator's bypass declaration (store_vsharps: every V# the program "
                                         "reads and writes through) to this file, or remove it when the program "
                                         "reaches memory the emulator could not follow")
    ap.add_argument('--probes', action='store_true', help='compile the probes (pc.debug) in')
    ap.add_argument('--resources', help="Kyty's images and samplers of the program's specialisation (.res dump)")
    ap.add_argument('--shape', help='workgroup shape x,y,z (default: threads,1,1)')
    ap.add_argument('--tidig', type=int, default=0, help='thread id components in v0..v2 (TIDIG_COMP_CNT)')
    ap.add_argument('--stage', default='cs', help='cs, vs or ps')
    ap.add_argument('--gres', help="graphics: Kyty's module record (<STAGE>_<hash>_<id>.gres; its .spv beside it)")
    a = ap.parse_args()
    shape = tuple(int(x) for x in a.shape.split(',')) if a.shape else None
    gfx_if = None
    if a.stage != 'cs':
        import gfx
        spv = a.gres[:-5] + '.spv'
        gfx_if = gfx.load(a.gres, spv if os.path.exists(spv) else None)
    glsl, tr = translate_shader(a.code, a.threads, a.lds_bytes, a.user_sgprs, a.tgid_sgpr, a.probes, a.resources,
                                shape, a.tidig, a.stage, gfx_if)
    open(a.out, 'w').write(glsl)
    if a.reach_out:
        reach = tr.reach
        if gfx_if is not None:
            # graphics: the emulator resolves 'user' roots from the stage's user data (s[user_data_base] on)
            base = gfx_if.user_data_base
            if any(kind == 'user' and x < base for kind, x, off, extent in reach):
                reach = None          # a chain from a system SGPR: no snapshot (the page table serves the loads)
            else:
                reach = [(kind, x - base if kind == 'user' else x, off, extent) for kind, x, off, extent in reach]
        if reach is None:
            if os.path.exists(a.reach_out):
                os.remove(a.reach_out)
        else:
            with open(a.reach_out, 'w') as f:
                for kind, x, off, extent in reach:
                    f.write('%s %d %d %d\n' % (kind, x, off, extent))
    if a.bypass_out:
        insts = gcnisa.decode(a.code)
        stores = store_vsharps(insts, a.user_sgprs)
        loads = store_vsharps(insts, a.user_sgprs, loads=True)
        unknown = [off for off, chain in stores + loads if chain is None]
        if unknown or not stores:
            if os.path.exists(a.bypass_out):
                os.remove(a.bypass_out)
            tr.notes.append('bypass: no (%s)' % ('no stores' if not stores else
                                                 'memory through unknown V#s at ' + ', '.join(
                                                     '0x%x' % off for off in unknown[:4])))
        else:
            writes = sorted(set(chain for off, chain in stores))
            reads = sorted(set(chain for off, chain in loads))
            with open(a.bypass_out, 'w') as f:
                for chain in reads:
                    f.write('read %s\n' % ' '.join(str(x) for x in chain))
                for chain in writes:
                    f.write('write %s\n' % ' '.join(str(x) for x in chain))
            tr.notes.append('bypass: reads through %d V#s, %d stores through %d V#s: %s' % (
                len(reads), len(stores), len(writes),
                ', '.join('s%d%s' % (c[0], ''.join('->+0x%x' % o for o in c[1:])) for c in writes)))
    for n in tr.notes:
        print('note:', n)
    if tr.unknown:
        print('UNHANDLED:', ', '.join('%s x%d' % kv for kv in tr.unknown.most_common()))


import prelude  # noqa: E402

if __name__ == '__main__':
    main()
