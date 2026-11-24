#!/usr/bin/env python3
# native.py <out.spv> <body.glsl> [--threads N] [-D NAME=VALUE ...] - builds a native (hand-written) replacement of a
# guest compute program: the GLSL body uses the shader-sr runtime (prelude.py: guest memory through the BDA page
# table, V# access with format decoding, push constants and batched user data as the emulator's translated-shader
# path passes them), so the result drops in where a translation of the guest program would (KYTY_TRANSLATED_DIR
# <name>.spv next to the guest code <name>.code). Compiled like the translations: glslc, spirv-opt, nocontract.py
# (float multiplies and adds rounded separately, as RDNA's v_mul / v_add / v_mad / v_mac; write fma() where the
# guest fuses), spirv-val.
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build  # noqa: E402
import prelude  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('body')
    ap.add_argument('--threads', type=int, default=1024, help='host threads per workgroup')
    ap.add_argument('-D', action='append', default=[], help='NAME=VALUE defines for the body')
    a = ap.parse_args()
    # the runtime for a workgroup of a.threads host threads (prelude() halves its argument: lane pairs)
    text = prelude.prelude(2 * a.threads, 1, 1, False)
    # extensions the native bodies use beyond the translator's
    text = text.replace('#version 460\n', '#version 460\n#extension GL_KHR_shader_subgroup_arithmetic : require\n', 1)
    defines = ''.join('#define %s %s\n' % tuple((d.split('=', 1) + ['1'])[:2]) for d in a.D)
    body = open(a.body).read()
    work = os.path.join(os.path.dirname(os.path.abspath(a.out)) or '.', 'work')
    os.makedirs(work, exist_ok=True)
    stem = os.path.splitext(os.path.basename(a.out))[0]
    comp = os.path.join(work, stem + '.comp')
    with open(comp, 'w') as f:
        f.write(text + '\n' + defines + '#line 1\n' + body)
    try:
        build.compile_spirv(comp, os.path.join(work, stem + '.raw.spv'), os.path.join(work, stem + '_opt.spv'),
                            a.out + '.new')
    except build.Failed as e:
        sys.exit(str(e))
    os.replace(a.out + '.new', a.out)
    print('%s: %d bytes of SPIR-V' % (a.out, os.path.getsize(a.out)))


if __name__ == '__main__':
    main()
