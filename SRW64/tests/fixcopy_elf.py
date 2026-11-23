#! /usr/bin/env python3
#
# Moves COPY relocations of recompiled dynamically linked Linux executable to the original addresses.
#
# usage: fixcopy_elf.py <recompiled executable> <name>.imports
#
# Objects copied from shared libraries into the executable (COPY relocations - e.g. stdout, environ,
# C++ type information) are used at their original addresses by the recompiled code and by the
# original code which is kept. The linker creates the COPY relocations at new addresses, this script
# moves the relocations and the dynamic symbols to the original addresses (lines "copy" in .imports),
# so the dynamic linker initializes the original locations and the libraries use them.
#

import struct
import sys

R_X86_64_COPY = 5
SHT_RELA = 4
SHT_DYNSYM = 11


def main():
    if len(sys.argv) < 3:
        print("usage: fixcopy_elf.py <recompiled executable> <name>.imports", file=sys.stderr)
        return 1

    exe_file, imports_file = sys.argv[1], sys.argv[2]

    copies = {}
    with open(imports_file) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 4 and parts[0] == "copy":
                copies[parts[1]] = (int(parts[2], 0), int(parts[3]))

    if not copies:
        return 0

    data = bytearray(open(exe_file, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2:
        print("fixcopy_elf.py: not an ELF64 file", file=sys.stderr)
        return 1

    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3a)
    sections = []
    for index in range(shnum):
        sections.append(struct.unpack_from("<IIQQQQIIQQ", data, shoff + index * shentsize))

    def section_of(address):
        for index, (name, sh_type, flags, addr, offset, size, link, info, align, entsize) in enumerate(sections):
            if flags & 2 and addr <= address < addr + size:
                return index
        return None

    dynsym = next((s for s in sections if s[1] == SHT_DYNSYM), None)
    if dynsym is None:
        print("fixcopy_elf.py: dynamic symbol table not found", file=sys.stderr)
        return 1
    dynstr = sections[dynsym[6]]

    def symbol_name(index):
        st_name, = struct.unpack_from("<I", data, dynsym[4] + index * 24)
        end = data.index(b"\0", dynstr[4] + st_name)
        return data[dynstr[4] + st_name:end].decode()

    moved = 0
    for name_offset, sh_type, flags, addr, offset, size, link, info, align, entsize in sections:
        if sh_type != SHT_RELA or link != sections.index(dynsym):
            continue
        for position in range(offset, offset + size, 24):
            r_offset, r_info, r_addend = struct.unpack_from("<QQq", data, position)
            if r_info & 0xffffffff != R_X86_64_COPY:
                continue
            sym = r_info >> 32
            name = symbol_name(sym)
            if name not in copies:
                print("fixcopy_elf.py: unknown COPY relocation: %s" % name, file=sys.stderr)
                continue

            address, copy_size = copies[name]
            target_section = section_of(address)
            if target_section is None:
                print("fixcopy_elf.py: original address of %s is not in any section: 0x%x" % (name, address), file=sys.stderr)
                return 1

            sym_position = dynsym[4] + sym * 24
            st_size, = struct.unpack_from("<Q", data, sym_position + 16)
            if st_size != copy_size:
                print("fixcopy_elf.py: warning: different size of %s: %d (original %d)" % (name, st_size, copy_size), file=sys.stderr)

            struct.pack_into("<Q", data, position, address)
            struct.pack_into("<Q", data, sym_position + 8, address)
            struct.pack_into("<H", data, sym_position + 6, target_section)
            moved += 1

    open(exe_file, "wb").write(data)
    print("COPY relocations moved to original addresses: %d" % moved)
    return 0


if __name__ == "__main__":
    sys.exit(main())
