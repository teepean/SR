#!/usr/bin/env python3
"""Code roots for SRW (x64/fixup_interpret_as_code.sci) of a relocatable PE (a game DLL).

  mkroots.py <module dir> <PE file> <IDA funcs.csv> [<enumfuncs .csv> [<IDA heads.csv>]]

SRW translates only what it reaches from its roots, and a function referenced only from data (vtables,
interface tables filled at run time) would otherwise be emitted as data bytes and then run as x64 code.
Roots = IDA's function starts + avp2src tools/enumfuncs.py's starts + every code address stored in data
(found through the base relocations) + the entry point, minus external_procedures.sci and ignored_areas.sci.
With IDA's heads.csv also the code addresses that code takes (relocations inside .text whose target is an IDA
instruction head - e.g. `mov [x], offset thunk` for member function pointers; MSVC's jump tables inside .text
point at case labels, also instruction heads; the tables themselves are IDA data heads and are left out)."""
import csv, os, struct, sys
import pefile

d, pefn, funcs = sys.argv[1:4]
enum = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] != '-' else None
heads = sys.argv[5] if len(sys.argv) > 5 else None
pe = pefile.PE(pefn)
base = pe.OPTIONAL_HEADER.ImageBase
text = None
for s in pe.sections:
    if s.Characteristics & 0x20000000:      # IMAGE_SCN_MEM_EXECUTE
        lo = base + s.VirtualAddress
        hi = lo + s.Misc_VirtualSize
        text = (lo, hi) if text is None else (min(text[0], lo), max(text[1], hi))

def sci(name):
    p = os.path.join(d, 'x64', name)
    if not os.path.exists(p): return []
    return [l.strip().split(',') for l in open(p) if l.strip() and not l.startswith('#')]

ext = {int(r[0][4:], 16) for r in sci('external_procedures.sci')}
ign = [(int(r[0][4:], 16), int(r[1])) for r in sci('ignored_areas.sci')]

roots = set()
for r in csv.reader(open(funcs)):
    try: roots.add(int(r[0], 16))
    except (ValueError, IndexError): pass
nida = len(roots)
if enum:
    for r in csv.reader(open(enum)):
        try: roots.add(int(r[0], 16))
        except (ValueError, IndexError): pass
nenum = len(roots) - nida

code_heads = set()
if heads:
    for r in csv.reader(open(heads)):
        if len(r) >= 3 and r[2] == 'c': code_heads.add(int(r[0], 16))

img = pe.get_memory_mapped_image()
ndata = ncode = 0
for e in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
    for r in e.entries:
        if r.type != 3: continue            # IMAGE_REL_BASED_HIGHLOW
        a = base + r.rva
        v = struct.unpack_from('<I', img, r.rva)[0]
        if not (text[0] <= v < text[1]) or v in roots: continue
        if text[0] <= a < text[1]:
            if v in code_heads:
                roots.add(v); ncode += 1
        else:
            roots.add(v); ndata += 1
roots.add(base + pe.OPTIONAL_HEADER.AddressOfEntryPoint)

roots = {a for a in roots if text[0] <= a < text[1] and a not in ext and not any(x <= a < x + k for x, k in ign)}
open(os.path.join(d, 'x64', 'fixup_interpret_as_code.sci'), 'w').write(''.join('loc_%X\n' % a for a in sorted(roots)))
print('%d roots (IDA %d, enumfuncs +%d, code pointers in data +%d, code addresses taken by code +%d)' % (len(roots), nida, nenum, ndata, ncode))
