#! /usr/bin/env python3
#
# Creates stub shared libraries for linking of recompiled dynamically linked Linux executable.
#
# usage: mkstubs_elf.py <name>.imports <output directory> [compiler]
#
# The file <name>.imports is written by SRW64. For each needed library a stub library with the
# same name (SONAME) is created, it defines the imported symbols with the same versions, types
# and sizes (sizes of objects are needed for COPY relocations). At runtime the real libraries
# are used. Unversioned symbols are defined in the first library (symbols are searched in all
# libraries at runtime), weak symbols are not defined (they stay undefined like in the original).
#
# Prints arguments for the linker (dynamic linker, rpath, stub libraries).
#

import os
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        print("usage: mkstubs_elf.py <name>.imports <output directory> [compiler]", file=sys.stderr)
        return 1

    imports_file, out_dir = sys.argv[1], sys.argv[2]
    compiler = sys.argv[3] if len(sys.argv) > 3 else "gcc"

    interp = "/lib64/ld-linux-x86-64.so.2"
    rpath = None
    needed = []
    symbols = []

    with open(imports_file) as f:
        for line in f:
            parts = line.split()
            if not parts or parts[0].startswith("#"):
                continue
            if parts[0] == "interp":
                interp = parts[1]
            elif parts[0] == "rpath" and len(parts) > 1:
                rpath = parts[1]
            elif parts[0] == "needed":
                needed.append(parts[1])
            elif parts[0] == "symbol":
                library, name, version, sym_type, size, bind = parts[1:7]
                symbols.append((library, name, version, sym_type, int(size), bind))

    if not needed:
        print("mkstubs_elf.py: no needed libraries", file=sys.stderr)
        return 1

    os.makedirs(out_dir, exist_ok=True)

    interp_name = os.path.basename(interp)
    default_library = next((lib for lib in needed if lib != interp_name), needed[0])

    per_library = {lib: [] for lib in needed}
    for library, name, version, sym_type, size, bind in symbols:
        if bind == "WEAK" and sym_type != "OBJECT":
            continue
        if library == "-" or library not in per_library:
            library = default_library
            if version != "-" and library not in per_library:
                version = "-"
        per_library[library].append((name, version, sym_type, size))

    stubs = []
    for library in needed:
        base = os.path.join(out_dir, library.replace("/", "_"))
        asm_lines = []
        versions = {}
        defined = set()
        for name, version, sym_type, size in per_library[library]:
            if name in defined:
                continue
            defined.add(name)
            if version != "-":
                versions.setdefault(version, []).append(name)
            if sym_type == "OBJECT":
                asm_lines += ["\t.data", "\t.globl %s" % name, "\t.type %s, @object" % name,
                              "\t.size %s, %d" % (name, size), "%s:" % name, "\t.zero %d" % max(size, 1)]
            elif sym_type == "TLS":
                asm_lines += ["\t.section .tbss,\"awT\",@nobits", "\t.globl %s" % name, "\t.type %s, @tls_object" % name,
                              "\t.size %s, %d" % (name, size), "%s:" % name, "\t.zero %d" % max(size, 1)]
            else:
                asm_lines += ["\t.text", "\t.globl %s" % name, "\t.type %s, @function" % name, "%s:" % name, "\tret"]

        with open(base + ".s", "w") as f:
            f.write("\n".join(asm_lines) + "\n\t.section .note.GNU-stack,\"\",@progbits\n")

        command = [compiler, "-shared", "-nostdlib", "-o", os.path.join(out_dir, library), base + ".s", "-Wl,-soname," + library]
        if versions:
            with open(base + ".map", "w") as f:
                for version, names in versions.items():
                    f.write("%s {\n    global:\n%s};\n" % (version, "".join("        %s;\n" % n for n in names)))
            command.append("-Wl,--version-script=" + base + ".map")

        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stderr, file=sys.stderr)
            return 1
        stubs.append(os.path.join(out_dir, library))

    args = ["-dynamic-linker", interp]
    if rpath:
        args += ["-rpath", rpath]
    args += stubs
    print(" ".join(args))
    return 0


if __name__ == "__main__":
    sys.exit(main())
