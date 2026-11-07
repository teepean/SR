#!/usr/bin/env python3
"""Draft imports.spec entries for the recompiled AvP2 modules.

  mkspec.py <reference imports.spec> <out.spec> <module>:<ida imports.csv>:<module .asm> ...

For every `extern Name ; DLL` line SRW wrote into a module's .asm, the entry comes from the reference spec
(Interstate '76's, same runtime conventions) when it has the name, otherwise from IDA's prototype
(ida_imports.py: module|name|ordinal|iat|type): stdN / cN / vN (varargs), fret for float/double results;
decorated names (_Name@N) give stdcall with N/4 arguments. Entries without a prototype are written as `??`
and must be completed by hand. Existing lines of <out.spec> are kept (hand edits win)."""
import re, sys

def split_top(s):
    depth = 0; parts = ['']
    for ch in s:
        if ch in '(<[': depth += 1
        if ch in ')>]': depth -= 1
        if ch == ',' and depth == 0: parts.append('')
        else: parts[-1] += ch
    return [p.strip() for p in parts]

def parse(t):
    if not t or not t.endswith(')'): return None
    depth = 0
    for i in range(len(t) - 1, -1, -1):
        if t[i] == ')': depth += 1
        elif t[i] == '(':
            depth -= 1
            if depth == 0: break
    args = t[i + 1:-1].strip(); head = t[:i]
    conv = 'std' if '__stdcall' in head else 'c'
    ret = head.split('(')[0].split()
    fret = bool(ret) and ret[0] in ('double', 'float')
    if args in ('', 'void'): return conv, 0, False, fret
    parts = split_top(args)
    va = parts[-1] == '...'
    if va: parts = parts[:-1]
    n = 0
    for p in parts:
        w = p.replace('const', '').split()
        # 64-bit values take two stack slots; pointers to them one
        n += 2 if (w and '*' not in p and w[0] in ('double', '__int64', 'unsigned __int64', 'LONGLONG', 'ULONGLONG', 'LARGE_INTEGER')) else 1
    return conv, n, va, fret

ref_spec, out_spec = sys.argv[1:3]
ref = {}
for l in open(ref_spec):
    s = l.split('#', 1)[0].split()
    if s: ref[s[0]] = l.rstrip('\n')
keep = {}
try:
    for l in open(out_spec):
        s = l.split('#', 1)[0].split()
        if s: keep[s[0]] = l.rstrip('\n')
except FileNotFoundError:
    pass

protos, used = {}, {}
for arg in sys.argv[3:]:
    mod, csvf, asm = arg.split(':')
    for l in open(csvf):
        p = l.rstrip('\n').split('|', 4)
        if len(p) == 5 and (p[4] or p[1] not in protos): protos[p[1]] = p[4]
    for l in open(asm):
        m = re.match(r'extern (\S+) ; (\S+)', l)
        if m: used.setdefault(m.group(1), [set(), m.group(2)])[0].add(mod)

out = []
for name, (mods, dll) in sorted(used.items(), key=lambda x: x[0].lower()):
    if name in keep:
        out.append(keep[name]); continue
    if name in ref:
        line = re.sub(r'#\s*[^|]*\|', '# %s |' % ','.join(sorted(mods)), ref[name], count=1)
        out.append(line); continue
    t = protos.get(name, '')
    p = parse(t)
    m = re.match(r'_(\w+)@(\d+)$', name)
    if m: p = ('std', int(m.group(2)) // 4, False, False)
    spec = '??' if not p else ('v%d' % p[1] if p[2] else '%s%d' % (p[0], p[1]))
    flags = 'fret' if p and p[3] else '-'
    out.append('%-28s %-6s %-5s %-14s # %s | %s' % (name, spec, flags, dll, ','.join(sorted(mods)), t))
for name, line in keep.items():          # hand-written entries SRW doesn't emit as externs (static, raw, module)
    if name not in used: out.append(line)
open(out_spec, 'w').write('\n'.join(out) + '\n')
print('%d entries, %d without a convention' % (len(out), sum(1 for l in out if l.split()[1] == '??')))
