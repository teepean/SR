#!/usr/bin/env python3
"""prefix_exports.py <module> <exports.txt> <module.asm> <seg*.inc...>
SRW names exported functions by their export names; modules export the same names (SetInstanceHandle, ...),
so they are renamed <module>_<name> in the generated files (in place); llasm's C wrappers c_<name> c_<module>_<name>."""
import re, sys
mod, lst = sys.argv[1:3]
names = [l.strip() for l in open(lst) if l.strip() and not l.startswith('#')]
pat = re.compile(r'\b(c_)?(' + '|'.join(map(re.escape, names)) + r')\b')
for fn in sys.argv[3:]:
    s = open(fn).read()
    s2 = pat.sub(lambda m: (m.group(1) or '') + mod + '_' + m.group(2), s)
    if s2 != s: open(fn, 'w').write(s2)
