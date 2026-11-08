#!/usr/bin/env python3
"""mkiflags.py: instruction_flags.sci entries for SRW's llasm output (OUTPUT_TYPE OUT_LLASM).

llasm has no flags register: SRW computes flags only where an instruction needs them. A label whose code reads flags
set before the jump to it ("Error: unknown flags on start of region": MSVC's `jne L ... L: sbb eax, eax`, `jb L ...
L: jz M`) needs an entry: at the label the flags come from outside (clear), and every instruction that reaches the
label - each jump to it and the instruction before it, when it falls through - writes them (set).

  mkiflags.py <module (PE)> <srw.err from SRW_LIST_ERRORS=1> [<instruction_flags.sci so far>] > instruction_flags.sci

After a call the flags come from the callee (MSVC's CRT _fload returns ZF): then its ret instructions write them.
A label that is also called (the CRT's _CIacos: `call _fload; call L` ... `L: push edx; fstcw; je`) gets its flags
from before each call too.

The jumps come from a linear disassembly of the code sections (objdump). Run SRW again with the result: an entry can
move the need to an earlier label (the jump's own region), which the next run reports - repeat until none is left.
"""
import re, subprocess, sys

FLAG_NAMES = [(0x01, 'CF'), (0x02, 'PF'), (0x04, 'AF'), (0x08, 'ZF'), (0x10, 'SF'), (0x20, 'OF')]

def names(f):
    return '|'.join(n for b, n in FLAG_NAMES if f & b) or '0'

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pe, err = sys.argv[1], sys.argv[2]
    entries = {}            # address -> [set, clear, comments]
    if len(sys.argv) > 3 and sys.argv[3] != '-':
        for line in open(sys.argv[3]):
            m = re.match(r'\s*loc_([0-9A-Fa-f]+)\s*,\s*([^,]+),\s*([^;]+)(;.*)?', line)
            if m:
                e = entries.setdefault(int(m.group(1), 16), [0, 0, []])
                e[0] |= int(m.group(2), 0)
                e[1] |= int(m.group(3), 0)
                if m.group(4):
                    e[2].append(m.group(4)[1:].strip())

    sites = []
    for line in open(err, errors='replace'):
        m = re.search(r'Error: unknown flags on start of region - \d+ - \d+ \(0x([0-9a-f]+)\) - 0x([0-9a-f]+)', line)
        if m:
            sites.append((int(m.group(1), 16), int(m.group(2), 16)))
    if not sites:
        print('mkiflags: no flags errors in ' + err, file=sys.stderr)
    # the labels of the existing entries again (what writes their flags is derived anew: new rules apply to them)
    sites += [(a, e[1]) for a, e in sorted(entries.items()) if e[1] and (a, e[1]) not in sites]

    # linear disassembly of the code: every instruction's address, the jumps' targets
    out = subprocess.run(['objdump', '-d', '--no-show-raw-insn', pe], capture_output=True, text=True).stdout
    insns = []              # (address, mnemonic, target or None)
    for line in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', line)
        if not m:
            continue
        a, mn, ops = int(m.group(1), 16), m.group(2), m.group(3)
        t = None
        if mn.startswith('j') or mn.startswith('loop') or mn.startswith('call'):
            tm = re.match(r'(0x)?([0-9a-f]+)\b', ops.strip())
            if tm and not ops.strip().startswith('*'):
                t = int(tm.group(2), 16)
        insns.append((a, mn, t))
    index = {a: i for i, (a, mn, t) in enumerate(insns)}
    sources, callers = {}, {}
    for a, mn, t in insns:
        if t is not None:
            (callers if mn.startswith('call') else sources).setdefault(t, []).append(a)

    def returns(entry):
        # the ret instructions of the function at entry (following its jumps)
        rets, todo, seen = [], [entry], set()
        while todo and len(seen) < 5000:
            i = index.get(todo.pop())
            while i is not None and i not in seen and i < len(insns):
                seen.add(i)
                a, mn, t = insns[i]
                if mn.startswith('ret'):
                    rets.append(a)
                    break
                if mn.startswith('jmp'):
                    if t is not None:
                        todo.append(t)
                    break
                if t is not None and not mn.startswith('call'):
                    todo.append(t)
                i += 1
        return rets

    for site, flags in sites:
        e = entries.setdefault(site, [0, 0, []])
        e[1] |= flags
        e[2].append('%s from the jumps to it' % names(flags))
        preds = list(sources.get(site, []))

        def before(a, what):
            # the instruction that runs before the one at a (falls through to it) writes the flags; a call: its rets
            i = index.get(a)
            if i is None or i == 0:
                print('mkiflags: 0x%x is not an instruction start in the linear disassembly' % a, file=sys.stderr)
                return
            pa, pmn, pt = insns[i - 1]
            if pmn.startswith('call'):
                # after a call the flags come from the callee (MSVC's _fload returns ZF): its rets write them
                if pt is None:
                    print('mkiflags: 0x%x follows an indirect call' % a, file=sys.stderr)
                else:
                    rets = returns(pt)
                    if not rets:
                        print('mkiflags: no ret found in 0x%x (called before 0x%x)' % (pt, a), file=sys.stderr)
                    preds.extend(rets)
            elif not (pmn.startswith('jmp') or pmn.startswith('ret') or pmn in ('int3', 'hlt')):
                preds.append(pa)
            elif what == 'call':
                print('mkiflags: nothing before the call at 0x%x writes the flags of 0x%x' % (a, site), file=sys.stderr)

        before(site, 'label')
        # a label that is also called: the flags come from before each call (the call itself keeps them)
        for c in callers.get(site, []):
            before(c, 'call')
        if not preds:
            print('mkiflags: nothing reaches 0x%x' % site, file=sys.stderr)
        for p in preds:
            pe_ = entries.setdefault(p, [0, 0, []])
            pe_[0] |= flags
            pe_[2].append('%s for loc_%X' % (names(flags), site))

    print('; generated by tools/mkiflags.py: flags across labels for the llasm output')
    for a in sorted(entries):
        s, c, com = entries[a]
        seen = []
        for x in com:
            if x not in seen:
                seen.append(x)
        print('loc_%X,0x%02x,0x%02x ; %s' % (a, s, c, '; '.join(seen)))
    print('mkiflags: %d sites, %d entries' % (len(sites), len(entries)), file=sys.stderr)

main()
