#! /usr/bin/env python3
#
# Compares the recompiled code of an SRW64 build with the original code instruction by instruction.
#
# usage: compare_code.py <original executable> <recompiled executable (with loc_ symbols)> [address bias] [max reported]
#
#   address bias - difference between the addresses in loc_ names and the original addresses (position independent
#                  Linux executables: "# address bias" of --list_code_ranges / the note of SRW64), default 0
#
# Works with ELF64 and PE32+ executables (the recompiled program must not be stripped). Requires the Python module
# capstone (pip install capstone). Exit code: 0 - no differences, 1 - differences.
#
# The loc_<original address> symbols of the recompiled program are anchors: the original instructions between two
# consecutive anchors are compared with the recompiled instructions between the same anchors (alignment nops at the
# end of a recompiled span are ignored). Differences which are expected (branch targets, RIP-relative displacements,
# calls/jumps through GOT replaced by direct calls, short/near jump forms) are normalized; everything else is reported.

import collections
import re
import struct
import subprocess
import sys

import capstone


def load_regions(data):
    # executable parts of the file: (virtual address, file offset, size)
    if data[:2] == b'MZ':
        pe, = struct.unpack_from('<I', data, 0x3c)
        num_sections, = struct.unpack_from('<H', data, pe + 6)
        optional_size, = struct.unpack_from('<H', data, pe + 20)
        image_base, = struct.unpack_from('<Q', data, pe + 24 + 24)
        regions = []
        for index in range(num_sections):
            position = pe + 24 + optional_size + 40 * index
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from('<IIII', data, position + 8)
            characteristics, = struct.unpack_from('<I', data, position + 36)
            if characteristics & 0x20000000:
                regions.append((image_base + virtual_address, raw_offset, min(virtual_size, raw_size) if virtual_size else raw_size))
        return regions
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phnum, = struct.unpack_from('<H', data, 0x38)
    regions = []
    for index in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', data, phoff + 56 * index)
        if p_type == 1 and (p_flags & 1):
            regions.append((p_vaddr, p_offset, p_filesz))
    return regions


def reader(data, regions):
    def read(address, size):
        for vaddr, offset, filesz in regions:
            if vaddr <= address < vaddr + filesz:
                size = min(size, vaddr + filesz - address)
                return data[offset + address - vaddr:offset + address - vaddr + size]
        return b''
    return read


RIP = re.compile(r'\[rip [+-] 0x[0-9a-f]+\]')
HEX = re.compile(r'0x[0-9a-f]+')


def image_base(data):
    if data[:2] == b'MZ':
        pe, = struct.unpack_from('<I', data, 0x3c)
        return struct.unpack_from('<Q', data, pe + 24 + 24)[0]
    return 0


def normalize(ins):
    mnemonic = ins.mnemonic
    if mnemonic.startswith('notrack '):
        # CET no-track prefix (the recompiled program isn't IBT enforced)
        mnemonic = mnemonic[8:]
    ops = ins.op_str
    if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL):
        # branch targets and call/jump through memory (GOT) vs. direct call (PLT) differ
        return mnemonic, 'TARGET'
    if mnemonic in ('nop', 'int3'):
        return mnemonic, ''
    ops = RIP.sub('[rip+DISP]', ops)
    return mnemonic, ops


def main():
    original_name, recompiled_name = sys.argv[1], sys.argv[2]
    bias = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
    max_reported = int(sys.argv[4]) if len(sys.argv) > 4 else 50

    original = open(original_name, 'rb').read()
    recompiled = open(recompiled_name, 'rb').read()
    read_original = reader(original, load_regions(original))
    read_recompiled = reader(recompiled, load_regions(recompiled))
    original_base, recompiled_base = image_base(original), image_base(recompiled)

    anchors = []
    output = subprocess.run(['nm', recompiled_name], capture_output=True, text=True).stdout
    for line in output.split('\n'):
        parts = line.split()
        if len(parts) == 3 and parts[2].startswith('loc_') and parts[1] in 'tT':
            try:
                anchors.append((int(parts[2][4:], 16) - bias, int(parts[0], 16)))
            except ValueError:
                pass
    anchors.sort()
    anchor_map = dict(anchors)
    print('anchors: %d' % len(anchors), file=sys.stderr)

    def converted(old, new):
        # number converted by SRW64: address of label (immediate) or image relative address (RVA)
        if anchor_map.get(old) == new:
            return True
        return original_base != 0 and anchor_map.get(original_base + old) == recompiled_base + new

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = False

    differences = collections.Counter()
    not_recompiled = 0
    truncated = 0
    examples = []
    compared = 0
    for index in range(len(anchors) - 1):
        orig_start, new_start = anchors[index]
        orig_end, new_end = anchors[index + 1]
        if orig_end <= orig_start or orig_end - orig_start > 0x10000 or new_end <= new_start:
            continue
        orig_code = read_original(orig_start, orig_end - orig_start)
        new_code = read_recompiled(new_start, new_end - new_start)
        if not orig_code or not new_code:
            continue
        orig_ins = list(md.disasm(orig_code, orig_start))
        new_ins = list(md.disasm(new_code, new_start))
        # undecodable data / end of function: compare only while the original decodes completely
        if sum(i.size for i in orig_ins) != len(orig_code):
            continue
        # data in code section (not decoded by SRW64)
        if not new_ins or all(i.mnemonic == 'add' and i.op_str == 'byte ptr [rax], al' for i in orig_ins):
            continue
        # padding (nops/int3) at the end of a span: alignment in the recompiled code, padding in the original
        orig_cmp = list(orig_ins)
        new_cmp = list(new_ins)
        while orig_cmp and orig_cmp[-1].mnemonic in ('nop', 'int3'):
            orig_cmp.pop()
        while new_cmp and new_cmp[-1].mnemonic in ('nop', 'int3'):
            new_cmp.pop()
        if orig_cmp and not new_cmp:
            # not recompiled (function kept in original code, ignored area)
            not_recompiled += 1
            continue
        if 0 < len(new_cmp) < len(orig_cmp) and (new_cmp[-1].group(capstone.CS_GRP_CALL) or new_cmp[-1].group(capstone.CS_GRP_JUMP) or new_cmp[-1].mnemonic in ('syscall', 'ud2', 'hlt', 'ret')) and \
           all(normalize(x)[0] == normalize(y)[0] for x, y in zip(orig_cmp, new_cmp)):
            # decoding stopped after a call of a function which doesn't return (the rest is decoded elsewhere or unreachable)
            truncated += 1
            orig_cmp = orig_cmp[:len(new_cmp)]
        if len(orig_cmp) != len(new_cmp):
            key = 'instruction count %d vs %d' % (len(orig_cmp), len(new_cmp))
            differences['count mismatch'] += 1
            if len(examples) < max_reported:
                examples.append('0x%x (new 0x%x): %s: %s || %s' % (orig_start, new_start, key,
                                '; '.join('%s %s' % (i.mnemonic, i.op_str) for i in orig_cmp[:6]),
                                '; '.join('%s %s' % (i.mnemonic, i.op_str) for i in new_cmp[:6])))
            continue
        for a, b in zip(orig_cmp, new_cmp):
            compared += 1
            na, nb = normalize(a), normalize(b)
            if na == nb:
                continue
            # immediates which are addresses (converted) - only if mnemonic is the same and the difference is in hex numbers
            if na[0] == nb[0] and HEX.sub('N', na[1]) == HEX.sub('N', nb[1]) and ('[rip' in a.op_str or 'lea' == a.mnemonic):
                continue
            # mov r64, imm32 (sign extended) printed as 64-bit immediate: same value or converted address
            if a.mnemonic == 'mov' and b.mnemonic == 'movabs' and (a.operands[1].imm == b.operands[1].imm or converted(a.operands[1].imm & 0xffffffffffffffff, b.operands[1].imm)):
                continue
            # numbers converted by SRW64 (addresses of labels, image relative addresses)
            if na[0] == nb[0] and HEX.sub('N', na[1]) == HEX.sub('N', nb[1]):
                pairs = list(zip([int(x, 16) for x in HEX.findall(na[1])], [int(x, 16) for x in HEX.findall(nb[1])]))
                if all(x == y or converted(x, y) for x, y in pairs):
                    continue
            differences['%s -> %s' % (a.mnemonic, b.mnemonic) if na[0] != nb[0] else 'operands of %s' % a.mnemonic] += 1
            if len(examples) < max_reported:
                examples.append('0x%x (new 0x%x): %s %s || %s %s  [%s | %s]' % (a.address, b.address, a.mnemonic, a.op_str, b.mnemonic, b.op_str, a.bytes.hex(), b.bytes.hex()))

    print('compared instructions: %d (spans not recompiled - kept in original code / ignored: %d, spans ending at ret or a call which doesn\'t return: %d)' % (compared, not_recompiled, truncated))
    if not differences:
        print('no differences')
    for key, count in differences.most_common(60):
        print('%8d  %s' % (count, key))
    print()
    for example in examples:
        print(example)
    return 1 if differences else 0


if __name__ == '__main__':
    sys.exit(main())
