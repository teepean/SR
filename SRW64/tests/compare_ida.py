#! /usr/bin/env python3
#
# Compares jump tables and decoded code of SRW64 with the analysis of IDA Pro.
#
# usage: compare_ida.py <executable> <srw64 jump tables> <srw64 code ranges> <ida export directory> [number of details]
#
#   <srw64 jump tables>, <srw64 code ranges> - files written by SRW64 options --list_jump_tables and --list_code_ranges
#   <ida export directory> - files written by ida_export.py (headless IDA)
#
# Reports:
#   * switches found by IDA: same targets in SRW64 / different table / targets missing in SRW64 / more targets in SRW64
#     (IDA's number of cases can be too small for switches without range check) / jump without jump table in SRW64 /
#     jump or targets not decoded by SRW64,
#   * jump tables converted by SRW64 which IDA doesn't know,
#   * code in IDA functions which SRW64 didn't decode (code in sections which SRW64 considers data, padding, unreachable ud2 after calls of functions which
#     don't return, thunks - PLT stubs and import thunks which SRW64 replaces - and areas which SRW64 doesn't decode
#     on purpose - functions kept in original code, ignored_areas.sci - are listed separately; code directly after
#     a call is listed separately too - SRW64 considers the called function as not returning, IDA doesn't).
# Tables of absolute addresses which are found as pointers in data (e.g. computed goto tables in statically linked
# executables: mov rdx, [rdi+rax*8]; jmp rdx) are not jump tables for SRW64, such jumps are listed separately if
# all targets are decoded.
# IDA is not always right (check the differences manually), the exit code is 1 if there are differences.
#

import bisect
import collections
import os
import struct
import sys


def read_header_bias(name):
    with open(name) as f:
        for line in f:
            if not line.startswith("#"):
                break
            if line.startswith("# address bias:"):
                return int(line.split()[3], 16)
    return 0


class Executable:
    def __init__(self, name):
        with open(name, "rb") as f:
            self.data = f.read()
        self.regions = []       # (virtual address, file offset, size)
        self.base = 0           # lowest address (ELF) / image base (PE)
        self.pie = False
        if self.data[:4] == b"\x7fELF":
            e_type, = struct.unpack_from("<H", self.data, 0x10)
            phoff, = struct.unpack_from("<Q", self.data, 0x20)
            phentsize, phnum = struct.unpack_from("<HH", self.data, 0x36)
            addresses = []
            for index in range(phnum):
                p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from("<IIQQQQQQ", self.data, phoff + index * phentsize)
                if p_type == 1:
                    self.regions.append((p_vaddr, p_offset, p_filesz))
                    addresses.append(p_vaddr)
            self.base = (min(addresses) if addresses else 0) & ~0xfff
            self.pie = (e_type == 3)
        elif self.data[:2] == b"MZ":
            pe, = struct.unpack_from("<I", self.data, 0x3c)
            num_sections, = struct.unpack_from("<H", self.data, pe + 6)
            optional_size, = struct.unpack_from("<H", self.data, pe + 20)
            self.base, = struct.unpack_from("<Q", self.data, pe + 24 + 24)
            sections = pe + 24 + optional_size
            for index in range(num_sections):
                virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", self.data, sections + 40 * index + 8)
                self.regions.append((self.base + virtual_address, raw_offset, min(virtual_size, raw_size) if virtual_size else raw_size))
        else:
            raise ValueError("unknown executable format: %s" % name)

    def read(self, address, size):
        for vaddr, offset, region_size in self.regions:
            if vaddr <= address and address + size <= vaddr + region_size:
                return self.data[offset + address - vaddr:offset + address - vaddr + size]
        return None


def plt_stub(code):
    # PLT entries for lazy binding (push index; jmp PLT0) and PLT0 (push [rip+X]; jmp [rip+Y]), without padding
    if code is None:
        return False
    if len(code) >= 10 and code[0] == 0x68 and code[5] == 0xe9:
        rest = code[10:]
    elif len(code) >= 12 and code[0:2] == b"\xff\x35" and code[6:8] == b"\xff\x25":
        rest = code[12:]
    else:
        return False
    return len(rest) == 0 or padding_kind(rest) == "padding"


def padding_kind(code):
    # returns "padding" (int3, nop), "ud2" (only ud2 or hlt), or None
    if code is None or len(code) == 0:
        return None
    if all(b == 0xcc for b in code):
        return "padding"
    if code in (b"\x0f\x0b", b"\xf4"):
        return "ud2"
    position = 0
    while position < len(code):
        start = position
        while position < len(code) and code[position] in (0x66, 0x2e):
            position += 1
        if position < len(code) and code[position] == 0x90:
            position += 1
            continue
        if position + 2 < len(code) and code[position] == 0x0f and code[position + 1] == 0x1f:
            modrm = code[position + 2]
            mod, rm = modrm >> 6, modrm & 7
            length = 3 + (1 if rm == 4 and mod != 3 else 0)
            length += {0: 4 if rm == 5 else 0, 1: 1, 2: 4, 3: 0}[mod]
            tail = code[position + 3 + (1 if rm == 4 and mod != 3 else 0):position + length]
            if position + length <= len(code) and all(b == 0 for b in tail):
                position += length
                continue
        position = start
        break
    return "padding" if position == len(code) else None


class Ranges:
    def __init__(self, ranges):
        merged = []
        for start, end in sorted(ranges):
            if merged and start <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], end)
            else:
                merged.append([start, end])
        self.ranges = merged
        self.starts = [r[0] for r in merged]

    def contains(self, address):
        index = bisect.bisect_right(self.starts, address) - 1
        return index >= 0 and self.ranges[index][0] <= address < self.ranges[index][1]

    def uncovered(self, start, end):
        # parts of [start, end) which are not covered
        result = []
        address = start
        index = max(bisect.bisect_right(self.starts, start) - 1, 0)
        while address < end:
            while index < len(self.ranges) and self.ranges[index][1] <= address:
                index += 1
            if index >= len(self.ranges) or self.ranges[index][0] >= end:
                result.append((address, end))
                break
            if self.ranges[index][0] > address:
                result.append((address, self.ranges[index][0]))
            address = self.ranges[index][1]
        return result


def main():
    if len(sys.argv) < 5:
        print("usage: compare_ida.py <executable> <srw64 jump tables> <srw64 code ranges> <ida export directory> [number of details]", file=sys.stderr)
        return 2
    executable = Executable(sys.argv[1])
    jump_tables_name, code_ranges_name, ida_dir = sys.argv[2:5]
    details = int(sys.argv[5]) if len(sys.argv) > 5 else 10

    # address mapping: SRW64 address = original address + bias, IDA rebases position independent ELF executables
    bias = read_header_bias(jump_tables_name)
    ida_imagebase = 0
    with open(os.path.join(ida_dir, "ida_info.txt")) as f:
        for line in f:
            parts = line.split()
            if parts and parts[0] == "imagebase":
                ida_imagebase = int(parts[1], 16)
    delta = bias + ((executable.base - ida_imagebase) if executable.pie else 0)

    def from_ida(address):
        return address + delta

    # SRW64
    srw_tables = {}
    with open(jump_tables_name) as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.split()
            srw_tables[int(parts[0], 16)] = (int(parts[1], 16), parts[2], int(parts[3]), int(parts[4]), set(int(x, 16) for x in parts[5:]))
    decoded, ignored, code_sections = [], [], []
    with open(code_ranges_name) as f:
        for line in f:
            if line.startswith("# code section:"):
                parts = line.split()
                code_sections.append((int(parts[3], 16), int(parts[4], 16)))
            if line.startswith("#"):
                continue
            parts = line.split()
            (ignored if len(parts) > 2 and parts[2] == "ignored" else decoded).append((int(parts[0], 16), int(parts[1], 16)))
    srw_code = Ranges(decoded)
    srw_ignored = Ranges(ignored)
    srw_sections = Ranges(code_sections) if code_sections else None

    # IDA
    functions = []
    thunks = []
    with open(os.path.join(ida_dir, "functions.txt")) as f:
        for line in f:
            parts = line.split()
            functions.append((from_ida(int(parts[0], 16)), parts[1]))
            if parts[2] == "thunk":
                for chunk in parts[3:]:
                    start, end = chunk.split("-")
                    thunks.append((from_ida(int(start, 16)), from_ida(int(end, 16))))
    functions.sort()
    function_starts = [function[0] for function in functions]
    thunk_code = Ranges(thunks)

    def function_of(address):
        index = bisect.bisect_right(function_starts, address) - 1
        return "%s+0x%x" % (functions[index][1], address - functions[index][0]) if index >= 0 else "?"

    switches = {}
    with open(os.path.join(ida_dir, "switches.txt")) as f:
        for line in f:
            parts = line.split()
            count = int(parts[5])
            switches[from_ida(int(parts[0], 16))] = (from_ida(int(parts[1], 16)), from_ida(int(parts[2], 16)), int(parts[4]), set(from_ida(int(x, 16)) for x in parts[6:6 + count]))
    with open(os.path.join(ida_dir, "code_ranges.txt")) as f:
        # (chunks shared by more functions are listed more times)
        ida_code = Ranges([tuple(from_ida(int(x, 16)) for x in line.split()) for line in f]).ranges

    stats = collections.OrderedDict()
    examples = collections.defaultdict(list)
    problems = 0

    def add(category, text, problem):
        nonlocal problems
        if category not in stats:
            stats[category] = [0, problem]
        stats[category][0] += 1
        if problem:
            problems += 1
        examples[category].append(text)

    # switches
    for jump, (table, elbase, cases, targets) in sorted(switches.items()):
        where = "jump 0x%x (%s)" % (jump, function_of(jump))
        if srw_ignored.contains(jump):
            add("switch: in area which SRW64 doesn't decode (kept in original code, ignored_areas.sci)", where, False)
            continue
        if jump in srw_tables:
            srw_table, kind, entries, bound, srw_targets = srw_tables[jump]
            missing = targets - srw_targets
            extra = srw_targets - targets
            if entries == 0:
                add("switch: SRW64 didn't convert the table", "%s IDA table 0x%x" % (where, table), True)
            elif srw_table not in (table, elbase):
                add("switch: different table", "%s IDA table 0x%x (base 0x%x), SRW64 table 0x%x" % (where, table, elbase, srw_table), True)
            elif missing:
                add("switch: targets missing in SRW64", "%s table 0x%x: IDA %d targets (%d cases), SRW64 %d (%d entries, bound %d), missing %s" % (
                    where, srw_table, len(targets), cases, len(srw_targets), entries, bound, " ".join("0x%x" % x for x in sorted(missing)[:8])), True)
            elif extra:
                add("switch: more targets in SRW64 (IDA's number of cases can be too small)", "%s table 0x%x: IDA %d cases, SRW64 %d entries (bound %d)" % (
                    where, srw_table, cases, entries, bound), False)
            else:
                add("switch: same targets", where, False)
        elif srw_code.contains(jump):
            if elbase == from_ida(0) and all(srw_code.contains(target) for target in targets):
                add("switch: table of absolute addresses, not a jump table for SRW64 (pointers in data?), targets decoded", "%s IDA table 0x%x, %d targets" % (where, table, len(targets)), False)
            else:
                add("switch: jump decoded by SRW64 without jump table", "%s IDA table 0x%x, %d targets" % (where, table, len(targets)), True)
        else:
            add("switch: jump not decoded by SRW64", "%s IDA table 0x%x" % (where, table), True)
        for target in sorted(targets):
            if not srw_code.contains(target) and not srw_ignored.contains(target):
                add("switch: target not decoded by SRW64", "%s target 0x%x" % (where, target), True)

    # jump tables which IDA doesn't know
    for jump, (srw_table, kind, entries, bound, srw_targets) in sorted(srw_tables.items()):
        if jump not in switches:
            add("SRW64 jump table unknown to IDA", "jump 0x%x (%s) table 0x%x (%s, %d entries, bound %d)" % (jump, function_of(jump), srw_table, kind, entries, bound), entries == 0)

    # code in IDA functions which SRW64 didn't decode
    for start, end in ida_code:
        for part_start, part_end in srw_code.uncovered(start, end):
            kind = padding_kind(executable.read(part_start - bias, part_end - part_start))
            text = "0x%x-0x%x (%d bytes, %s)" % (part_start, part_end, part_end - part_start, function_of(part_start))
            if srw_sections is not None and not srw_sections.contains(part_start):
                add("code: in section which is data for SRW64 (original bytes are used if the code is executed)", text, False)
            elif srw_ignored.contains(part_start):
                add("code: in area which SRW64 doesn't decode (kept in original code, ignored_areas.sci)", text, False)
            elif thunk_code.contains(part_start):
                add("code: thunk not decoded by SRW64 (PLT stub, import thunk)", text, False)
            elif plt_stub(executable.read(part_start - bias, part_end - part_start)):
                add("code: PLT entry for lazy binding not decoded by SRW64", text, False)
            elif kind == "padding":
                add("code: padding not decoded by SRW64", text, False)
            elif kind == "ud2":
                add("code: ud2/hlt not decoded by SRW64 (after call of function which doesn't return)", text, False)
            elif srw_code.contains(part_start - 5) and executable.read(part_start - 5 - bias, 1) == b"\xe8":
                add("code: after call of function which doesn't return for SRW64 (dead code?)", text, False)
            elif (srw_code.contains(part_start - 1) and executable.read(part_start - 1 - bias, 1) == b"\xf4") or \
                 (srw_code.contains(part_start - 2) and executable.read(part_start - 2 - bias, 2) == b"\x0f\x0b"):
                add("code: after hlt/ud2 (dead code?)", text, False)
            else:
                add("code: decoded by IDA, not by SRW64", text, True)

    print("address bias: 0x%x, IDA image base: 0x%x, IDA -> SRW64 address: +0x%x" % (bias, ida_imagebase, delta))
    print("IDA: %d functions, %d switches; SRW64: %d jumps through jump tables" % (len(functions), len(switches), len(srw_tables)))
    print()
    for category, (count, problem) in stats.items():
        print("%8d  %s%s" % (count, category, "  <-- check" if problem else ""))
    for category, (count, problem) in stats.items():
        if category == "switch: same targets" or (not problem and details <= 0):
            continue
        if category.startswith("code: padding") or category.startswith("code: ud2") or category.startswith("code: thunk") or category.startswith("code: PLT") or "doesn't decode" in category or category.startswith("switch: table of absolute"):
            continue
        print("\n%s:" % category)
        for text in examples[category][:details]:
            print("    " + text)
        if count > details:
            print("    ... (%d more)" % (count - details))

    print("\n%s" % ("differences found: %d (check them, IDA is not always right)" % problems if problems else "no differences"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
