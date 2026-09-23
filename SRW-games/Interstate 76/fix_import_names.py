#! /usr/bin/python3
# Renames imported symbols whose names clash with x86 instruction mnemonics (e.g. MSVCRT "div"),
# so that the %define in imports.inc doesn't also rewrite the instructions.
# usage: fix_import_names.py <module.asm> <seg*.inc...>   (files are rewritten in place)
import re, sys

CLASHES = {'div': 'msvcrt_div'}

for fname in sys.argv[1:]:
    lines = open(fname).read().split('\n')
    changed = 0
    for i, l in enumerate(lines):
        for name, new in CLASHES.items():
            pat = [
                (r'^extern %s( ;.*)?$' % name, r'extern %s\1' % new),
                (r'^(call|jmp|dd|push|CALL|PUSH32) %s$' % name, r'\1 %s' % new),
                (r'^(\w+ \w+, )%s$' % name, r'\1%s' % new),
            ]
            for p, r in pat:
                nl = re.sub(p, r, l)
                if nl != l:
                    lines[i] = l = nl
                    changed += 1
    if changed:
        open(fname, 'w').write('\n'.join(lines))
        print('%s: %d import references renamed' % (fname, changed))
