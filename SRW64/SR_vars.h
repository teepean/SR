/**
 *
 *  Copyright (C) 2016-2025 Roman Pauer
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

#if !defined(_SR_VARS_H_INCLUDED_)
#define _SR_VARS_H_INCLUDED_

#include <stdio.h>
#include <udis86.h>

#include "SR_defs.h"

#if defined(DEFINE_VARIABLES)
	#define EXTERNAL_VARIABLE
#else
	#define EXTERNAL_VARIABLE extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

EXTERNAL_VARIABLE section_data section[MAX_SECTIONS];   // array of sections
EXTERNAL_VARIABLE unsigned int num_sections;            // number of sections
EXTERNAL_VARIABLE uint8_t *SR_CodeBase;                 // allocated data

EXTERNAL_VARIABLE int InputFormat;                      // INPUT_PE or INPUT_ELF
EXTERNAL_VARIABLE uint64_t CodeBase;                    // address of recompiled code (ELF)
EXTERNAL_VARIABLE int ElfPie;                           // position independent executable (ELF)
EXTERNAL_VARIABLE int ElfRelocatedPointers;             // pointers to code are known from relocations (position independent executable, executable with relative relocations)
EXTERNAL_VARIABLE uint64_t ElfLoadBias;                 // address of position independent executable (ELF)
EXTERNAL_VARIABLE uint64_t ElfHeadersBase;              // address of headers of new executable (ELF, 0 = image base)
EXTERNAL_VARIABLE uint64_t ImageBase;                   // image base
EXTERNAL_VARIABLE uint_fast32_t SizeOfImage;            // size of image
EXTERNAL_VARIABLE uint_fast32_t SizeOfHeaders;          // size of headers
EXTERNAL_VARIABLE uint_fast32_t FirstSectionRVA;        // lowest section RVA
EXTERNAL_VARIABLE uint_fast32_t ExceptionDirRVA;        // exception directory (.pdata) RVA
EXTERNAL_VARIABLE uint_fast32_t ExceptionDirSize;       // exception directory (.pdata) size
EXTERNAL_VARIABLE int LargeAddressAware;               // executable can handle addresses above 2 GB
EXTERNAL_VARIABLE unsigned int NumAbsolute32Relocations;// number of 32-bit absolute relocations
EXTERNAL_VARIABLE uint_fast32_t DelayImportDirRVA;      // delay load import directory RVA
EXTERNAL_VARIABLE uint_fast32_t DelayImportDirSize;     // delay load import directory size

EXTERNAL_VARIABLE uint_fast32_t EIPObjectNum;           // initial section
EXTERNAL_VARIABLE uint_fast32_t EIP;                    // initial offset

EXTERNAL_VARIABLE ud_t ud_obj;                          // udis86 object

EXTERNAL_VARIABLE pdata_entry *pdata_list;              // sorted list of functions from exception directory (.pdata)
EXTERNAL_VARIABLE unsigned int pdata_count;             // number of entries in pdata_list

EXTERNAL_VARIABLE int list_invalid_code_fixups;
EXTERNAL_VARIABLE char *invalid_code_fixups_name;
EXTERNAL_VARIABLE FILE *invalid_code_fixups_file;

EXTERNAL_VARIABLE int list_data_to_code_fixups;
EXTERNAL_VARIABLE char *data_to_code_fixups_name;

EXTERNAL_VARIABLE int list_rva_displacements;
EXTERNAL_VARIABLE char *rva_displacements_name;
EXTERNAL_VARIABLE FILE *rva_displacements_file;

EXTERNAL_VARIABLE char *jump_tables_name;
EXTERNAL_VARIABLE FILE *jump_tables_file;               // list of jumps through jump tables (--list_jump_tables)
EXTERNAL_VARIABLE char *code_ranges_name;               // list of decoded code (--list_code_ranges)

EXTERNAL_VARIABLE char *generate_relocations_name;

EXTERNAL_VARIABLE int add_tracing_to_code;

// configuration (SR.cfg)
EXTERNAL_VARIABLE int use_pdata;                        // use exception directory to find functions
EXTERNAL_VARIABLE int convert_exception_data;           // convert exception directory and unwind/exception handling data
EXTERNAL_VARIABLE int detect_jump_tables;               // detect jump tables which are not relocated (image relative, label differences)
EXTERNAL_VARIABLE int detect_image_relative;            // detect image relative addressing (register containing image base + RVA)
EXTERNAL_VARIABLE int keep_original_code;               // ELF: original code is kept at original addresses (functions which can't be recompiled)

EXTERNAL_VARIABLE Pvoid_t import_list;

#ifdef __cplusplus
}
#endif

#if defined(USE_JUDY)
	#include "judy_helpers.h"
#else
	#include "container_helpers.h"
#endif

// SR_main.c
int SR_get_section_reladr(uint64_t Address, uint_fast32_t *SecNum, uint_fast32_t *RelAdr);
int SR_get_section_reladr_exact(uint64_t Address, uint_fast32_t *SecNum, uint_fast32_t *RelAdr);
int SR_is_ignored(uint_fast32_t SecNum, uint_fast32_t RelAdr);

// SR_basic.c
void SR_get_label(char *cbuf, uint64_t Address);
void SR_get_label_def(char *cbuf, uint64_t Address);
void SR_format_label_expr(char *cbuf, uint64_t LabelAddress, int64_t displacement);
int SR_get_label_expr(char *cbuf, uint_fast32_t SecNum, int_fast64_t RelAdr, int set_label);
int SR_initial_disassembly(void);
int SR_apply_fixup_info(void);

// SRW64_loader.c
int SRW64_LoadFile(const char *fname);
void *SR_rva_to_ptr(uint_fast32_t rva, uint_fast32_t size);
void SR_add_orig_section(uint_fast32_t virtaddr, uint_fast32_t size, uint8_t *adr);

// SRW64_elf.c
int SRW64_LoadElf(const uint8_t *file, int64_t file_size);
int SR_elf_process(void);
int SR_elf_is_function_start(uint64_t address);
int SR_elf_is_instruction_start(uint64_t address);
int SR_elf_jump_table_valid(uint64_t table, uint64_t jump_address, int bound);
uint64_t SR_elf_find_table_address(uint64_t jump_address, int base_register, int base_register2, int bound);
int SR_elf_absolute_label(char *cbuf, uint64_t address);
void SR_elf_write_externs(FILE *fout);
void SR_elf_write_phdr_notes(FILE *fout);
int SR_write_linker_script(const char *fname);
void SR_elf_output_section_name(char *buf, unsigned int Entry);
void SR_elf_free(void);
int SR_elf_is_noret_name(const char *name);
int SR_elf_add_fixup(uint64_t source, uint64_t target);

// SRW64_elf_dyn.c
int SR_elf_dyn_load(const uint8_t *file, int64_t file_size, const void *ehdr, const void *shdrs, const void *phdrs);
int SR_elf_dyn_process(void);
int SR_elf_dyn_is_tls_got(uint64_t address);
extrn_data *SR_elf_dyn_find_outside_import(uint64_t address);
int SR_elf_dyn_reference_label(char *cbuf, uint64_t address);
int SR_elf_dyn_is_import_address(uint64_t value);
void SR_elf_dyn_write_externs(FILE *fout);
void SR_elf_dyn_write_exports(FILE *fout);
void SR_elf_dyn_write_copy_references(FILE *fout);
int SR_elf_dyn_write_imports(const char *fname);
int SR_elf_dyn_is_dynamic(void);
const char *SR_elf_dyn_interp(void);
void SR_elf_dyn_free(void);

// SRW64_elf_eh.c
int SR_elf_eh_prescan(uint64_t **ranges, unsigned int *num_ranges);
int SR_elf_eh_add_entries(void);
int SR_elf_eh_post_process(void);
int SR_elf_eh_write(FILE *fout);
int SR_elf_eh_present(void);
void SR_elf_eh_free(void);

// SRW64_pdata.c
const pdata_entry *SR_find_pdata(uint_fast32_t rva);
const pdata_entry *SR_find_pdata_begin(uint_fast32_t rva);
int SR_process_pdata(void);
int SR_process_delay_imports(void);
int SR_add_rva_fixup(uint_fast32_t src_rva, int flags);

// SRW64_eh4.c
int SR_fh4_process(uint_fast32_t unwind_rva, uint_fast32_t data_rva, const pdata_entry *function, int handler_kind);
int SR_fh4_post_process(void);
const char *SR_fh4_get_data(void);
void SR_fh4_free(void);
int SR_process_unwind_offsets(void);
int SR_prescan_image_base(void);

// SR_source_code_information.c
int SR_LoadSCI(void);

// SR_full.c
int SR_full_disassembly(void);
output_data *SR_disassemble_offset_init_output(unsigned int SecNum, uint_fast32_t offset, unsigned int length);

// SR_full_helpers.c
void SR_disassemble_add_address(unsigned int Entry, uint64_t address);
int SR_disassemble_convert_cjump(char *dst, const char *modifier, uint64_t address, extrn_data *extrn);
int SR_disassemble_convert_fixup(const char *ostr, char *dst, fixup_data *fixup, extrn_data *extrn, int Entry, uint_fast32_t offset, int decoded_length);
int SR_disassemble_find_noret(uint64_t address);
extrn_data *SR_disassemble_find_proc(unsigned int Entry, uint64_t address);
extrn_data *SR_disassemble_find_import(uint64_t address);

// SR_full_win64.c
int SR_disassemble_offset_win64(unsigned int Entry, uint_fast32_t offset);

// SR_output.c
int SR_write_output(const char *fname);

#undef EXTERNAL_VARIABLE

#endif
