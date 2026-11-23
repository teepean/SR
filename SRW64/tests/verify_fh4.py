#! /usr/bin/python3
#
# Verifies C++ exception handling data version 4 (__CxxFrameHandler4) in executable recompiled by SRW64.
#
# The data of every function of the original executable is decoded and compared with the data
# of the same function in the recompiled executable (addresses are mapped using the labels
# loc_<original address> from the symbol table of the recompiled executable).
#
# usage: python3 verify_fh4.py <original.exe> <recompiled.exe>
#

import bisect
import struct
import sys


class PEFile:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        pe = self.u32(0x3c)
        num_sections = self.u16(pe + 6)
        optional_size = self.u16(pe + 20)
        optional = pe + 24
        self.image_base = self.u64(optional + 24)
        directories = optional + 112
        self.exception_rva = self.u32(directories + 3 * 8)
        self.exception_size = self.u32(directories + 3 * 8 + 4)
        self.import_rva = self.u32(directories + 1 * 8)
        self.sections = []
        for index in range(num_sections):
            header = optional + optional_size + 40 * index
            name = self.data[header:header + 8].rstrip(b'\0').decode('latin-1')
            vsize, va, raw_size, raw_ptr = struct.unpack_from('<IIII', self.data, header + 8)
            self.sections.append((name, va, max(vsize, raw_size), raw_ptr, raw_size))
        self.symbols = {}
        symbol_ptr = self.u32(pe + 12)
        num_symbols = self.u32(pe + 16)
        if symbol_ptr != 0:
            strings = symbol_ptr + 18 * num_symbols
            index = 0
            while index < num_symbols:
                entry = symbol_ptr + 18 * index
                if self.data[entry:entry + 4] == b'\0\0\0\0':
                    offset = self.u32(entry + 4)
                    end = self.data.index(b'\0', strings + offset)
                    name = self.data[strings + offset:end].decode('latin-1')
                else:
                    name = self.data[entry:entry + 8].rstrip(b'\0').decode('latin-1')
                value = self.u32(entry + 8)
                section_number = struct.unpack_from('<h', self.data, entry + 12)[0]
                aux = self.data[entry + 17]
                if name.startswith('loc_') and 0 < section_number <= len(self.sections):
                    self.symbols[name] = self.sections[section_number - 1][1] + value
                index += 1 + aux
        self.imports = self.read_imports()

    def u16(self, offset):
        return struct.unpack_from('<H', self.data, offset)[0]

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, offset)[0]

    def u64(self, offset):
        return struct.unpack_from('<Q', self.data, offset)[0]

    def file_offset(self, rva):
        for name, va, size, raw_ptr, raw_size in self.sections:
            if va <= rva < va + size:
                if rva - va >= raw_size:
                    return None
                return raw_ptr + rva - va
        return None

    def byte(self, rva):
        return self.data[self.file_offset(rva)]

    def dword(self, rva):
        return self.u32(self.file_offset(rva))

    def int32(self, rva):
        return struct.unpack_from('<i', self.data, self.file_offset(rva))[0]

    def read_imports(self):
        imports = {}
        if self.import_rva == 0:
            return imports
        descriptor = self.import_rva
        while True:
            lookup, _, _, name, iat = struct.unpack_from('<IIIII', self.data, self.file_offset(descriptor))
            if name == 0 and iat == 0:
                break
            if lookup == 0:
                lookup = iat
            index = 0
            while True:
                value = self.u64(self.file_offset(lookup + 8 * index))
                if value == 0:
                    break
                if not (value >> 63):
                    offset = self.file_offset((value & 0x7fffffff) + 2)
                    end = self.data.index(b'\0', offset)
                    imports[iat + 8 * index] = self.data[offset:end].decode('latin-1')
                index += 1
            descriptor += 20
        return imports

    def handler_name(self, rva):
        offset = self.file_offset(rva)
        if offset is not None and self.data[offset:offset + 2] == b'\xff\x25':
            target = rva + 6 + struct.unpack_from('<i', self.data, offset + 2)[0]
            return self.imports.get(target, '')
        return ''

    def runtime_functions(self):
        result = []
        for index in range(self.exception_size // 12):
            begin, end, unwind = struct.unpack_from('<III', self.data, self.file_offset(self.exception_rva + 12 * index))
            if begin != 0:
                result.append((begin, end, unwind))
        return result

    def read_unsigned(self, rva):
        first = self.byte(rva)
        if first & 1 == 0:
            return first >> 1, 1
        if first & 2 == 0:
            return (self.byte(rva) | (self.byte(rva + 1) << 8)) >> 2, 2
        if first & 4 == 0:
            return (self.byte(rva) | (self.byte(rva + 1) << 8) | (self.byte(rva + 2) << 16)) >> 3, 3
        if first & 8 == 0:
            return self.dword(rva) >> 4, 4
        return self.dword(rva + 1), 5

    def handler_data(self, unwind):
        flags = self.byte(unwind) >> 3
        if not (flags & 3) or (flags & 4):
            return None
        count = self.byte(unwind + 2)
        after = unwind + 4 + 2 * ((count + 1) & ~1)
        return self.dword(after), after + 4

    def decode_funcinfo4(self, rva, function_start):
        header = self.byte(rva)
        pos = rva + 1
        result = {'header': header, 'unwind': [], 'try': [], 'ip': None}
        if header & 0x04:
            _, length = self.read_unsigned(pos)
            pos += length
        if header & 0x08:
            unwind_rva = self.int32(pos)
            pos += 4
            count, length = self.read_unsigned(unwind_rva)
            p = unwind_rva + length
            for _ in range(count):
                value, length = self.read_unsigned(p)
                p += length
                entry = [value & 3, None, None]
                if value & 3 in (1, 2):
                    entry[1] = self.int32(p)
                    p += 4
                    entry[2], length = self.read_unsigned(p)
                    p += length
                elif value & 3 == 3:
                    entry[1] = self.int32(p)
                    p += 4
                result['unwind'].append(entry)
        if header & 0x10:
            try_rva = self.int32(pos)
            pos += 4
            count, length = self.read_unsigned(try_rva)
            p = try_rva + length
            for _ in range(count):
                low, length = self.read_unsigned(p)
                p += length
                high, length = self.read_unsigned(p)
                p += length
                catch_high, length = self.read_unsigned(p)
                p += length
                handlers_rva = self.int32(p)
                p += 4
                result['try'].append((low, high, catch_high, self.decode_handlers(handlers_rva, function_start)))
        if not (header & 0x02):
            ip_rva = self.int32(pos)
            if ip_rva != 0:
                count, length = self.read_unsigned(ip_rva)
                p = ip_rva + length
                ip = 0
                entries = []
                for _ in range(count):
                    delta, length = self.read_unsigned(p)
                    p += length
                    state, length = self.read_unsigned(p)
                    p += length
                    ip += delta
                    entries.append((function_start + ip, state))
                result['ip'] = entries
        return result

    def decode_handlers(self, rva, function_start):
        handlers = []
        count, length = self.read_unsigned(rva)
        p = rva + length
        for _ in range(count):
            header = self.byte(p)
            p += 1
            handler = {'header': header, 'adjectives': None, 'type': None, 'object': None, 'conts': []}
            if header & 0x01:
                handler['adjectives'], length = self.read_unsigned(p)
                p += length
            if header & 0x02:
                handler['type'] = self.int32(p)
                p += 4
            if header & 0x04:
                handler['object'], length = self.read_unsigned(p)
                p += length
            handler['funclet'] = self.int32(p)
            p += 4
            cont_count = (header >> 4) & 3
            for _ in range(cont_count if cont_count != 3 else 0):
                if header & 0x08:
                    handler['conts'].append(self.int32(p))
                    p += 4
                else:
                    value, length = self.read_unsigned(p)
                    p += length
                    handler['conts'].append(function_start + value)
            handlers.append(handler)
        return handlers


def main():
    original = PEFile(sys.argv[1])
    recompiled = PEFile(sys.argv[2])

    # mapping of original addresses to new addresses
    labels = sorted((int(name[4:], 16) - original.image_base, address) for name, address in recompiled.symbols.items())
    label_rvas = [rva for rva, _ in labels]

    def new_address(rva, exact=True):
        index = bisect.bisect_right(label_rvas, rva) - 1
        if index < 0:
            return None
        if label_rvas[index] == rva:
            return labels[index][1]
        if exact:
            return None
        return labels[index][1] + (rva - label_rvas[index])

    new_functions = {begin: unwind for begin, end, unwind in recompiled.runtime_functions()}

    checked = 0
    errors = 0
    for begin, end, unwind in original.runtime_functions():
        handler = original.handler_data(unwind)
        if handler is None:
            continue
        handler_rva, data_rva = handler
        if original.handler_name(handler_rva) not in ('__CxxFrameHandler4', '__GSHandlerCheck_EH4'):
            continue

        new_begin = new_address(begin)
        if new_begin is None or new_begin not in new_functions:
            print('function 0x%x: not found in recompiled executable' % (original.image_base + begin))
            errors += 1
            continue

        new_handler = recompiled.handler_data(new_functions[new_begin])
        old_info = original.decode_funcinfo4(original.dword(data_rva), begin)
        new_info = recompiled.decode_funcinfo4(recompiled.dword(new_handler[1]), new_begin)

        problems = []
        if old_info['header'] != new_info['header']:
            problems.append('header')
        if old_info['ip'] is not None:
            if new_info['ip'] is None or len(old_info['ip']) != len(new_info['ip']):
                problems.append('IP to state map length')
            else:
                for (old_ip, old_state), (new_ip, new_state) in zip(old_info['ip'], new_info['ip']):
                    if new_address(old_ip) != new_ip or old_state != new_state:
                        problems.append('IP to state map entry 0x%x' % (original.image_base + old_ip))
                        break
        if len(old_info['unwind']) != len(new_info['unwind']):
            problems.append('unwind map length')
        else:
            for old_entry, new_entry in zip(old_info['unwind'], new_info['unwind']):
                if old_entry[0] != new_entry[0] or old_entry[2] != new_entry[2] or (old_entry[1] is not None and new_address(old_entry[1]) != new_entry[1]):
                    problems.append('unwind map entry')
                    break
        if len(old_info['try']) != len(new_info['try']):
            problems.append('try block map length')
        else:
            for old_try, new_try in zip(old_info['try'], new_info['try']):
                if old_try[:3] != new_try[:3] or len(old_try[3]) != len(new_try[3]):
                    problems.append('try block')
                    continue
                for old_handler, new_handler_entry in zip(old_try[3], new_try[3]):
                    if (old_handler['header'] & 0xf7) != (new_handler_entry['header'] & 0xf7) or old_handler['adjectives'] != new_handler_entry['adjectives'] or old_handler['object'] != new_handler_entry['object']:
                        problems.append('handler')
                    if new_address(old_handler['funclet']) != new_handler_entry['funclet']:
                        problems.append('catch funclet')
                    if old_handler['type'] not in (None, 0) and new_address(old_handler['type'], False) != new_handler_entry['type']:
                        problems.append('catch type')
                    if [new_address(c) for c in old_handler['conts']] != new_handler_entry['conts']:
                        problems.append('continuation address')

        checked += 1
        if problems:
            errors += 1
            print('function 0x%x: %s' % (original.image_base + begin, ', '.join(sorted(set(problems)))))

    print('functions with __CxxFrameHandler4 data: %i, errors: %i' % (checked, errors))
    sys.exit(1 if errors else 0)


main()
