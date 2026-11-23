/**
 *
 *  Copyright (C) 2026 SRW64 authors
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 */

/*
 * Linux executables (ELF64, x86-64, statically linked, not position independent).
 *
 * Executables which are not position independent don't contain relocations, so it's not
 * possible to find all pointers in data. Therefore all sections except code keep their
 * original addresses in the recompiled executable (the generated linker script places them)
 * and only the code is moved (to address CodeBase). Only references to code are converted:
 *  - RIP-relative addresses, direct calls and jumps (like in Windows executables)
 *  - immediate values which are addresses of functions (from symbol table)
 *  - jump tables with absolute addresses
 *  - pointers to code in data: addresses of functions and groups of pointers
 *    into the same function (jump tables, tables of labels)
 *  - addresses of IFUNC resolvers in IRELATIVE relocations
 * Thread local storage keeps its layout, so offsets from fs: remain valid.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SR_defs.h"
#include "SR_vars.h"

#pragma pack(1)

typedef struct PACKED _ELF64_Ehdr_ {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} ELF64_Ehdr;

typedef struct PACKED _ELF64_Shdr_ {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} ELF64_Shdr;

typedef struct PACKED _ELF64_Phdr_ {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} ELF64_Phdr;

typedef struct PACKED _ELF64_Sym_ {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} ELF64_Sym;

typedef struct PACKED _ELF64_Rela_ {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} ELF64_Rela;

#pragma pack()

#define ET_EXEC 2
#define ET_DYN  3
#define EM_X86_64 62

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3

#define SHT_SYMTAB 2
#define SHT_RELA   4
#define SHT_NOTE   7
#define SHT_NOBITS 8
#define SHT_DYNSYM 11

#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHF_TLS       0x400

#define STT_NOTYPE    0
#define STT_FUNC      2
#define STT_GNU_IFUNC 10

#define STB_LOCAL     0

#define SHN_UNDEF     0
#define SHN_LORESERVE 0xff00

#define R_X86_64_IRELATIVE 37

#define MAX_TABLE_SCAN 4096

typedef struct _elf_function_ {
    uint64_t address;
    uint64_t size;
    const char *name;
} elf_function;

static uint64_t *elf_absolute_addresses = NULL;  // addresses referenced by srw_abs_<address> symbols (defined in linker script)
static unsigned int elf_absolute_count = 0, elf_absolute_alloc = 0;
static uint8_t *elf_instruction_starts[MAX_SECTIONS];  // bitmaps of instruction starts in code sections (from linear disassembly of functions)
static elf_function *elf_functions = NULL;
static unsigned int elf_num_functions = 0;
static char *elf_strings = NULL;

// original section addresses for the linker script
typedef struct _elf_section_info_ {
    char name[64];
    uint64_t address;
    uint64_t size;
} elf_section_info;

// functions which don't return
static const char *elf_noret_functions[] = {
    "_exit", "_Exit", "exit", "abort", "__libc_fatal", "__assert_fail", "__assert_perror_fail",
    "__stack_chk_fail", "__fortify_fail", "__chk_fail", "__fortify_fail_abort", "quick_exit",
    "__cxa_throw", "__cxa_rethrow", "__cxa_bad_cast", "__cxa_bad_typeid", "__cxa_pure_virtual", "_Unwind_Resume",
    "_ZSt9terminatev", "_ZN10__cxxabiv111__terminateEPFvvE", "longjmp", "_longjmp", "siglongjmp", "__longjmp_chk",
    "pthread_exit", "__pthread_exit", "__libc_start_main", "__libc_longjmp", "__libc_siglongjmp", "err", "errx", "verr", "verrx",
    "__libc_assert_fail", "__assert_fail_base", "__malloc_assert", "malloc_printerr", "__libc_dynarray_at_failure",
    "__libc_start_call_main", "__run_exit_handlers", "__pthread_unwind", "__pthread_unwind_next", "__do_cancel",
    "_dl_signal_error", "_dl_signal_exception", "_dl_fatal_printf", "_dl_fatal_printf_impl", "__libc_message_impl", "__libc_message",
    "_Unwind_Resume_or_Rethrow", "__cxa_call_terminate", "__cxa_call_unexpected", "__cxa_bad_array_new_length",
    NULL
};


static int elf_function_compare(const void *p1, const void *p2)
{
    const elf_function *f1 = (const elf_function *)p1;
    const elf_function *f2 = (const elf_function *)p2;

    if (f1->address < f2->address) return -1;
    if (f1->address > f2->address) return 1;
    if (f1->size > f2->size) return -1;
    if (f1->size < f2->size) return 1;
    return 0;
}

static int elf_is_noret_name(const char *name)
{
    int index;

    if (name == NULL) return 0;

    for (index = 0; elf_noret_functions[index] != NULL; index++)
    {
        if (strcmp(name, elf_noret_functions[index]) == 0) return 1;
    }

    // libstdc++: std::__throw_*
    if (strncmp(name, "_ZSt", 4) == 0 && strstr(name, "__throw_") != NULL) return 1;

    return 0;
}

static int elf_add_fixup(uint64_t source, uint64_t target)
{
    uint_fast32_t SourceObject, SourceOffset, TargetObject, TargetOffset;
    fixup_data *fixup;

    if (!SR_get_section_reladr_exact(source, &SourceObject, &SourceOffset)) return 0;
    if (!SR_get_section_reladr_exact(target, &TargetObject, &TargetOffset)) return 0;
    if (SourceOffset + 8 > section[SourceObject].size_in_file) return 0;

    // original code which is not recompiled - the address stays valid
    if (section[TargetObject].type == ST_CODE && SR_is_ignored(TargetObject, TargetOffset)) return 0;

    fixup = section_fixup_list_FindEntryEqual(SourceObject, SourceOffset);
    if (fixup != NULL) return 0;

    // overlapping fixups
    fixup = section_fixup_list_FindEntryLower(SourceObject, SourceOffset);
    if (fixup != NULL && fixup->sofs + FIXUP_SIZE(fixup->type) > SourceOffset) return 0;
    fixup = section_fixup_list_FindEntryHigher(SourceObject, SourceOffset);
    if (fixup != NULL && fixup->sofs < SourceOffset + 8) return 0;

    fixup = section_fixup_list_Insert(SourceObject, SourceOffset);
    if (fixup == NULL) return -1;

    fixup->tofs = TargetOffset;
    fixup->tsec = TargetObject;
    fixup->type = FT_DIR64;
    fixup->flags = 0;

    return 1;
}

static int elf_function_name_compare(const void *p1, const void *p2)
{
    const elf_function *f1 = *(const elf_function * const *)p1;
    const elf_function *f2 = *(const elf_function * const *)p2;

    return strcmp(f1->name, f2->name);
}

static int elf_link_cold_parts(void)
{
    elf_function **by_name;
    unsigned int index, count, linked;
    char name[256];

    by_name = (elf_function **) malloc(elf_num_functions * sizeof(elf_function *) + 1);
    if (by_name == NULL) return -1;

    count = 0;
    for (index = 0; index < elf_num_functions; index++)
    {
        if (elf_functions[index].name != NULL) by_name[count++] = &(elf_functions[index]);
    }
    qsort(by_name, count, sizeof(elf_function *), elf_function_name_compare);

    linked = 0;
    for (index = 0; index < elf_num_functions; index++)
    {
        const char *cold;
        elf_function key, *pkey, **found;
        pdata_entry *cold_entry, *main_entry;

        if (elf_functions[index].name == NULL) continue;
        cold = strstr(elf_functions[index].name, ".cold");
        if (cold == NULL || cold == elf_functions[index].name || (size_t)(cold - elf_functions[index].name) >= sizeof(name)) continue;

        memcpy(name, elf_functions[index].name, cold - elf_functions[index].name);
        name[cold - elf_functions[index].name] = 0;

        key.name = name;
        pkey = &key;
        found = (elf_function **) bsearch(&pkey, by_name, count, sizeof(elf_function *), elf_function_name_compare);
        if (found == NULL) continue;

        cold_entry = (pdata_entry *) SR_find_pdata_begin((uint_fast32_t)(elf_functions[index].address - ImageBase));
        main_entry = (pdata_entry *) SR_find_pdata_begin((uint_fast32_t)((*found)->address - ImageBase));
        if (cold_entry == NULL || main_entry == NULL) continue;

        cold_entry->primary = main_entry->primary;
        linked++;
    }

    free(by_name);

    if (linked)
    {
        fprintf(stderr, "\tCold parts of functions: %u\n", linked);
    }

    return 0;
}

int SRW64_LoadElf(const uint8_t *file, int64_t file_size)
{
    const ELF64_Ehdr *header;
    const ELF64_Shdr *sections;
    const ELF64_Phdr *segments;
    const char *section_names;
    uint64_t min_address, max_address, memory_size, max_section_end;
    uint8_t *memory;
    unsigned int index, Entry;
    int is_dynamic;

    if (file_size < (int64_t)sizeof(ELF64_Ehdr))
    {
        fprintf(stderr, "Error: File too small\n");
        return -1;
    }

    header = (const ELF64_Ehdr *) file;

    if (header->e_ident[4] != 2 || header->e_ident[5] != 1 || header->e_machine != EM_X86_64)
    {
        fprintf(stderr, "Error: only 64-bit little-endian x86-64 ELF executables are supported\n");
        return -1;
    }

    if (header->e_type == ET_DYN)
    {
        fprintf(stderr, "Error: position independent executables and shared libraries are not supported\n");
        return -1;
    }

    if (header->e_type != ET_EXEC)
    {
        fprintf(stderr, "Error: not an executable\n");
        return -1;
    }

    if (header->e_phoff == 0 || header->e_phnum == 0 || header->e_phentsize != sizeof(ELF64_Phdr) ||
        (int64_t)(header->e_phoff + header->e_phnum * sizeof(ELF64_Phdr)) > file_size)
    {
        fprintf(stderr, "Error: invalid program headers\n");
        return -1;
    }

    if (header->e_shoff == 0 || header->e_shnum == 0 || header->e_shentsize != sizeof(ELF64_Shdr) ||
        (int64_t)(header->e_shoff + header->e_shnum * sizeof(ELF64_Shdr)) > file_size ||
        header->e_shstrndx >= header->e_shnum)
    {
        fprintf(stderr, "Error: executables without section headers are not supported\n");
        return -1;
    }

    segments = (const ELF64_Phdr *) (file + header->e_phoff);
    sections = (const ELF64_Shdr *) (file + header->e_shoff);

    if ((int64_t)(sections[header->e_shstrndx].sh_offset + sections[header->e_shstrndx].sh_size) > file_size)
    {
        fprintf(stderr, "Error: invalid section names\n");
        return -1;
    }
    section_names = (const char *) (file + sections[header->e_shstrndx].sh_offset);

    // image base and size from loaded segments
    min_address = UINT64_MAX;
    max_address = 0;
    is_dynamic = 0;
    for (index = 0; index < header->e_phnum; index++)
    {
        if (segments[index].p_type == PT_LOAD)
        {
            if (segments[index].p_vaddr < min_address) min_address = segments[index].p_vaddr;
            if (segments[index].p_vaddr + segments[index].p_memsz > max_address) max_address = segments[index].p_vaddr + segments[index].p_memsz;
        }
        else if (segments[index].p_type == PT_INTERP || segments[index].p_type == PT_DYNAMIC)
        {
            is_dynamic = 1;
        }
    }

    if (is_dynamic)
    {
        fprintf(stderr, "Error: dynamically linked executables are not supported (only statically linked executables)\n");
        return -1;
    }

    if (min_address == UINT64_MAX)
    {
        fprintf(stderr, "Error: no loadable segments\n");
        return -1;
    }

    ImageBase = min_address & ~UINT64_C(0xfff);
    SizeOfImage = (uint_fast32_t)(max_address - ImageBase);
    SizeOfHeaders = 0;
    ExceptionDirRVA = 0;
    ExceptionDirSize = 0;
    DelayImportDirRVA = 0;
    DelayImportDirSize = 0;
    LargeAddressAware = 1;
    NumAbsolute32Relocations = 0;

    if (max_address > UINT64_C(0x70000000))
    {
        fprintf(stderr, "Error: the executable is not in the low 2 GB of address space\n");
        return -1;
    }

    fprintf(stderr, "\tImage base: 0x%" PRIx64 "\n", ImageBase);
    fprintf(stderr, "\tSize of image: 0x%x\n", (unsigned int)SizeOfImage);

    // allocate memory for sections
    memory_size = 0;
    for (index = 0; index < header->e_shnum; index++)
    {
        if (!(sections[index].sh_flags & SHF_ALLOC) || sections[index].sh_size == 0) continue;
        memory_size += (sections[index].sh_size + 15) & ~UINT64_C(15);
    }

    if (memory_size > UINT64_C(0x40000000))
    {
        fprintf(stderr, "Error: Image too big\n");
        return -1;
    }

    SR_CodeBase = (uint8_t *) malloc((size_t)memory_size + 16);
    if (SR_CodeBase == NULL)
    {
        fprintf(stderr, "Error: Not enough memory\n");
        return -1;
    }
    memset(SR_CodeBase, 0, (size_t)memory_size + 16);
    memory = SR_CodeBase;

    // sections
    num_sections = 0;
    import_list = NULL;
    FirstSectionRVA = SizeOfImage;
    max_section_end = 0;
    for (index = 0; index < header->e_shnum; index++)
    {
        const ELF64_Shdr *shdr;
        const char *name;

        shdr = &(sections[index]);
        if (!(shdr->sh_flags & SHF_ALLOC) || shdr->sh_size == 0) continue;

        name = (shdr->sh_name < sections[header->e_shstrndx].sh_size)?(section_names + shdr->sh_name):"";

        // notes are not needed (and the space after ELF headers is needed for new program headers)
        if (shdr->sh_type == SHT_NOTE) continue;

        if (num_sections >= MAX_SECTIONS)
        {
            fprintf(stderr, "Error: Too many sections\n");
            return -1;
        }

        Entry = num_sections;
        num_sections++;

        memset(&(section[Entry]), 0, sizeof(section_data));
        strncpy(section[Entry].name, name, sizeof(section[Entry].name) - 1);
        section[Entry].start = shdr->sh_addr;
        section[Entry].virtaddr = (uint_fast32_t)(shdr->sh_addr - ImageBase);
        section[Entry].size = (uint_fast32_t)shdr->sh_size;
        section[Entry].size_in_file = (shdr->sh_type == SHT_NOBITS)?0:(uint_fast32_t)shdr->sh_size;
        section[Entry].adr = memory;
        section[Entry].flags = (uint_fast32_t)shdr->sh_flags;
        section[Entry].align = (shdr->sh_addralign > 1)?(uint_fast32_t)shdr->sh_addralign:1;
        section[Entry].tls = (shdr->sh_flags & SHF_TLS)?1:0;

        if (shdr->sh_flags & SHF_EXECINSTR)
        {
            section[Entry].type = ST_CODE;
        }
        else if (shdr->sh_type == SHT_NOBITS)
        {
            section[Entry].type = ST_UDATA;
        }
        else if (shdr->sh_flags & SHF_WRITE)
        {
            section[Entry].type = ST_DATA;
        }
        else
        {
            section[Entry].type = ST_RDATA;
        }

        if (section[Entry].size_in_file != 0)
        {
            if ((int64_t)(shdr->sh_offset + shdr->sh_size) > file_size)
            {
                fprintf(stderr, "Error: section outside of file: %s\n", name);
                return -1;
            }
            memcpy(memory, file + shdr->sh_offset, (size_t)shdr->sh_size);
        }

        if (!(section[Entry].tls && section[Entry].type == ST_UDATA))
        {
            SR_add_orig_section(section[Entry].virtaddr, section[Entry].size, memory);

            if (section[Entry].virtaddr < FirstSectionRVA) FirstSectionRVA = section[Entry].virtaddr;
            if (section[Entry].start + section[Entry].size > max_section_end) max_section_end = section[Entry].start + section[Entry].size;
        }

        memory += (shdr->sh_size + 15) & ~UINT64_C(15);

        fprintf(stderr, "name: %s (seg%.2i) - 0x%" PRIx64 " - %u - type %i%s\n", section[Entry].name, Entry + 1, section[Entry].start, (unsigned int)section[Entry].size, section[Entry].type, (section[Entry].tls)?" - tls":"");
    }

    // address of recompiled code
    CodeBase = ((max_section_end + UINT64_C(0xfffff)) & ~UINT64_C(0xfffff)) + UINT64_C(0x100000);

    // entry point
    if (!SR_get_section_reladr_exact(header->e_entry, &EIPObjectNum, &EIP))
    {
        fprintf(stderr, "Error: entry point is not in any section\n");
        return -1;
    }
    fprintf(stderr, "\tEntry point: 0x%" PRIx64 "\n", header->e_entry);

    // functions from symbol table
    {
        const ELF64_Shdr *symtab, *strtab;
        const ELF64_Sym *symbols;
        uint64_t num_symbols, symbol;

        symtab = NULL;
        for (index = 0; index < header->e_shnum; index++)
        {
            if (sections[index].sh_type == SHT_SYMTAB) symtab = &(sections[index]);
        }
        if (symtab == NULL)
        {
            for (index = 0; index < header->e_shnum; index++)
            {
                if (sections[index].sh_type == SHT_DYNSYM) symtab = &(sections[index]);
            }
        }

        if (symtab == NULL || symtab->sh_link >= header->e_shnum)
        {
            fprintf(stderr, "Warning: executable doesn't contain symbols - code is found only from entry point and references\n");
        }
        else
        {
            strtab = &(sections[symtab->sh_link]);
            if ((int64_t)(symtab->sh_offset + symtab->sh_size) > file_size || (int64_t)(strtab->sh_offset + strtab->sh_size) > file_size)
            {
                fprintf(stderr, "Error: invalid symbol table\n");
                return -1;
            }

            elf_strings = (char *) malloc((size_t)strtab->sh_size + 1);
            if (elf_strings == NULL)
            {
                fprintf(stderr, "Error: Not enough memory\n");
                return -1;
            }
            memcpy(elf_strings, file + strtab->sh_offset, (size_t)strtab->sh_size);
            elf_strings[strtab->sh_size] = 0;

            symbols = (const ELF64_Sym *) (file + symtab->sh_offset);
            num_symbols = symtab->sh_size / sizeof(ELF64_Sym);

            elf_functions = (elf_function *) malloc((size_t)num_symbols * sizeof(elf_function) + 1);
            if (elf_functions == NULL)
            {
                fprintf(stderr, "Error: Not enough memory\n");
                return -1;
            }

            elf_num_functions = 0;
            for (symbol = 0; symbol < num_symbols; symbol++)
            {
                uint_fast32_t SecNum, RelAdr;
                unsigned int type;

                type = symbols[symbol].st_info & 0x0f;
                if (type == STT_NOTYPE)
                {
                    // functions in assembler without type (e.g. __strcpy_ssse3 in older glibc) or alternative entry points
                    if (symbols[symbol].st_size == 0 && (symbols[symbol].st_info >> 4) == STB_LOCAL) continue;
                    if (symbols[symbol].st_name >= strtab->sh_size) continue;
                    if (elf_strings[symbols[symbol].st_name] == 0 || elf_strings[symbols[symbol].st_name] == '.' || elf_strings[symbols[symbol].st_name] == '$') continue;
                }
                else if (type != STT_FUNC && type != STT_GNU_IFUNC) continue;
                if (symbols[symbol].st_shndx == SHN_UNDEF || symbols[symbol].st_shndx >= SHN_LORESERVE) continue;
                if (!SR_get_section_reladr_exact(symbols[symbol].st_value, &SecNum, &RelAdr)) continue;
                if (section[SecNum].type != ST_CODE) continue;

                elf_functions[elf_num_functions].address = symbols[symbol].st_value;
                elf_functions[elf_num_functions].size = symbols[symbol].st_size;
                elf_functions[elf_num_functions].name = (symbols[symbol].st_name < strtab->sh_size)?(elf_strings + symbols[symbol].st_name):NULL;
                elf_num_functions++;

                if (elf_is_noret_name(elf_functions[elf_num_functions - 1].name))
                {
                    section_noret_list_Insert(SecNum, RelAdr);
                }
            }

            qsort(elf_functions, elf_num_functions, sizeof(elf_function), elf_function_compare);

            // function list (the same structure as exception directory in Windows executables)
            pdata_list = (pdata_entry *) malloc(elf_num_functions * sizeof(pdata_entry) + 1);
            if (pdata_list == NULL)
            {
                fprintf(stderr, "Error: Not enough memory\n");
                return -1;
            }

            pdata_count = 0;
            for (index = 0; index < elf_num_functions; index++)
            {
                uint64_t end;
                uint_fast32_t SecNum, RelAdr;

                // duplicate addresses (aliases)
                if (pdata_count != 0 && pdata_list[pdata_count - 1].begin == elf_functions[index].address - ImageBase) continue;

                SR_get_section_reladr_exact(elf_functions[index].address, &SecNum, &RelAdr);

                end = elf_functions[index].address + elf_functions[index].size;
                if (elf_functions[index].size == 0)
                {
                    // size unknown - function ends at next function or section end
                    end = section[SecNum].start + section[SecNum].size;
                    if (index + 1 < elf_num_functions && elf_functions[index + 1].address < end) end = elf_functions[index + 1].address;
                    if (end == elf_functions[index].address) end++;
                }

                pdata_list[pdata_count].begin = (uint_fast32_t)(elf_functions[index].address - ImageBase);
                pdata_list[pdata_count].end = (uint_fast32_t)(end - ImageBase);
                pdata_list[pdata_count].unwind = 0;
                pdata_list[pdata_count].primary = pdata_list[pdata_count].begin;
                pdata_list[pdata_count].pofs = 0;
                pdata_list[pdata_count].psec = 0;
                pdata_list[pdata_count].ignored = 0;

                // overlapping functions (e.g. nested aliases with different sizes) are cut
                if (pdata_count != 0 && pdata_list[pdata_count - 1].end > pdata_list[pdata_count].begin)
                {
                    pdata_list[pdata_count - 1].end = pdata_list[pdata_count].begin;
                }

                pdata_count++;
            }

            // cold parts of functions (GCC -freorder-blocks-and-partition: foo.cold, foo.cold.N) belong to the function
            if (elf_link_cold_parts()) return -1;

            fprintf(stderr, "\tFunctions in symbol table: %u\n", pdata_count);
        }
    }

    // addresses of IFUNC resolvers in IRELATIVE relocations
    {
        unsigned int count;

        count = 0;
        for (index = 0; index < header->e_shnum; index++)
        {
            const ELF64_Rela *relocations;
            uint64_t num_relocations, relocation;

            if (sections[index].sh_type != SHT_RELA || !(sections[index].sh_flags & SHF_ALLOC)) continue;
            if ((int64_t)(sections[index].sh_offset + sections[index].sh_size) > file_size) continue;

            relocations = (const ELF64_Rela *) (file + sections[index].sh_offset);
            num_relocations = sections[index].sh_size / sizeof(ELF64_Rela);

            for (relocation = 0; relocation < num_relocations; relocation++)
            {
                if ((relocations[relocation].r_info & 0xffffffff) != R_X86_64_IRELATIVE) continue;

                if (elf_add_fixup(sections[index].sh_addr + relocation * sizeof(ELF64_Rela) + 16, (uint64_t)relocations[relocation].r_addend) > 0)
                {
                    count++;
                }
            }
        }

        if (count)
        {
            fprintf(stderr, "\tIRELATIVE relocations: %u\n", count);
        }
    }

    fprintf(stderr, "\tAddress of recompiled code: 0x%" PRIx64 "\n", CodeBase);

    return 0;
}

static const char *elf_function_name(uint_fast32_t rva)
{
    unsigned int low, high, middle;

    low = 0;
    high = elf_num_functions;
    while (low < high)
    {
        middle = (low + high) / 2;
        if (elf_functions[middle].address < ImageBase + rva) low = middle + 1;
        else high = middle;
    }

    if (low < elf_num_functions && elf_functions[low].address == ImageBase + rva && elf_functions[low].name != NULL) return elf_functions[low].name;
    return "";
}

static int elf_register_absolute(uint64_t address)
{
    if (elf_absolute_count != 0 && elf_absolute_addresses[elf_absolute_count - 1] == address) return 0;

    if (elf_absolute_count >= elf_absolute_alloc)
    {
        uint64_t *new_addresses;

        new_addresses = (uint64_t *) realloc(elf_absolute_addresses, (elf_absolute_alloc + 256) * sizeof(uint64_t));
        if (new_addresses == NULL) return -1;
        elf_absolute_addresses = new_addresses;
        elf_absolute_alloc += 256;
    }

    elf_absolute_addresses[elf_absolute_count] = address;
    elf_absolute_count++;
    return 0;
}

// label for address which is not recompiled: original code (functions which are kept) or address outside of sections (e.g. undefined weak function)
// - symbol srw_abs_<address> with absolute value (NASM doesn't create relocations for jumps to numbers)
int SR_elf_absolute_label(char *cbuf, uint64_t address)
{
    uint_fast32_t SecNum, RelAdr;

    if (InputFormat != INPUT_ELF) return 0;

    if (SR_get_section_reladr_exact(address, &SecNum, &RelAdr))
    {
        if (section[SecNum].type != ST_CODE || !keep_original_code) return 0;
        if (!SR_is_ignored(SecNum, RelAdr)) return 0;
    }

    sprintf(cbuf, "srw_abs_%" PRIX64, address);
    elf_register_absolute(address);
    return 1;
}

static int elf_compare_addresses(const void *p1, const void *p2)
{
    uint64_t a1 = *(const uint64_t *)p1;
    uint64_t a2 = *(const uint64_t *)p2;

    return (a1 < a2)?-1:((a1 > a2)?1:0);
}

static void elf_sort_absolute(void)
{
    unsigned int index, count;

    if (elf_absolute_count == 0) return;

    qsort(elf_absolute_addresses, elf_absolute_count, sizeof(uint64_t), elf_compare_addresses);

    count = 1;
    for (index = 1; index < elf_absolute_count; index++)
    {
        if (elf_absolute_addresses[index] != elf_absolute_addresses[count - 1])
        {
            elf_absolute_addresses[count++] = elf_absolute_addresses[index];
        }
    }
    elf_absolute_count = count;
}

void SR_elf_write_externs(FILE *fout)
{
    unsigned int index;

    elf_sort_absolute();

    if (elf_absolute_count == 0) return;

    fprintf(fout, "\n; original code and other addresses which are not recompiled (defined in linker script)\n");
    for (index = 0; index < elf_absolute_count; index++)
    {
        fprintf(fout, "extern srw_abs_%" PRIX64 "\n", elf_absolute_addresses[index]);
    }
}

int SR_elf_is_instruction_start(uint64_t address)
{
    uint_fast32_t SecNum, RelAdr;

    if (InputFormat != INPUT_ELF) return 1;
    if (!SR_get_section_reladr_exact(address, &SecNum, &RelAdr)) return 0;
    if (elf_instruction_starts[SecNum] == NULL) return 1;

    // code outside of known functions - no information
    if (SR_find_pdata((uint_fast32_t)(address - ImageBase)) == NULL) return 1;

    return (elf_instruction_starts[SecNum][RelAdr >> 3] & (1 << (RelAdr & 7)))?1:0;
}

// linear disassembly of functions: instruction starts and addresses in data referenced by code
// (labels at referenced addresses end jump tables of unknown size)
static int elf_prescan_functions(void)
{
    unsigned int index, Entry, refs;
    uint_fast32_t SecNum, RelAdr, DataSec, DataOfs;
    uint64_t begin, end, address;
    output_data *output;
    ud_t ud_tmp;
    int opnum;

    for (Entry = 0; Entry < num_sections; Entry++)
    {
        elf_instruction_starts[Entry] = NULL;
        if (section[Entry].type != ST_CODE) continue;

        elf_instruction_starts[Entry] = (uint8_t *) calloc(1, (section[Entry].size >> 3) + 1);
        if (elf_instruction_starts[Entry] == NULL) return -1;
    }

    refs = 0;
    for (index = 0; index < pdata_count; index++)
    {
        begin = ImageBase + pdata_list[index].begin;
        end = ImageBase + pdata_list[index].end;

        if (!SR_get_section_reladr_exact(begin, &SecNum, &RelAdr) || section[SecNum].type != ST_CODE) continue;
        if (end > section[SecNum].start + section[SecNum].size_in_file) end = section[SecNum].start + section[SecNum].size_in_file;
        if (end <= begin) continue;

        ud_init(&ud_tmp);
        ud_set_mode(&ud_tmp, 64);
        ud_set_vendor(&ud_tmp, UD_VENDOR_ANY);
        ud_set_input_buffer(&ud_tmp, &(section[SecNum].adr[RelAdr]), (size_t)(end - begin));
        ud_set_pc(&ud_tmp, begin);

        address = begin;
        while (address < end)
        {
            unsigned int length;

            length = ud_decode(&ud_tmp);
            if (length == 0 || ud_tmp.mnemonic == UD_Iinvalid)
            {
                // rest of function is unknown - all addresses are accepted
                for (; address < end; address++)
                {
                    RelAdr = (uint_fast32_t)(address - section[SecNum].start);
                    elf_instruction_starts[SecNum][RelAdr >> 3] |= (uint8_t)(1 << (RelAdr & 7));
                }
                break;
            }

            RelAdr = (uint_fast32_t)(address - section[SecNum].start);
            elf_instruction_starts[SecNum][RelAdr >> 3] |= (uint8_t)(1 << (RelAdr & 7));

            for (opnum = 0; opnum < 4; opnum++)
            {
                const struct ud_operand *op;
                uint64_t target;

                op = &(ud_tmp.operand[opnum]);
                if (op->type == UD_OP_MEM && op->base == UD_R_RIP && op->offset == 32)
                {
                    target = address + length + (int64_t)op->lval.sdword;
                }
                else if (op->type == UD_OP_MEM && op->base == UD_NONE && op->offset == 32)
                {
                    target = (uint64_t)(int64_t)op->lval.sdword;
                }
                else continue;

                if (!SR_get_section_reladr_exact(target, &DataSec, &DataOfs) || section[DataSec].type == ST_CODE) continue;
                if (section[DataSec].tls && section[DataSec].type == ST_UDATA) continue;

                output = section_output_list_FindEntryEqual(DataSec, DataOfs);
                if (output != NULL && !output->has_label)
                {
                    output->has_label = 1;
                    refs++;
                }
            }

            address += length;
        }
    }

    fprintf(stderr, "\tAddresses in data referenced by code: %u\n", refs);

    return 0;
}

int SR_elf_is_function_start(uint64_t address)
{
    unsigned int Entry;

    if (address < ImageBase || address >= ImageBase + SizeOfImage) return 0;

    if (SR_find_pdata_begin((uint_fast32_t)(address - ImageBase)) == NULL) return 0;

    // address is also the end of a data section (e.g. __rela_iplt_end at the start of .init) - ambiguous
    for (Entry = 0; Entry < num_sections; Entry++)
    {
        if (section[Entry].type == ST_CODE || (section[Entry].tls && section[Entry].type == ST_UDATA)) continue;
        if (section[Entry].start + section[Entry].size == address) return 0;
    }

    return 1;
}

static int elf_is_code_pointer_section(unsigned int Entry)
{
    static const char *excluded[] = { ".eh_frame", ".eh_frame_hdr", ".gcc_except_table", ".got.plt", NULL };
    int index;

    if (section[Entry].type != ST_DATA && section[Entry].type != ST_RDATA) return 0;

    for (index = 0; excluded[index] != NULL; index++)
    {
        if (strcmp(section[Entry].name, excluded[index]) == 0) return 0;
    }

    return 1;
}

// returns address of first instruction of function which can't be decoded (e.g. AVX-512 instructions are not supported), 0 = all instructions can be decoded
static uint64_t elf_function_undecodable(uint64_t begin, uint64_t end)
{
    uint_fast32_t SecNum, RelAdr;
    uint64_t address;
    ud_t ud_tmp;
    unsigned int length;

    if (!SR_get_section_reladr_exact(begin, &SecNum, &RelAdr)) return 0;
    if (end > section[SecNum].start + section[SecNum].size_in_file) end = section[SecNum].start + section[SecNum].size_in_file;
    if (end <= begin) return 0;

    ud_init(&ud_tmp);
    ud_set_mode(&ud_tmp, 64);
    ud_set_vendor(&ud_tmp, UD_VENDOR_ANY);
    ud_set_input_buffer(&ud_tmp, &(section[SecNum].adr[RelAdr]), (size_t)(end - begin));
    ud_set_pc(&ud_tmp, begin);

    address = begin;
    while ((length = ud_decode(&ud_tmp)) != 0)
    {
        if (ud_tmp.mnemonic == UD_Iinvalid) return address;
        address += length;
    }

    return 0;
}

// processing after loading of information files (before disassembly)
int SR_elf_process(void)
{
    unsigned int index, Entry, functions, single, grouped, kept;
    uint_fast32_t SecNum, RelAdr, ofs;
    output_data *output;
    alias_data *alias;

    if (elf_prescan_functions()) return -1;

    // functions which can't be recompiled are kept in original code (at original address)
    kept = 0;
    if (keep_original_code)
    {
        for (index = 0; index < pdata_count; index++)
        {
            char name[32];
            int *ignored_area_value;
            uint64_t invalid_address;

            invalid_address = elf_function_undecodable(ImageBase + pdata_list[index].begin, ImageBase + pdata_list[index].end);
            if (invalid_address == 0) continue;
            if (!SR_get_section_reladr_exact(ImageBase + pdata_list[index].begin, &SecNum, &RelAdr)) continue;

            ignored_area_value = section_ignored_list_Insert(SecNum, RelAdr);
            if (ignored_area_value == NULL) return -1;
            *ignored_area_value = (int)(pdata_list[index].end - pdata_list[index].begin);

            // references to the function use original address
            sprintf(name, "srw_abs_%" PRIX64, ImageBase + pdata_list[index].begin);
            if (section_extrn_list_Insert(SecNum, RelAdr, name, NULL) == NULL) return -1;
            elf_register_absolute(ImageBase + pdata_list[index].begin);

            {
                uint_fast32_t InvSec, InvOfs, byte;
                char bytes[64];

                bytes[0] = 0;
                if (SR_get_section_reladr_exact(invalid_address, &InvSec, &InvOfs))
                {
                    for (byte = 0; byte < 8 && InvOfs + byte < section[InvSec].size_in_file; byte++)
                    {
                        sprintf(&(bytes[strlen(bytes)]), " %02x", section[InvSec].adr[InvOfs + byte]);
                    }
                }
                fprintf(stderr, "Note: function is kept in original code: 0x%" PRIx64 " %s (instruction at 0x%" PRIx64 ":%s)\n", ImageBase + pdata_list[index].begin, elf_function_name(pdata_list[index].begin), invalid_address, bytes);
            }
            kept++;
        }
    }

    // functions are code entries
    functions = 0;
    for (index = 0; index < pdata_count; index++)
    {
        if (!SR_get_section_reladr_exact(ImageBase + pdata_list[index].begin, &SecNum, &RelAdr)) continue;

        pdata_list[index].ignored = SR_is_ignored(SecNum, RelAdr);
        if (pdata_list[index].ignored) continue;
        if (!use_pdata) continue;
        if (section_nocode_list_CanFindEntryEqual(SecNum, RelAdr)) continue;

        section_entry_list_Insert(SecNum, RelAdr);
        functions++;

        output = section_output_list_FindEntryEqual(SecNum, RelAdr);
        if (output != NULL) output->has_label = 1;
    }

    // entry point gets global name (used in linker script)
    alias = section_alias_list_FindEntryEqual(EIPObjectNum, EIP);
    if (alias == NULL)
    {
        alias = section_alias_list_Insert(EIPObjectNum, EIP, "srw_entry");
        if (alias == NULL) return -1;
    }

    // pointers to code in data
    single = 0;
    grouped = 0;
    for (Entry = 0; Entry < num_sections; Entry++)
    {
        uint_fast32_t group_start, group_count;
        const pdata_entry *group_function;

        if (!elf_is_code_pointer_section(Entry)) continue;

        group_start = 0;
        group_count = 0;
        group_function = NULL;

        for (ofs = (uint_fast32_t)((8 - (section[Entry].start & 7)) & 7); ofs + 8 <= section[Entry].size_in_file; ofs += 8)
        {
            uint64_t value;
            const pdata_entry *function;
            int is_start;

            memcpy(&value, &(section[Entry].adr[ofs]), 8);

            function = NULL;
            is_start = 0;
            if (SR_get_section_reladr_exact(value, &SecNum, &RelAdr) && section[SecNum].type == ST_CODE && !SR_is_ignored(SecNum, RelAdr))
            {
                function = SR_find_pdata((uint_fast32_t)(value - ImageBase));
                if (function != NULL && function->begin == (uint_fast32_t)(value - ImageBase)) is_start = 1;
            }

            // group of pointers into the same function
            if (function != NULL && group_count != 0 && group_function != NULL && function->primary == group_function->primary && group_start + 8 * group_count == ofs)
            {
                group_count++;
            }
            else
            {
                if (group_count >= 2)
                {
                    uint_fast32_t item;

                    for (item = 0; item < group_count; item++)
                    {
                        uint64_t item_value;

                        memcpy(&item_value, &(section[Entry].adr[group_start + 8 * item]), 8);
                        if (elf_add_fixup(section[Entry].start + group_start + 8 * item, item_value) > 0) grouped++;
                    }
                }

                group_start = ofs;
                group_count = (function != NULL)?1:0;
                group_function = function;
            }

            // address of function
            if (is_start)
            {
                if (elf_add_fixup(section[Entry].start + ofs, value) > 0) single++;
            }
        }

        if (group_count >= 2)
        {
            uint_fast32_t item;

            for (item = 0; item < group_count; item++)
            {
                uint64_t item_value;

                memcpy(&item_value, &(section[Entry].adr[group_start + 8 * item]), 8);
                if (elf_add_fixup(section[Entry].start + group_start + 8 * item, item_value) > 0) grouped++;
            }
        }
    }

    if (kept)
    {
        fprintf(stderr, "\tFunctions kept in original code: %u\n", kept);
    }
    fprintf(stderr, "\tFunctions used as code entries: %u\n", functions);
    fprintf(stderr, "\tPointers to functions in data: %u\n", single);
    fprintf(stderr, "\tPointers into functions in data (tables): %u\n", grouped);

    return 0;
}

static void elf_output_name(char *buf, const char *name)
{
    char *dst;

    dst = buf;
    dst += sprintf(dst, "srw");
    if (name[0] != '.') *(dst++) = '.';
    for (; *name; name++)
    {
        *(dst++) = ((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z') || (*name >= '0' && *name <= '9') || *name == '.' || *name == '_')?*name:'_';
    }
    *dst = 0;
}

// linker script which places sections at original addresses (except code)
int SR_write_linker_script(const char *fname)
{
    FILE *fout;
    char *ldname, *dotptr, outname[96];
    unsigned int Entry, index, order[MAX_SECTIONS], count;
    alias_data *alias;

    ldname = (char *) malloc(strlen(fname) + 4);
    if (ldname == NULL) return -1;
    strcpy(ldname, fname);
    dotptr = strrchr(ldname, '.');
    if (dotptr != NULL) *dotptr = 0;
    strcat(ldname, ".ld");

    fout = fopen(ldname, "wt");
    free(ldname);
    if (fout == NULL) return -1;

    alias = section_alias_list_FindEntryEqual(EIPObjectNum, EIP);

    // sections ordered by address
    count = 0;
    for (Entry = 0; Entry < num_sections; Entry++)
    {
        if (section[Entry].type == ST_CODE) continue;
        if (section[Entry].type != ST_DATA && section[Entry].type != ST_RDATA && section[Entry].type != ST_UDATA) continue;
        order[count++] = Entry;
    }
    for (Entry = 1; Entry < count; Entry++)
    {
        for (index = Entry; index > 0 && section[order[index - 1]].start > section[order[index]].start; index--)
        {
            unsigned int tmp = order[index - 1];
            order[index - 1] = order[index];
            order[index] = tmp;
        }
    }

    fprintf(fout, "/* Linker script generated by SRW64: original sections (except code) are placed at original addresses */\n\n");
    fprintf(fout, "ENTRY(%s)\n\n", (alias != NULL)?alias->proc:"srw_entry");

    // original code and other addresses which are not recompiled
    elf_sort_absolute();
    for (index = 0; index < elf_absolute_count; index++)
    {
        fprintf(fout, "srw_abs_%" PRIX64 " = 0x%" PRIx64 ";\n", elf_absolute_addresses[index], elf_absolute_addresses[index]);
    }
    if (elf_absolute_count != 0) fprintf(fout, "\n");
    fprintf(fout, "SECTIONS\n{\n");
    fprintf(fout, "    . = 0x%" PRIx64 " + SIZEOF_HEADERS;\n\n", ImageBase);

    for (index = 0; index < count; index++)
    {
        Entry = order[index];
        elf_output_name(outname, section[Entry].name);
        fprintf(fout, "    %s 0x%" PRIx64 " : { KEEP(*(%s)) }\n", outname, section[Entry].start, outname);
    }

    if (keep_original_code)
    {
        fprintf(fout, "\n    /* original code (functions which are not recompiled) */\n");
        for (Entry = 0; Entry < num_sections; Entry++)
        {
            if (section[Entry].type != ST_CODE) continue;
            elf_output_name(outname, section[Entry].name);
            fprintf(fout, "    %s.orig 0x%" PRIx64 " : { KEEP(*(%s.orig)) }\n", outname, section[Entry].start, outname);
        }
    }

    fprintf(fout, "\n    /* recompiled code */\n");
    fprintf(fout, "    .text 0x%" PRIx64 " :\n    {\n", CodeBase);
    for (Entry = 0; Entry < num_sections; Entry++)
    {
        if (section[Entry].type != ST_CODE) continue;
        elf_output_name(outname, section[Entry].name);
        fprintf(fout, "        KEEP(*(%s))\n", outname);
    }
    fprintf(fout, "        *(.text .text.*)\n    }\n\n");

    fprintf(fout, "    /* sections of other object files */\n");
    fprintf(fout, "    .rodata : { *(.rodata .rodata.*) }\n");
    fprintf(fout, "    .data : { *(.data .data.*) }\n");
    fprintf(fout, "    .bss : { *(.bss .bss.*) *(COMMON) }\n\n");
    fprintf(fout, "    /DISCARD/ : { *(.comment) *(.note.gnu.property) *(.eh_frame) }\n");
    fprintf(fout, "}\n");

    fclose(fout);

    return 0;
}

void SR_elf_output_section_name(char *buf, unsigned int Entry)
{
    elf_output_name(buf, section[Entry].name);
}

void SR_elf_free(void)
{
    unsigned int Entry;

    if (elf_absolute_addresses != NULL)
    {
        free(elf_absolute_addresses);
        elf_absolute_addresses = NULL;
    }
    elf_absolute_count = 0;
    elf_absolute_alloc = 0;

    for (Entry = 0; Entry < MAX_SECTIONS; Entry++)
    {
        if (elf_instruction_starts[Entry] != NULL)
        {
            free(elf_instruction_starts[Entry]);
            elf_instruction_starts[Entry] = NULL;
        }
    }

    if (elf_functions != NULL)
    {
        free(elf_functions);
        elf_functions = NULL;
    }
    elf_num_functions = 0;

    if (elf_strings != NULL)
    {
        free(elf_strings);
        elf_strings = NULL;
    }
}
