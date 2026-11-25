#!/usr/bin/env python3
# bypass_decl.py <code.bin> <out.bypass> [--user-sgprs N] - the emulator's bypass declaration (<name>.bypass, see
# gcn2glsl.store_vsharps) of a guest program, for a native replacement (native.py) of it: the native program writes
# what the guest program writes. Removes <out.bypass> when the guest program's writes cannot be followed.
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
    a = ap.parse_args()
    stores = gcn2glsl.store_vsharps(gcnisa.decode(a.code), a.user_sgprs)
    unknown = [off for off, chain in stores if chain is None]
    if unknown or not stores:
        if os.path.exists(a.out):
            os.remove(a.out)
        print('%s: no bypass (%s)' % (a.out, 'no stores' if not stores else 'stores through unknown V#s at ' +
                                      ', '.join('0x%x' % off for off in unknown[:4])))
        return
    chains = sorted(set(chain for off, chain in stores))
    with open(a.out + '.new', 'w') as f:
        f.write('sync full\n')
        for chain in chains:
            f.write('write %s\n' % ' '.join(str(x) for x in chain))
    os.replace(a.out + '.new', a.out)
    print('%s: %d stores through %d V#s' % (a.out, len(stores), len(chains)))


if __name__ == '__main__':
    main()
