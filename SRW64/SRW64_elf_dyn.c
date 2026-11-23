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
 * Dynamically linked Linux executables (ELF64, not position independent).
 *
 * The same model as for statically linked executables (sections except code keep their
 * original addresses) and the dynamic linking is done again by the linker:
 *  - imported symbols are external symbols of the generated assembler file,
 *    the recompiled executable is linked with stub libraries (generated from file .imports)
 *    which define the symbols with the same versions as the original libraries,
 *  - calls/jumps through GOT and calls of PLT stubs are calls of the symbols,
 *    other accesses to GOT use GOT of the new executable (wrt ..gotpc, wrt ..gottpoff),
 *    GOT entries of the original executable get addresses of the symbols (for original code),
 *  - references to objects copied into the executable (COPY relocations) are references
 *    to the symbols (the linker creates new COPY relocations),
 *  - data relocations with symbols are written as dq symbol+addend,
 *  - symbols exported by the original executable are exported again.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SR_defs.h"
#include "SR_vars.h"
#include "SRW64_elf.h"

typedef struct _elf_dynsym_ {
    char *name;
    const char *version;    // version name (NULL = unversioned)
    const char *library;    // library which defines the version (NULL = unknown)
    uint64_t value;
    uint64_t size;
    uint8_t type;
    uint8_t bind;
    int defined;            // symbol is defined in executable
    int copy;               // object copied into executable (COPY relocation)
    int imported;           // symbol is imported
} elf_dynsym;

typedef struct _elf_dynrel_ {
    uint64_t address;
    unsigned int sym;
    unsigned int type;
    int64_t addend;
} elf_dynrel;

typedef struct _elf_plt_stub_ {
    uint64_t address;
    unsigned int sym;
} elf_plt_stub;

static char *dyn_strings = NULL;
static uint64_t dyn_strings_size = 0;
static elf_dynsym *dynsyms = NULL;
static unsigned int num_dynsyms = 0;
static elf_dynrel *got_slots = NULL;        // GOT entries (sorted by address)
static unsigned int num_got_slots = 0;
static elf_dynrel *data_relocs = NULL;      // data relocations with symbols (R_X86_64_64)
static unsigned int num_data_relocs = 0;
static elf_dynrel *copies = NULL;           // COPY relocations (sorted by address)
static unsigned int num_copies = 0;
static elf_dynrel *relatives = NULL;        // relative relocations (position independent executable)
static unsigned int num_relatives = 0;
static elf_plt_stub *plt_stubs = NULL;      // PLT stubs (sorted by address)
static unsigned int num_plt_stubs = 0;
static char **needed = NULL;                // DT_NEEDED
static unsigned int num_needed = 0;
static char interp[256];
static char rpath[1024];
static uint64_t dt_init = 0, dt_fini = 0;


static int dynrel_compare(const void *p1, const void *p2)
{
    const elf_dynrel *r1 = (const elf_dynrel *)p1;
    const elf_dynrel *r2 = (const elf_dynrel *)p2;

    return (r1->address < r2->address)?-1:((r1->address > r2->address)?1:0);
}

static int plt_compare(const void *p1, const void *p2)
{
    const elf_plt_stub *s1 = (const elf_plt_stub *)p1;
    const elf_plt_stub *s2 = (const elf_plt_stub *)p2;

    return (s1->address < s2->address)?-1:((s1->address > s2->address)?1:0);
}

static const char *dyn_string(uint64_t offset)
{
    if (dyn_strings == NULL || offset >= dyn_strings_size) return "";
    return dyn_strings + offset;
}

static int add_dynrel(elf_dynrel **list, unsigned int *count, uint64_t address, unsigned int sym, unsigned int type, int64_t addend)
{
    elf_dynrel *new_list;

    if ((*count & 255) == 0)
    {
        new_list = (elf_dynrel *) realloc(*list, (*count + 256) * sizeof(elf_dynrel));
        if (new_list == NULL) return -1;
        *list = new_list;
    }

    (*list)[*count].address = address;
    (*list)[*count].sym = sym;
    (*list)[*count].type = type;
    (*list)[*count].addend = addend;
    (*count)++;
    return 0;
}

// relative relocation from RELR table (implicit addend: value in file at the location)
static int relr_add(const uint8_t *file, int64_t file_size, const ELF64_Ehdr *header, const ELF64_Shdr *sections, uint64_t location)
{
    unsigned int index;

    for (index = 0; index < header->e_shnum; index++)
    {
        uint64_t value;

        if (!(sections[index].sh_flags & SHF_ALLOC) || sections[index].sh_type == SHT_NOBITS) continue;
        if (location < sections[index].sh_addr || location + 8 > sections[index].sh_addr + sections[index].sh_size) continue;
        if ((int64_t)(sections[index].sh_offset + (location - sections[index].sh_addr) + 8) > file_size) return 0;

        memcpy(&value, file + sections[index].sh_offset + (location - sections[index].sh_addr), 8);
        return add_dynrel(&relatives, &num_relatives, location + ElfLoadBias, 0, R_X86_64_RELATIVE, (int64_t)(value + ElfLoadBias));
    }

    return 0;
}

// loads dynamic linking information (called by ELF loader)
int SR_elf_dyn_load(const uint8_t *file, int64_t file_size, const void *ehdr, const void *shdrs)
{
    const ELF64_Ehdr *header = (const ELF64_Ehdr *) ehdr;
    const ELF64_Shdr *sections = (const ELF64_Shdr *) shdrs;
    const ELF64_Shdr *dynsym_sec, *versym_sec, *verneed_sec, *dynamic_sec;
    const char **version_names, **version_files;
    unsigned int index, max_version;

    dynsym_sec = NULL;
    versym_sec = NULL;
    verneed_sec = NULL;
    dynamic_sec = NULL;
    for (index = 0; index < header->e_shnum; index++)
    {
        if ((int64_t)(sections[index].sh_offset + ((sections[index].sh_type == SHT_NOBITS)?0:sections[index].sh_size)) > file_size) continue;

        switch (sections[index].sh_type)
        {
            case SHT_DYNSYM: dynsym_sec = &(sections[index]); break;
            case SHT_GNU_versym: versym_sec = &(sections[index]); break;
            case SHT_GNU_verneed: verneed_sec = &(sections[index]); break;
            case SHT_DYNAMIC: dynamic_sec = &(sections[index]); break;
            default: break;
        }
    }

    if (dynsym_sec == NULL || dynsym_sec->sh_link >= header->e_shnum)
    {
        fprintf(stderr, "Error: dynamic symbol table not found\n");
        return -1;
    }

    // strings
    dyn_strings_size = sections[dynsym_sec->sh_link].sh_size;
    if ((int64_t)(sections[dynsym_sec->sh_link].sh_offset + dyn_strings_size) > file_size) return -1;
    dyn_strings = (char *) malloc(dyn_strings_size + 1);
    if (dyn_strings == NULL) return -1;
    memcpy(dyn_strings, file + sections[dynsym_sec->sh_link].sh_offset, dyn_strings_size);
    dyn_strings[dyn_strings_size] = 0;

    // versions (verneed): version index -> version name, library
    max_version = 0;
    version_names = NULL;
    version_files = NULL;
    if (verneed_sec != NULL)
    {
        uint64_t pos;
        unsigned int count;

        version_names = (const char **) calloc(65536, sizeof(const char *));
        version_files = (const char **) calloc(65536, sizeof(const char *));
        if (version_names == NULL || version_files == NULL) return -1;

        pos = 0;
        for (count = 0; count < 4096 && pos + sizeof(ELF64_Verneed) <= verneed_sec->sh_size; count++)
        {
            const ELF64_Verneed *vn;
            uint64_t aux_pos;
            unsigned int aux;

            vn = (const ELF64_Verneed *) (file + verneed_sec->sh_offset + pos);
            aux_pos = pos + vn->vn_aux;
            for (aux = 0; aux < vn->vn_cnt && aux_pos + sizeof(ELF64_Vernaux) <= verneed_sec->sh_size; aux++)
            {
                const ELF64_Vernaux *vna;

                vna = (const ELF64_Vernaux *) (file + verneed_sec->sh_offset + aux_pos);
                version_names[vna->vna_other & 0x7fff] = dyn_string(vna->vna_name);
                version_files[vna->vna_other & 0x7fff] = dyn_string(vn->vn_file);
                if ((vna->vna_other & 0x7fff) > max_version) max_version = vna->vna_other & 0x7fff;
                if (vna->vna_next == 0) break;
                aux_pos += vna->vna_next;
            }

            if (vn->vn_next == 0) break;
            pos += vn->vn_next;
        }
    }

    // dynamic symbols
    num_dynsyms = (unsigned int)(dynsym_sec->sh_size / sizeof(ELF64_Sym));
    dynsyms = (elf_dynsym *) calloc(num_dynsyms + 1, sizeof(elf_dynsym));
    if (dynsyms == NULL) return -1;

    for (index = 0; index < num_dynsyms; index++)
    {
        const ELF64_Sym *sym;
        uint16_t versym;

        sym = (const ELF64_Sym *) (file + dynsym_sec->sh_offset + index * sizeof(ELF64_Sym));
        dynsyms[index].name = (char *) dyn_string(sym->st_name);
        dynsyms[index].value = sym->st_value + ((sym->st_shndx != SHN_UNDEF && sym->st_shndx < SHN_LORESERVE)?ElfLoadBias:0);
        dynsyms[index].size = sym->st_size;
        dynsyms[index].type = sym->st_info & 0x0f;
        dynsyms[index].bind = sym->st_info >> 4;
        dynsyms[index].defined = (sym->st_shndx != SHN_UNDEF)?1:0;

        if (versym_sec != NULL && index * 2 + 2 <= versym_sec->sh_size && version_names != NULL)
        {
            versym = *(const uint16_t *) (file + versym_sec->sh_offset + index * 2);
            if ((versym & 0x7fff) > 1 && (versym & 0x7fff) <= max_version)
            {
                dynsyms[index].version = version_names[versym & 0x7fff];
                dynsyms[index].library = version_files[versym & 0x7fff];
            }
        }

    }

    if (version_names != NULL) free((void *)version_names);
    if (version_files != NULL) free((void *)version_files);

    // relocations
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
            unsigned int type, sym;
            uint64_t address;
            int64_t addend;
            int ret;

            type = (unsigned int)(relocations[relocation].r_info & 0xffffffff);
            sym = (unsigned int)(relocations[relocation].r_info >> 32);
            address = relocations[relocation].r_offset + ElfLoadBias;
            addend = relocations[relocation].r_addend;

            if (sym >= num_dynsyms) continue;

            ret = 0;
            switch (type)
            {
                case R_X86_64_GLOB_DAT:
                case R_X86_64_JUMP_SLOT:
                case R_X86_64_TPOFF64:
                    if (sym == 0) break;
                    ret = add_dynrel(&got_slots, &num_got_slots, address, sym, type, addend);
                    break;
                case R_X86_64_64:
                    if (sym == 0) break;
                    ret = add_dynrel(&data_relocs, &num_data_relocs, address, sym, type, addend);
                    break;
                case R_X86_64_COPY:
                    dynsyms[sym].copy = 1;
                    ret = add_dynrel(&copies, &num_copies, address, sym, type, addend);
                    break;
                case R_X86_64_RELATIVE:
                    // pointer in position independent executable
                    ret = add_dynrel(&relatives, &num_relatives, address, 0, type, addend + (int64_t)ElfLoadBias);
                    break;
                case R_X86_64_IRELATIVE:
                    fprintf(stderr, "Warning: IRELATIVE relocation is not supported in dynamically linked executable: 0x%" PRIx64 "\n", address);
                    break;
                case R_X86_64_DTPMOD64:
                case R_X86_64_DTPOFF64:
                    fprintf(stderr, "Warning: general dynamic TLS relocation is not supported: 0x%" PRIx64 " (%s)\n", address, dynsyms[sym].name);
                    break;
                default:
                    fprintf(stderr, "Warning: unknown dynamic relocation type %u: 0x%" PRIx64 "\n", type, address);
                    break;
            }
            if (ret) return -1;
        }
    }

    // imported symbols: undefined symbols and objects copied into executable
    // (relocations of symbols defined in executable - e.g. unique symbols - are references to the executable)
    for (index = 1; index < num_dynsyms; index++)
    {
        dynsyms[index].imported = ((!dynsyms[index].defined && dynsyms[index].name[0] != 0) || dynsyms[index].copy)?1:0;
    }

    // relative relocations in compact format (RELR, -z pack-relative-relocs): addends are in the relocated locations
    for (index = 0; index < header->e_shnum; index++)
    {
        const uint64_t *entries;
        uint64_t num_entries, entry, where;

        if (sections[index].sh_type != SHT_RELR) continue;
        if ((int64_t)(sections[index].sh_offset + sections[index].sh_size) > file_size) continue;

        entries = (const uint64_t *) (file + sections[index].sh_offset);
        num_entries = sections[index].sh_size / 8;
        where = 0;
        for (entry = 0; entry < num_entries; entry++)
        {
            uint64_t value, location;
            unsigned int bit;

            value = entries[entry];
            if ((value & 1) == 0)
            {
                // address
                location = value;
                if (relr_add(file, file_size, header, sections, location)) return -1;
                where = location + 8;
            }
            else
            {
                // bitmap of the following 63 words
                for (bit = 0; (value >>= 1) != 0; bit++)
                {
                    if (value & 1)
                    {
                        if (relr_add(file, file_size, header, sections, where + 8 * (uint64_t)bit)) return -1;
                    }
                }
                where += 63 * 8;
            }
        }
    }

    if (num_got_slots) qsort(got_slots, num_got_slots, sizeof(elf_dynrel), dynrel_compare);
    if (num_copies) qsort(copies, num_copies, sizeof(elf_dynrel), dynrel_compare);

    // dynamic section: needed libraries, rpath, init/fini
    interp[0] = 0;
    rpath[0] = 0;
    if (dynamic_sec != NULL)
    {
        uint64_t entry;

        for (entry = 0; (entry + 1) * sizeof(ELF64_Dyn) <= dynamic_sec->sh_size; entry++)
        {
            const ELF64_Dyn *dyn;

            dyn = (const ELF64_Dyn *) (file + dynamic_sec->sh_offset + entry * sizeof(ELF64_Dyn));
            if (dyn->d_tag == DT_NULL) break;

            switch (dyn->d_tag)
            {
                case DT_NEEDED:
                    {
                        char **new_needed;

                        new_needed = (char **) realloc(needed, (num_needed + 1) * sizeof(char *));
                        if (new_needed == NULL) return -1;
                        needed = new_needed;
                        needed[num_needed++] = (char *) dyn_string(dyn->d_val);
                    }
                    break;
                case DT_RPATH:
                case DT_RUNPATH:
                    strncpy(rpath, dyn_string(dyn->d_val), sizeof(rpath) - 1);
                    break;
                case DT_INIT:
                    dt_init = dyn->d_val + ElfLoadBias;
                    break;
                case DT_FINI:
                    dt_fini = dyn->d_val + ElfLoadBias;
                    break;
                default:
                    break;
            }
        }
    }

    // interpreter
    for (index = 0; index < header->e_shnum; index++)
    {
        const ELF64_Shdr *shdr = &(sections[index]);

        if (shdr->sh_name < sections[header->e_shstrndx].sh_size &&
            strcmp((const char *)(file + sections[header->e_shstrndx].sh_offset + shdr->sh_name), ".interp") == 0 &&
            shdr->sh_size < sizeof(interp) && (int64_t)(shdr->sh_offset + shdr->sh_size) <= file_size
           )
        {
            memcpy(interp, file + shdr->sh_offset, shdr->sh_size);
            interp[shdr->sh_size] = 0;
        }
    }
    if (interp[0] == 0) strcpy(interp, "/lib64/ld-linux-x86-64.so.2");

    {
        unsigned int imported, exported;

        imported = 0;
        exported = 0;
        for (index = 1; index < num_dynsyms; index++)
        {
            if (dynsyms[index].imported) imported++;
            else if (dynsyms[index].defined && dynsyms[index].bind != STB_LOCAL) exported++;
        }

        fprintf(stderr, "\tDynamically linked: %u libraries, %u imported symbols (%u GOT entries, %u COPY relocations, %u data relocations), %u exported symbols, %u relative relocations\n", num_needed, imported, num_got_slots, num_copies, num_data_relocs, exported, num_relatives);
    }

    return 0;
}

static const elf_dynrel *find_got_slot(uint64_t address)
{
    unsigned int low, high, middle;

    low = 0;
    high = num_got_slots;
    while (low < high)
    {
        middle = (low + high) / 2;
        if (got_slots[middle].address < address) low = middle + 1;
        else high = middle;
    }

    if (low < num_got_slots && got_slots[low].address == address) return &(got_slots[low]);
    return NULL;
}

static const elf_dynrel *find_copy(uint64_t address)
{
    unsigned int low, high, middle;

    // last copy with address <= address
    low = 0;
    high = num_copies;
    while (low < high)
    {
        middle = (low + high) / 2;
        if (copies[middle].address <= address) low = middle + 1;
        else high = middle;
    }

    if (low == 0) return NULL;
    low--;
    if (address < copies[low].address + ((dynsyms[copies[low].sym].size != 0)?dynsyms[copies[low].sym].size:1)) return &(copies[low]);
    return NULL;
}

static const elf_plt_stub *find_plt_stub(uint64_t address)
{
    unsigned int low, high, middle;

    low = 0;
    high = num_plt_stubs;
    while (low < high)
    {
        middle = (low + high) / 2;
        if (plt_stubs[middle].address < address) low = middle + 1;
        else high = middle;
    }

    if (low < num_plt_stubs && plt_stubs[low].address == address) return &(plt_stubs[low]);
    return NULL;
}

int SR_elf_dyn_is_tls_got(uint64_t address)
{
    const elf_dynrel *slot;

    slot = find_got_slot(address);
    return (slot != NULL && slot->type == R_X86_64_TPOFF64)?1:0;
}

// reference to imported function instead of address of PLT stub, returns 1 if label was written
// (objects copied into executable stay at their original addresses - the COPY relocations are moved there after linking)
int SR_elf_dyn_reference_label(char *cbuf, uint64_t address)
{
    const elf_plt_stub *stub;

    if (num_dynsyms == 0) return 0;

    stub = find_plt_stub(address);
    if (stub != NULL)
    {
        strcpy(cbuf, dynsyms[stub->sym].name);
        return 1;
    }

    return 0;
}

// is value address which is converted to imported symbol (PLT stub) ?
int SR_elf_dyn_is_import_address(uint64_t value)
{
    if (num_dynsyms == 0) return 0;

    return (find_plt_stub(value) != NULL)?1:0;
}

static int set_data_string(uint64_t address, const char *str)
{
    uint_fast32_t SecNum, RelAdr, k;
    output_data *output;

    if (!SR_get_section_reladr_exact(address, &SecNum, &RelAdr)) return 1;
    if (section[SecNum].type == ST_CODE || RelAdr + 8 > section[SecNum].size_in_file) return 1;

    output = section_output_list_FindEntryEqual(SecNum, RelAdr);
    if (output == NULL || output->type != OT_UNKNOWN || output->str != NULL) return 1;

    for (k = 1; k < 8; k++)
    {
        output_data *output2;

        output2 = section_output_list_FindEntryEqual(SecNum, RelAdr + k);
        if (output2 == NULL || output2->type != OT_UNKNOWN || output2->has_label || output2->str != NULL) return 1;
    }

    for (k = 1; k < 8; k++)
    {
        section_output_list_Delete(SecNum, RelAdr + k);
    }

    output->len = 7;
    output->type = OT_OFFSET;
    output->str = strdup(str);
    if (output->str == NULL) return -1;

    return 0;
}

// "dq label" for address of symbol defined in executable (or object copied into executable)
static int elf_own_symbol_string(char *buf, unsigned int sym, int64_t addend)
{
    uint_fast32_t SecNum, RelAdr;
    output_data *output;
    char label[256];

    if (dynsyms[sym].copy)
    {
        unsigned int k;

        for (k = 0; k < num_copies; k++)
        {
            if (copies[k].sym == sym) break;
        }
        if (k >= num_copies) return 1;
        if (!SR_get_section_reladr_exact(copies[k].address, &SecNum, &RelAdr)) return 1;
    }
    else if (!SR_get_section_reladr_exact(dynsyms[sym].value, &SecNum, &RelAdr)) return 1;

    output = section_output_list_FindEntryEqual(SecNum, RelAdr);
    if (output != NULL) output->has_label = 1;

    SR_get_label_expr(label, SecNum, (int_fast64_t)RelAdr + addend, 1);
    sprintf(buf, "dq %s", label);
    return 0;
}

// processing after initial disassembly: PLT stubs, imports in GOT, data relocations, exports
int SR_elf_dyn_process(void)
{
    unsigned int Entry, index, stubs_alloc, written;
    char buf[1024];

    if (num_dynsyms == 0) return 0;

    // PLT stubs: jmp [rip+GOT entry]
    stubs_alloc = 0;
    for (Entry = 0; Entry < num_sections; Entry++)
    {
        ud_t ud_tmp;
        uint64_t address, entsize;
        unsigned int length;

        if (section[Entry].type != ST_CODE) continue;
        if (strcmp(section[Entry].name, ".plt") != 0 && strcmp(section[Entry].name, ".plt.sec") != 0 &&
            strcmp(section[Entry].name, ".plt.got") != 0 && strcmp(section[Entry].name, ".plt.bnd") != 0) continue;

        entsize = (strcmp(section[Entry].name, ".plt.got") == 0)?8:16;

        ud_init(&ud_tmp);
        ud_set_mode(&ud_tmp, 64);
        ud_set_vendor(&ud_tmp, UD_VENDOR_ANY);
        ud_set_input_buffer(&ud_tmp, section[Entry].adr, section[Entry].size_in_file);
        ud_set_pc(&ud_tmp, section[Entry].start);

        address = section[Entry].start;
        while ((length = ud_decode(&ud_tmp)) != 0)
        {
            if (ud_tmp.mnemonic == UD_Ijmp && ud_tmp.operand[0].type == UD_OP_MEM && ud_tmp.operand[0].base == UD_R_RIP)
            {
                const elf_dynrel *slot;

                slot = find_got_slot(address + length + (int64_t)ud_tmp.operand[0].lval.sdword);
                if (slot != NULL && slot->type != R_X86_64_TPOFF64)
                {
                    uint64_t stub_address;

                    // IBT: .plt.got entries are 16 bytes (endbr64; bnd jmp; nop)
                    if (entsize == 8 && address - section[Entry].start >= 4 && section[Entry].adr[address - section[Entry].start - 4] == 0xf3) entsize = 16;

                    stub_address = section[Entry].start + ((address - section[Entry].start) / entsize) * entsize;

                    if (num_plt_stubs >= stubs_alloc)
                    {
                        elf_plt_stub *new_stubs;

                        new_stubs = (elf_plt_stub *) realloc(plt_stubs, (stubs_alloc + 256) * sizeof(elf_plt_stub));
                        if (new_stubs == NULL) return -1;
                        plt_stubs = new_stubs;
                        stubs_alloc += 256;
                    }
                    plt_stubs[num_plt_stubs].address = stub_address;
                    plt_stubs[num_plt_stubs].sym = slot->sym;
                    num_plt_stubs++;
                }
            }
            address += length;
        }
    }
    if (num_plt_stubs) qsort(plt_stubs, num_plt_stubs, sizeof(elf_plt_stub), plt_compare);

    // calls of PLT stubs are calls of the symbols
    for (index = 0; index < num_plt_stubs; index++)
    {
        uint_fast32_t SecNum, RelAdr;

        if (!SR_get_section_reladr_exact(plt_stubs[index].address, &SecNum, &RelAdr)) continue;
        if (section_extrn_list_FindEntryEqual(SecNum, RelAdr) != NULL) continue;
        if (section_extrn_list_Insert(SecNum, RelAdr, dynsyms[plt_stubs[index].sym].name, NULL) == NULL) return -1;
    }

    // GOT entries: imports (accesses from code), addresses of symbols (for original code)
    written = 0;
    for (index = 0; index < num_got_slots; index++)
    {
        uint_fast32_t SecNum, RelAdr;
        extrn_data *extrn;

        if (!SR_get_section_reladr_exact(got_slots[index].address, &SecNum, &RelAdr)) continue;

        // symbol defined in executable or object copied into executable - GOT entry contains its (original) address
        if (!dynsyms[got_slots[index].sym].imported || dynsyms[got_slots[index].sym].copy)
        {
            if (got_slots[index].type != R_X86_64_TPOFF64 &&
                elf_own_symbol_string(buf, got_slots[index].sym, 0) == 0 &&
                set_data_string(got_slots[index].address, buf) == 0) written++;
            continue;
        }

        extrn = section_extrn_list_FindEntryEqual(SecNum, RelAdr);
        if (extrn == NULL)
        {
            extrn = section_extrn_list_Insert(SecNum, RelAdr, dynsyms[got_slots[index].sym].name, NULL);
            if (extrn == NULL) return -1;
            extrn->is_import = 1;
        }

        if (got_slots[index].type != R_X86_64_TPOFF64)
        {
            sprintf(buf, "dq %s", dynsyms[got_slots[index].sym].name);
            if (set_data_string(got_slots[index].address, buf) == 0) written++;
        }
    }

    // data relocations with symbols
    for (index = 0; index < num_data_relocs; index++)
    {
        if (!dynsyms[data_relocs[index].sym].imported || dynsyms[data_relocs[index].sym].copy)
        {
            if (elf_own_symbol_string(buf, data_relocs[index].sym, data_relocs[index].addend) == 0 &&
                set_data_string(data_relocs[index].address, buf) == 0) written++;
            continue;
        }

        if (data_relocs[index].addend > 0) sprintf(buf, "dq %s+%" PRIi64, dynsyms[data_relocs[index].sym].name, data_relocs[index].addend);
        else if (data_relocs[index].addend < 0) sprintf(buf, "dq %s-%" PRIi64, dynsyms[data_relocs[index].sym].name, -data_relocs[index].addend);
        else sprintf(buf, "dq %s", dynsyms[data_relocs[index].sym].name);

        if (set_data_string(data_relocs[index].address, buf) == 0) written++;
    }

    // relative relocations (pointers in position independent executable)
    {
        unsigned int converted;

        converted = 0;
        for (index = 0; index < num_relatives; index++)
        {
            int ret;

            ret = SR_elf_add_fixup(relatives[index].address, (uint64_t)relatives[index].addend);
            if (ret < 0) return -1;
            if (ret > 0) converted++;
        }
        if (num_relatives != 0)
        {
            fprintf(stderr, "\tRelative relocations converted: %u/%u\n", converted, num_relatives);
        }
    }

    // exported symbols need labels
    for (index = 1; index < num_dynsyms; index++)
    {
        uint_fast32_t SecNum, RelAdr;
        output_data *output;

        if (!dynsyms[index].defined || dynsyms[index].imported || dynsyms[index].bind == STB_LOCAL || dynsyms[index].name[0] == 0) continue;
        if (!SR_get_section_reladr_exact(dynsyms[index].value, &SecNum, &RelAdr)) continue;

        output = section_output_list_FindEntryEqual(SecNum, RelAdr);
        if (output != NULL) output->has_label = 1;

        // exported functions are code entries
        if (section[SecNum].type == ST_CODE && dynsyms[index].type == STT_FUNC && !SR_is_ignored(SecNum, RelAdr))
        {
            section_entry_list_Insert(SecNum, RelAdr);
        }
    }

    // DT_INIT, DT_FINI
    {
        uint64_t addresses[2];
        unsigned int k;

        addresses[0] = dt_init;
        addresses[1] = dt_fini;
        for (k = 0; k < 2; k++)
        {
            uint_fast32_t SecNum, RelAdr;
            output_data *output;

            if (addresses[k] == 0 || !SR_get_section_reladr_exact(addresses[k], &SecNum, &RelAdr)) continue;
            output = section_output_list_FindEntryEqual(SecNum, RelAdr);
            if (output != NULL) output->has_label = 1;
            if (section[SecNum].type == ST_CODE && !SR_is_ignored(SecNum, RelAdr)) section_entry_list_Insert(SecNum, RelAdr);
        }
    }

    fprintf(stderr, "\tPLT stubs: %u, GOT entries and data relocations written: %u\n", num_plt_stubs, written);

    return 0;
}

static int is_exported(unsigned int index)
{
    // symbols defined by the linker
    static const char *linker_symbols[] = { "_edata", "edata", "_end", "end", "__bss_start", "_etext", "etext", "__executable_start", "_DYNAMIC", "_GLOBAL_OFFSET_TABLE_", "__GNU_EH_FRAME_HDR", NULL };
    int k;

    if (!dynsyms[index].defined || dynsyms[index].imported || dynsyms[index].bind == STB_LOCAL || dynsyms[index].name[0] == 0) return 0;

    for (k = 0; linker_symbols[k] != NULL; k++)
    {
        if (strcmp(dynsyms[index].name, linker_symbols[k]) == 0) return 0;
    }

    // address of object copied into executable
    if (find_copy(dynsyms[index].value) != NULL) return 0;

    return 1;
}

// label for exported symbol (label definition, not reference - references can be redirected to imported symbols)
static int export_label(char *buf, uint64_t address)
{
    uint_fast32_t SecNum, RelAdr;

    if (!SR_get_section_reladr_exact(address, &SecNum, &RelAdr)) return 1;

    if (section[SecNum].type == ST_CODE && SR_is_ignored(SecNum, RelAdr))
    {
        if (!keep_original_code) return 1;
        sprintf(buf, "srw_orig_%u+0x%x", (unsigned int)SecNum, (unsigned int)RelAdr);
        return 0;
    }

    if (section_output_list_FindEntryEqual(SecNum, RelAdr) == NULL) return 1;

    SR_get_label_def(buf, address);
    return 0;
}

// writes declarations of imported symbols
void SR_elf_dyn_write_externs(FILE *fout)
{
    unsigned int index;

    if (num_dynsyms == 0) return;

    fprintf(fout, "\n; imported symbols (shared libraries)\n");
    for (index = 1; index < num_dynsyms; index++)
    {
        if (!dynsyms[index].imported) continue;
        fprintf(fout, "extern %s%s ; %s %s\n", dynsyms[index].name, (dynsyms[index].bind == STB_WEAK && !dynsyms[index].copy)?":weak":"",
                (dynsyms[index].library != NULL)?dynsyms[index].library:"-", (dynsyms[index].version != NULL)?dynsyms[index].version:"-");
    }
}

// writes references which make the linker create COPY relocations (never executed)
void SR_elf_dyn_write_copy_references(FILE *fout)
{
    unsigned int index;

    if (num_copies == 0) return;

    fprintf(fout, "\n; references which make the linker create COPY relocations (never executed)\n");
    fprintf(fout, "; the relocations are moved to the original addresses after linking (fixcopy_elf.py)\n");
    fprintf(fout, "section srw.copyref progbits alloc exec nowrite align=16\n");
    for (index = 0; index < num_copies; index++)
    {
        fprintf(fout, "lea rax, [rel %s]\n", dynsyms[copies[index].sym].name);
    }
}

// writes exported symbols (at the end of file - labels are defined)
void SR_elf_dyn_write_exports(FILE *fout)
{
    unsigned int index, k;
    char label[256];
    int has_init, has_fini;

    if (num_dynsyms == 0) return;

    fprintf(fout, "\n; exported symbols\n");
    has_init = 0;
    has_fini = 0;
    for (index = 1; index < num_dynsyms; index++)
    {
        if (!is_exported(index)) continue;

        // duplicate names (versions)
        for (k = 1; k < index; k++)
        {
            if (is_exported(k) && strcmp(dynsyms[k].name, dynsyms[index].name) == 0) break;
        }
        if (k < index) continue;

        if (export_label(label, dynsyms[index].value)) continue;

        if (dynsyms[index].type == STT_FUNC || dynsyms[index].type == STT_GNU_IFUNC)
        {
            fprintf(fout, "global %s:function\n", dynsyms[index].name);
        }
        else if (dynsyms[index].type == STT_OBJECT || dynsyms[index].type == STT_TLS)
        {
            fprintf(fout, "global %s:data %u\n", dynsyms[index].name, (unsigned int)dynsyms[index].size);
        }
        else
        {
            fprintf(fout, "global %s\n", dynsyms[index].name);
        }
        fprintf(fout, "%s equ %s\n", dynsyms[index].name, label);

        if (strcmp(dynsyms[index].name, "_init") == 0) has_init = 1;
        if (strcmp(dynsyms[index].name, "_fini") == 0) has_fini = 1;
    }

    // DT_INIT, DT_FINI (the linker uses symbols _init, _fini)
    if (dt_init != 0 && !has_init && export_label(label, dt_init) == 0)
    {
        fprintf(fout, "global _init:function\n_init equ %s\n", label);
    }
    if (dt_fini != 0 && !has_fini && export_label(label, dt_fini) == 0)
    {
        fprintf(fout, "global _fini:function\n_fini equ %s\n", label);
    }
}

// writes information for linking (interpreter, libraries, imported symbols) - file .imports
int SR_elf_dyn_write_imports(const char *fname)
{
    FILE *fout;
    char *name, *dotptr, *slash;
    unsigned int index;

    if (num_dynsyms == 0) return 0;

    name = (char *) malloc(strlen(fname) + 16);
    if (name == NULL) return -1;
    strcpy(name, fname);
    dotptr = strrchr(name, '.');
    slash = strrchr(name, '/');
    if (dotptr != NULL && (slash == NULL || dotptr > slash)) *dotptr = 0;
    strcat(name, ".imports");

    fout = fopen(name, "wt");
    free(name);
    if (fout == NULL) return -1;

    fprintf(fout, "# dynamic linking information generated by SRW64\n");
    fprintf(fout, "# interp <path>; rpath <path>; needed <library>\n");
    fprintf(fout, "# symbol <library or -> <name> <version or -> <FUNC|OBJECT|TLS|NOTYPE> <size> <GLOBAL|WEAK>\n");
    fprintf(fout, "# copy <name> <original address> <size> (COPY relocation, moved to original address after linking)\n");
    fprintf(fout, "interp %s\n", interp);
    if (rpath[0] != 0) fprintf(fout, "rpath %s\n", rpath);
    for (index = 0; index < num_needed; index++)
    {
        fprintf(fout, "needed %s\n", needed[index]);
    }

    for (index = 0; index < num_copies; index++)
    {
        fprintf(fout, "copy %s 0x%" PRIx64 " %u\n", dynsyms[copies[index].sym].name, copies[index].address, (unsigned int)dynsyms[copies[index].sym].size);
    }

    for (index = 1; index < num_dynsyms; index++)
    {
        const char *type;

        if (!dynsyms[index].imported) continue;

        switch (dynsyms[index].type)
        {
            case STT_FUNC:
            case STT_GNU_IFUNC:
                type = "FUNC";
                break;
            case STT_OBJECT:
                type = "OBJECT";
                break;
            case STT_TLS:
                type = "TLS";
                break;
            default:
                type = (dynsyms[index].copy)?"OBJECT":"NOTYPE";
                break;
        }

        fprintf(fout, "symbol %s %s %s %s %u %s\n", (dynsyms[index].library != NULL)?dynsyms[index].library:"-", dynsyms[index].name,
                (dynsyms[index].version != NULL)?dynsyms[index].version:"-", type, (unsigned int)dynsyms[index].size,
                (dynsyms[index].bind == STB_WEAK)?"WEAK":"GLOBAL");
    }

    fclose(fout);
    return 0;
}

int SR_elf_dyn_is_dynamic(void)
{
    return (num_dynsyms != 0)?1:0;
}

const char *SR_elf_dyn_interp(void)
{
    return interp;
}

void SR_elf_dyn_free(void)
{
    if (dyn_strings != NULL) free(dyn_strings);
    if (dynsyms != NULL) free(dynsyms);
    if (got_slots != NULL) free(got_slots);
    if (data_relocs != NULL) free(data_relocs);
    if (copies != NULL) free(copies);
    if (relatives != NULL) free(relatives);
    relatives = NULL;
    num_relatives = 0;
    if (plt_stubs != NULL) free(plt_stubs);
    if (needed != NULL) free(needed);
    dyn_strings = NULL;
    dynsyms = NULL;
    got_slots = NULL;
    data_relocs = NULL;
    copies = NULL;
    plt_stubs = NULL;
    needed = NULL;
    num_dynsyms = 0;
    num_got_slots = 0;
    num_data_relocs = 0;
    num_copies = 0;
    num_plt_stubs = 0;
    num_needed = 0;
}
