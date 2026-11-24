#!/usr/bin/env python3
# rdnaflow.py <code.bin> [--blocks] - control-flow / lane-structure analysis of a PS5 (gfx1013) compute shader for a
# lane-per-thread translation: basic blocks, loops, exec-mask writes, branches on exec/vcc, scalar side effects of the
# code those branches skip, cross-lane operations, LDS accesses and barriers.
import re, subprocess, sys, collections

def disassemble(path):
    data = open(path, 'rb').read()
    text = ' '.join('0x%02x' % b for b in data)
    out = subprocess.run(['llvm-mc', '-arch=amdgcn', '-mcpu=gfx1013', '-disassemble', '-show-encoding'], input=text,
                         capture_output=True, text=True).stdout
    insts, off = [], 0
    for line in out.split('\n'):
        m = re.search(r'encoding: \[(.*?)\]', line)
        if not m:
            continue
        n = len(m.group(1).split(','))
        text = line.split(';')[0].strip()
        insts.append(Inst(off, text, n))
        off += n
        if text.startswith('s_endpgm'):
            break
    return insts

REG = re.compile(r'^(-?)(\|?)(sext\()?([sv])(\[(\d+):(\d+)\]|(\d+))\)?\|?$')
SPECIAL = {'vcc': ('vcc', 2), 'vcc_lo': ('vcc', 1), 'vcc_hi': ('vcc_hi', 1), 'exec': ('exec', 2), 'exec_lo': ('exec', 1),
           'exec_hi': ('exec_hi', 1), 'm0': ('m0', 1), 'scc': ('scc', 1)}

def regs_of(token):
    """register names (s12, v3, vcc, exec, m0) referenced by one operand token"""
    token = token.strip().rstrip(',')
    for mod in ('neg(', 'abs(', '|'):
        token = token.replace(mod, '')
    token = token.replace(')', '').replace('-', '').replace('sext(', '')
    if token in SPECIAL:
        name, width = SPECIAL[token]
        if name == 'vcc' and width == 2:
            return ['vcc_lo', 'vcc_hi']
        if name == 'exec' and width == 2:
            return ['exec_lo', 'exec_hi']
        return {'vcc': ['vcc_lo'], 'exec': ['exec_lo']}.get(name, [name])
    m = re.match(r'^([sv])\[(\d+):(\d+)\]$', token)
    if m:
        return ['%s%d' % (m.group(1), i) for i in range(int(m.group(2)), int(m.group(3)) + 1)]
    m = re.match(r'^([sv])(\d+)$', token)
    if m:
        return [token]
    return []

class Inst:
    def __init__(self, off, text, size):
        self.off, self.text, self.size = off, text, size
        parts = text.split(None, 1)
        self.op = parts[0]
        self.args = [a.strip() for a in self.split_args(parts[1])] if len(parts) > 1 else []
        self.target = None
        m = re.match(r'(s_branch|s_cbranch_\w+)$', self.op)
        if m and self.args:
            rel = int(self.args[0], 0)
            rel = rel - 65536 if rel > 32767 else rel
            self.target = off + 4 + rel * 4
        self.defs, self.uses = self.def_use()

    @staticmethod
    def split_args(s):
        args, depth, cur = [], 0, ''
        for ch in s:
            if ch in '([':
                depth += 1
            elif ch in ')]':
                depth -= 1
            if ch == ',' and depth == 0:
                args.append(cur); cur = ''
            else:
                cur += ch
        if cur.strip():
            args.append(cur)
        # drop trailing modifiers that are space separated (offset:16 glc ...)
        out = []
        for a in args:
            out.append(a.split()[0] if a.split() else a)
        return out

    def def_use(self):
        op, a = self.op, self.args
        defs, uses = [], []
        regs = [regs_of(x) for x in a]
        if op.startswith(('s_branch', 's_waitcnt', 's_barrier', 's_nop', 's_endpgm', 's_inst_prefetch', 's_sleep',
                          's_dcache', 's_icache', 's_setprio', 's_sendmsg', 's_code_end', 's_clause')):
            pass
        elif op.startswith('s_cbranch_'):
            cond = op[len('s_cbranch_'):]
            uses += {'scc0': ['scc'], 'scc1': ['scc'], 'vccz': ['vcc_lo', 'vcc_hi'], 'vccnz': ['vcc_lo', 'vcc_hi'],
                     'execz': ['exec_lo', 'exec_hi'], 'execnz': ['exec_lo', 'exec_hi']}.get(cond, [])
        elif op.startswith(('s_cmp', 's_bitcmp')):
            uses += sum(regs, []); defs += ['scc']
        elif op.startswith(('s_load', 's_buffer_load')):
            defs += regs[0]; uses += sum(regs[1:], [])
        elif op.startswith('s_'):
            if regs:
                defs += regs[0]; uses += sum(regs[1:], [])
            if op.startswith(('s_cselect', 's_cmov', 's_addc', 's_subb', 's_cbranch')):
                uses.append('scc')
            if not op.startswith(('s_mov', 's_cselect', 's_cmov', 's_movk', 's_getpc', 's_setpc', 's_swappc')):
                defs.append('scc')
            if 'saveexec' in op:
                defs += ['exec_lo', 'exec_hi']; uses += ['exec_lo', 'exec_hi']
        elif op.startswith('v_cmpx'):
            defs += ['exec_lo', 'exec_hi']; uses += sum(regs, []) + ['exec_lo', 'exec_hi']
        elif op.startswith('v_cmp'):
            defs += regs[0] if regs else []; uses += sum(regs[1:], []) + ['exec_lo', 'exec_hi']
        elif op.startswith('v_readlane') or op.startswith('v_readfirstlane'):
            defs += regs[0]; uses += sum(regs[1:], []) + ['exec_lo', 'exec_hi']
        elif op.startswith(('buffer_store', 'global_store', 'ds_write', 'ds_store', 'buffer_atomic', 'global_atomic',
                            'ds_add', 'ds_min', 'ds_max', 'ds_and', 'ds_or', 'ds_xor', 'ds_inc', 'ds_dec')):
            uses += sum(regs, []) + ['exec_lo', 'exec_hi']
        elif op.startswith(('buffer_load', 'global_load', 'ds_read', 'ds_load', 'image_')):
            defs += regs[0] if regs else []; uses += sum(regs[1:], []) + ['exec_lo', 'exec_hi']
        elif op.startswith('v_'):
            if regs:
                defs += regs[0]; uses += sum(regs[1:], [])
            uses += ['exec_lo', 'exec_hi']
            if op.startswith(('v_cndmask', 'v_addc', 'v_subb', 'v_add_co_ci', 'v_sub_co_ci', 'v_subrev_co_ci')) and \
               not any('s' == x[:1] or x.startswith('vcc') for x in a[3:4]):
                uses += ['vcc_lo', 'vcc_hi']
            if op.startswith(('v_add_co_u32', 'v_sub_co_u32', 'v_subrev_co_u32', 'v_add_co_ci', 'v_sub_co_ci')) and \
               op.endswith('_e32'):
                defs += ['vcc_lo', 'vcc_hi']
        return defs, uses

def cross_lane(inst):
    t = inst.text
    if '_dpp' in inst.op or 'row_' in t or 'quad_perm' in t or 'dpp8' in t:
        return 'dpp'
    if inst.op.startswith(('v_permlane', 'v_readlane', 'v_writelane', 'v_readfirstlane', 'ds_swizzle', 'ds_bpermute',
                           'ds_permute', 'v_mbcnt', 's_ff1', 's_ff0', 's_bcnt', 's_flbit')):
        return inst.op
    return None

def mask_as_data(inst):
    """a lane mask (vcc/exec or an SGPR pair holding one) read as a value by a VALU instruction"""
    if inst.op.startswith('v_') and not inst.op.startswith(('v_cmp', 'v_cndmask', 'v_addc', 'v_subb', 'v_add_co',
                                                            'v_sub_co', 'v_subrev_co')):
        explicit = sum((regs_of(x) for x in inst.args[1:]), [])
        return any(x in ('vcc_lo', 'vcc_hi', 'exec_lo', 'exec_hi') for x in explicit)
    return False

def main():
    insts = disassemble(sys.argv[1])
    by_off = {i.off: k for k, i in enumerate(insts)}
    leaders = {0}
    for k, i in enumerate(insts):
        if i.target is not None:
            leaders.add(i.target)
            if k + 1 < len(insts):
                leaders.add(insts[k + 1].off)
    leaders = sorted(x for x in leaders if x in by_off)
    blocks = []
    for n, start in enumerate(leaders):
        end = leaders[n + 1] if n + 1 < len(leaders) else insts[-1].off + insts[-1].size
        blocks.append([i for i in insts if start <= i.off < end])
    print('instructions %d, basic blocks %d' % (len(insts), len(blocks)))
    ops = collections.Counter()
    for i in insts:
        kind = cross_lane(i)
        if kind:
            ops['cross-lane ' + kind] += 1
        if mask_as_data(i):
            ops['lane mask read as data (%s)' % i.op] += 1
        if 'exec_lo' in i.defs:
            ops['exec write: ' + re.sub(r'_e(32|64)$', '', i.op)] += 1
        if i.op.startswith('s_cbranch'):
            ops['branch ' + i.op[10:] + (' (backward)' if i.target <= i.off else '')] += 1
        if i.op.startswith('ds_'):
            ops['LDS ' + ('write/atomic' if i.op.startswith(('ds_write', 'ds_add', 'ds_min', 'ds_max', 'ds_and',
                                                             'ds_or', 'ds_xor')) else 'read')] += 1
        if i.op == 's_barrier':
            ops['s_barrier'] += 1
    for k, v in sorted(ops.items()):
        print('  %5d  %s' % (v, k))

    # forward exec/vcc-zero branches: does the skipped code write an SGPR (or scc) that is read later without being
    # redefined first? Then the skip is not just an optimisation: a lane-per-thread translation needs the wave-wide
    # "any lane active" decision there.
    def live_after(start_index, regs):
        """registers of `regs` read before being written, scanning linearly from start_index (approximation: follows
        the fall-through order, stops at s_endpgm)"""
        pending, found = set(regs), set()
        for i in insts[start_index:]:
            for r in i.uses:
                if r in pending:
                    found.add(r); pending.discard(r)
            for r in i.defs:
                pending.discard(r)
            if not pending:
                break
        return found
    # SGPRs written as lane masks (per-lane bits: become per-thread booleans in a lane-per-thread translation) vs
    # scalar values (loop counters, pointers, loaded constants: wave-uniform state)
    MASK_OPS = ('v_cmp', 's_and_b64', 's_or_b64', 's_xor_b64', 's_andn1_b64', 's_andn2_b64', 's_nand_b64',
                's_nor_b64', 's_xnor_b64', 's_orn1_b64', 's_orn2_b64', 's_not_b64', 's_and_saveexec_b64',
                's_or_saveexec_b64', 's_andn1_saveexec_b64', 's_andn2_saveexec_b64', 's_orn2_saveexec_b64',
                's_cselect_b64', 's_mov_b64')
    def is_mask_def(inst):
        if inst.op.startswith('v_cmp') and not inst.op.startswith('v_cmpx'):
            return True
        return inst.op in MASK_OPS or inst.op.startswith(MASK_OPS[1:])
    def value_defs(inst):
        if is_mask_def(inst):
            return [r for r in inst.defs if r == 'scc']
        return [r for r in inst.defs if r.startswith('s') or r == 'm0']
    pure, scalar_effect, loops = 0, [], 0
    for k, i in enumerate(insts):
        if not i.op.startswith(('s_cbranch_execz', 's_cbranch_vccz', 's_cbranch_execnz', 's_cbranch_vccnz')):
            continue
        if i.target <= i.off:
            loops += 1
            continue
        skipped = insts[k + 1:by_off.get(i.target, len(insts))]
        sdefs = set()
        for s in skipped:
            for r in value_defs(s):
                sdefs.add(r)
        live = live_after(by_off.get(i.target, len(insts)), sdefs) if sdefs else set()
        if live:
            scalar_effect.append((i.off, i.op, i.target, sorted(live)))
        else:
            pure += 1
    print('forward exec/vcc branches: %d pure skips (lane-local if), %d with live scalar effects, %d backward' %
          (pure, len(scalar_effect), loops))
    for off, op, target, live in scalar_effect[:40]:
        print('    %05x %-18s -> %05x  live SGPRs written in the skipped code: %s' % (off, op, target,
                                                                                     ' '.join(live[:8])))
    if '--blocks' in sys.argv:
        for b in blocks:
            print('B %05x..%05x %d insts, last: %s' % (b[0].off, b[-1].off, len(b), b[-1].text))

main()
