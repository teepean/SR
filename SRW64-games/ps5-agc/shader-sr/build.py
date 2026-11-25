#!/usr/bin/env python3
# build.py <out dir> <name>=<code.bin>[:threads[:lds bytes[:res,...]]] ... - translates guest compute programs for the
# Kyty clone's translated-shader path (KYTY_TRANSLATED_DIR): <out dir>/<name>.spv (gcn2glsl.py, glslc, spirv-opt), or
# with Kyty's resource dumps (.res, programs with images) one <name>.<resource key>.spv per specialisation,
# <name>.code (the guest code bytes the translation was made from, up to the last instruction; Kyty uses a
# translation only for a dispatch whose code matches them), <name>.reach (the scalar load chains, see
# gcn2glsl.scalar_reach), with SHADER_SR_BYPASS=1 <name>.bypass for a program whose memory the emulator can follow
# (gcn2glsl.store_vsharps; KYTY_TRANSLATED_BYPASS), and compare.spv for the check mode.
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gcnisa  # noqa: E402


class Failed(Exception):
    pass


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise Failed('%s failed:\n%s%s' % (' '.join(cmd), r.stdout[-3000:], r.stderr[-3000:]))
    return r.stdout


def compile_spirv(comp, raw, opt, out):
    run(['glslc', '--target-env=vulkan1.3', '-O0', '-o', raw, comp])
    run(['spirv-opt', '-O', '--compact-ids', '--max-id-bound=268435455', raw, '-o', opt])
    # RDNA rounds a multiply and an add separately: no FMA contraction by the driver
    run([sys.executable, os.path.join(HERE, 'nocontract.py'), opt, out])
    run(['spirv-val', '--target-env', 'vulkan1.3', out])


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
        resources = [r for r in parts[3].split(',') if r] if len(parts) > 3 else []
        # Kyty's dump next to the code (<hash>.txt): workgroup shape and thread id components
        shape_args = []
        dump_txt = os.path.splitext(code)[0] + '.txt'
        if os.path.exists(dump_txt):
            info = {l.split()[0]: l.split()[1:] for l in open(dump_txt) if l.split()}
            if 'threads' in info:
                shape_args += ['--shape', ','.join(info['threads'][:3])]
            if 'tidig_comp_cnt' in info:
                shape_args += ['--tidig', info['tidig_comp_cnt'][0]]
        insts = gcnisa.decode(code)
        end = insts[-1].off + insts[-1].size
        reach_new = os.path.join(out, name + '.reach.new')
        bypass_new = os.path.join(out, name + '.bypass.new')
        variants = [(None, None)] if not resources else [(r, open(r).readline().split()[3]) for r in resources]
        built = 0
        for res, key in variants:
            label = name if key is None else '%s.%s' % (name, key)
            comp = os.path.join(work, label + '.comp')
            try:
                notes = run([sys.executable, os.path.join(HERE, 'gcn2glsl.py'), code, comp, '--threads', str(threads),
                             '--lds-bytes', str(lds), '--reach-out', reach_new] +
                            (['--bypass-out', bypass_new] if os.environ.get('SHADER_SR_BYPASS') else []) +
                            (['--resources', res] if res else []) + shape_args +
                            (['--probes'] if os.environ.get('SHADER_SR_PROBES') else [])).strip()
                print(notes)
                if 'UNHANDLED' in notes:
                    # a translation with untranslated instructions computes something else: never installed
                    raise Failed([line for line in notes.splitlines() if 'UNHANDLED' in line][0])
                spv_new = os.path.join(out, label + '.spv.new')
                compile_spirv(comp, os.path.join(work, label + '.spv'), os.path.join(work, label + '_opt.spv'), spv_new)
            except Failed as e:
                print('%s: not built: %s' % (label, str(e).strip().splitlines()[-1][:300]))
                continue
            os.replace(spv_new, os.path.join(out, label + '.spv'))
            built += 1
            print('%s: %d instructions, %d bytes of guest code, %d bytes of SPIR-V' % (
                label, len(insts), end, os.path.getsize(os.path.join(out, label + '.spv'))))
        if built:
            code_new = os.path.join(out, name + '.code.new')
            with open(code, 'rb') as f, open(code_new, 'wb') as g:
                g.write(f.read()[:end])
            os.replace(code_new, os.path.join(out, name + '.code'))
            os.replace(reach_new, os.path.join(out, name + '.reach'))
            # SHADER_SR_BYPASS=1: <name>.bypass (gcn2glsl.store_vsharps) for a program whose memory all goes through
            # V#s with exact chains. Opt-in: a declared V# is marked GPU-written over its whole range, which is wrong
            # where the range also holds CPU-owned data the program does not write (then use an exact walker)
            if os.path.exists(bypass_new):
                os.replace(bypass_new, os.path.join(out, name + '.bypass'))
    cmp_new = os.path.join(out, 'compare.spv.new')
    try:
        run(['glslc', '--target-env=vulkan1.3', '-O', '-o', cmp_new, os.path.join(HERE, 'compare.comp')])
    except Failed as e:
        sys.exit(str(e))
    os.replace(cmp_new, os.path.join(out, 'compare.spv'))


if __name__ == '__main__':
    main()
