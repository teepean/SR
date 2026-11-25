#!/usr/bin/env python3
# bypass_decl.py <code.bin> <out.bypass> [--user-sgprs N] [--walker "cloth 0|1"] - the emulator's bypass declaration
# (<name>.bypass, see gcn2glsl.store_vsharps) of a guest program, also for a native replacement (native.py) of it,
# which reaches the memory the guest program reaches. Removes <out.bypass> when the guest program's memory cannot be
# followed. --walker: the emulator's own exact walker instead (the cloth solver: its collider sets are reached at
# run-time offsets, and its palette writes cover only some entries of V#s that hold CPU-owned bone matrices).
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gcn2glsl  # noqa: E402
import gcnisa  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('code')
    ap.add_argument('out')
    ap.add_argument('--user-sgprs', type=int, default=2)
    ap.add_argument('--walker', help='declare the emulator walker for the program instead (e.g. "cloth 1")')
    a = ap.parse_args()
    insts = gcnisa.decode(a.code)
    stores = gcn2glsl.store_vsharps(insts, a.user_sgprs)
    loads = gcn2glsl.store_vsharps(insts, a.user_sgprs, loads=True)
    if a.walker:
        # the emulator's own walker for this program (exact ranges, dynamic tables)
        with open(a.out + '.new', 'w') as f:
            f.write('walker %s\n' % a.walker)
        os.replace(a.out + '.new', a.out)
        print('%s: walker %s' % (a.out, a.walker))
        return
    unknown = [off for off, chain in stores + loads if chain is None]
    if unknown or not stores:
        if os.path.exists(a.out):
            os.remove(a.out)
        print('%s: no bypass (%s)' % (a.out, 'no stores' if not stores else 'memory through unknown V#s at ' +
                                      ', '.join('0x%x' % off for off in unknown[:4])))
        return
    with open(a.out + '.new', 'w') as f:
        for chain in sorted(set(chain for off, chain in loads)):
            f.write('read %s\n' % ' '.join(str(x) for x in chain))
        for chain in sorted(set(chain for off, chain in stores)):
            f.write('write %s\n' % ' '.join(str(x) for x in chain))
    os.replace(a.out + '.new', a.out)
    print('%s: %d stores, %d loads through known V#s' % (a.out, len(stores), len(loads)))


if __name__ == '__main__':
    main()
