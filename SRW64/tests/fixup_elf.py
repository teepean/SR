#! /usr/bin/env python3
#
# Post-link fixups of recompiled Linux executable.
#
# usage: fixup_elf.py <recompiled executable> <file> [<file> ...]
#
# Files written by SRW64:
#
#   <name>.imports (lines "copy <name> <original address> <size>"):
#     Objects copied from shared libraries into the executable (COPY relocations - e.g. stdout,
#     environ, C++ type information) are used at their original addresses by the recompiled code and
#     by the original code which is kept. The linker creates the COPY relocations at new addresses,
#     the relocations and the dynamic symbols are moved to the original addresses, so the dynamic
#     linker initializes the original locations and the libraries use them.
#
#   <name>.phdrs (lines "phdr <placeholder note type> <type> <flags> <address> <size in file> <size in memory> <alignment>"):
#     Program headers of the original executable which the linker doesn't create (e.g. OS specific
#     headers, notes in data). The generated file contains placeholder notes (the linker creates
#     program headers PT_NOTE for them), these program headers are changed to the original ones.
#

import struct
import sys

R_X86_64_COPY = 5
SHT_RELA = 4
SHT_DYNSYM = 11
PT_LOAD = 1
PT_NOTE = 4


def fix_copies(data, sections, copies):
    def section_of(address):
        for index, (name, sh_type, flags, addr, offset, size, link, info, align, entsize) in enumerate(sections):
            if flags & 2 and addr <= address < addr + size:
                return index
        return None

    dynsym = next((s for s in sections if s[1] == SHT_DYNSYM), None)
    if dynsym is None:
        print("fixup_elf.py: dynamic symbol table not found", file=sys.stderr)
        return -1
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
                print("fixup_elf.py: unknown COPY relocation: %s" % name, file=sys.stderr)
                continue

            address, copy_size = copies[name]
            target_section = section_of(address)
            if target_section is None:
                print("fixup_elf.py: original address of %s is not in any section: 0x%x" % (name, address), file=sys.stderr)
                return -1

            sym_position = dynsym[4] + sym * 24
            st_size, = struct.unpack_from("<Q", data, sym_position + 16)
            if st_size != copy_size:
                print("fixup_elf.py: warning: different size of %s: %d (original %d)" % (name, st_size, copy_size), file=sys.stderr)

            struct.pack_into("<Q", data, position, address)
            struct.pack_into("<Q", data, sym_position + 8, address)
            struct.pack_into("<H", data, sym_position + 6, target_section)
            moved += 1

    print("COPY relocations moved to original addresses: %d" % moved)
    return 0


def fix_phdrs(data, phdrs):
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)

    headers = [list(struct.unpack_from("<IIQQQQQQ", data, phoff + index * phentsize)) for index in range(phnum)]
    loads = [h for h in headers if h[0] == PT_LOAD]

    changed = 0
    for index, header in enumerate(headers):
        if header[0] != PT_NOTE or header[5] < 16:
            continue
        namesz, descsz, note_type = struct.unpack_from("<III", data, header[2])
        if namesz != 4 or data[header[2] + 12:header[2] + 16] != b"SRW\0" or note_type not in phdrs:
            continue

        p_type, p_flags, address, filesz, memsz, align = phdrs[note_type]
        load = next((l for l in loads if l[3] <= address and address + filesz <= l[3] + l[5]), None)
        if load is None:
            print("fixup_elf.py: address of program header 0x%x is not in loaded data: 0x%x" % (p_type, address), file=sys.stderr)
            return -1

        offset = load[2] + (address - load[3])
        struct.pack_into("<IIQQQQQQ", data, phoff + index * phentsize, p_type, p_flags, offset, address, address, filesz, memsz, align)
        changed += 1

    if changed != len(phdrs):
        print("fixup_elf.py: placeholders found for %d of %d program headers" % (changed, len(phdrs)), file=sys.stderr)
        return -1

    print("program headers created: %d" % changed)
    return 0


def main():
    if len(sys.argv) < 3:
        print("usage: fixup_elf.py <recompiled executable> <file> [<file> ...]", file=sys.stderr)
        return 1

    exe_file = sys.argv[1]
    copies = {}
    phdrs = {}
    for file_name in sys.argv[2:]:
        with open(file_name) as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 4 and parts[0] == "copy":
                    copies[parts[1]] = (int(parts[2], 0), int(parts[3]))
                elif len(parts) >= 8 and parts[0] == "phdr":
                    phdrs[int(parts[1], 0)] = tuple(int(value, 0) for value in parts[2:8])

    data = bytearray(open(exe_file, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2:
        print("fixup_elf.py: not an ELF64 file", file=sys.stderr)
        return 1

    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3a)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + index * shentsize) for index in range(shnum)]

    if copies and fix_copies(data, sections, copies):
        return 1
    if phdrs and fix_phdrs(data, phdrs):
        return 1

    open(exe_file, "wb").write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
