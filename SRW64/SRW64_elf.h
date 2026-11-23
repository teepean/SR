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

// ELF64 structures (Linux executables)

#if !defined(_SRW64_ELF_H_INCLUDED_)
#define _SRW64_ELF_H_INCLUDED_

#include <stdint.h>

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


typedef struct PACKED _ELF64_Dyn_ {
    int64_t  d_tag;
    uint64_t d_val;
} ELF64_Dyn;

typedef struct PACKED _ELF64_Verneed_ {
    uint16_t vn_version;
    uint16_t vn_cnt;
    uint32_t vn_file;
    uint32_t vn_aux;
    uint32_t vn_next;
} ELF64_Verneed;

typedef struct PACKED _ELF64_Vernaux_ {
    uint32_t vna_hash;
    uint16_t vna_flags;
    uint16_t vna_other;
    uint32_t vna_name;
    uint32_t vna_next;
} ELF64_Vernaux;

#define SHT_PROGBITS 1
#define SHT_RELR     19
#define SHT_DYNAMIC  6
#define SHT_GNU_verneed 0x6ffffffe
#define SHT_GNU_versym  0x6fffffff

#define STB_GLOBAL    1
#define STB_WEAK      2
#define STB_GNU_UNIQUE 10

#define STT_OBJECT    1
#define STT_TLS       6

#define DT_NULL       0
#define DT_NEEDED     1
#define DT_INIT       12
#define DT_FINI       13
#define DT_RPATH      15
#define DT_RUNPATH    29

#define R_X86_64_64        1
#define R_X86_64_COPY      5
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8
#define R_X86_64_DTPMOD64  16
#define R_X86_64_DTPOFF64  17
#define R_X86_64_TPOFF64   18

#endif
