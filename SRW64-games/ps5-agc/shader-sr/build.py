#!/usr/bin/env python3
# build.py <out dir> <name>=<code.bin>[:threads[:lds bytes]] ... - translates guest compute programs for the Kyty
# clone's translated-shader path (KYTY_TRANSLATED_DIR): <out dir>/<name>.spv (gcn2glsl.py, glslc, spirv-opt),
# <name>.code (the guest code bytes the translation was made from, up to the last instruction; Kyty uses a
# translation only for a dispatch whose code matches them), <name>.reach (the scalar load chains, see
# gcn2glsl.scalar_reach), and compare.spv for the check mode.
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gcnisa  # noqa: E402


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit('%s failed:\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__ or 'usage: build.py <out dir> <name>=<code.bin>[:threads[:lds bytes]] ...')
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, 'work')
    os.makedirs(work, exist_ok=True)
    for spec in sys.argv[2:]:
        name, _, rest = spec.partition('=')
        parts = rest.split(':')
        code, threads = parts[0], int(parts[1]) if len(parts) > 1 else 64
        lds = int(parts[2]) if len(parts) > 2 else 2048
        insts = gcnisa.decode(code)
        end = insts[-1].off + insts[-1].size
        comp = os.path.join(work, name + '.comp')
        reach_new = os.path.join(out, name + '.reach.new')
        print(run([sys.executable, os.path.join(HERE, 'gcn2glsl.py'), code, comp, '--threads', str(threads),
                   '--lds-bytes', str(lds), '--reach-out', reach_new]).strip())
        raw = os.path.join(work, name + '.spv')
        run(['glslc', '--target-env=vulkan1.3', '-O0', '-o', raw, comp])
        spv_new = os.path.join(out, name + '.spv.new')
        opt = os.path.join(work, name + '_opt.spv')
        run(['spirv-opt', '-O', '--max-id-bound=268435455', raw, '-o', opt])
        # RDNA rounds a multiply and an add separately: no FMA contraction by the driver
        run([sys.executable, os.path.join(HERE, 'nocontract.py'), opt, spv_new])
        run(['spirv-val', '--target-env', 'vulkan1.3', spv_new])
        code_new = os.path.join(out, name + '.code.new')
        with open(code, 'rb') as f, open(code_new, 'wb') as g:
            g.write(f.read()[:end])
        os.replace(spv_new, os.path.join(out, name + '.spv'))
        os.replace(code_new, os.path.join(out, name + '.code'))
        os.replace(reach_new, os.path.join(out, name + '.reach'))
        print('%s: %d instructions, %d bytes of guest code, %d bytes of SPIR-V' % (
            name, len(insts), end, os.path.getsize(os.path.join(out, name + '.spv'))))
    cmp_new = os.path.join(out, 'compare.spv.new')
    run(['glslc', '--target-env=vulkan1.3', '-O', '-o', cmp_new, os.path.join(HERE, 'compare.comp')])
    os.replace(cmp_new, os.path.join(out, 'compare.spv'))


if __name__ == '__main__':
    main()
