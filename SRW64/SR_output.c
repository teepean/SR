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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SR_defs.h"
#include "SR_vars.h"

typedef struct _Entry_FILE_ {
    uint_fast32_t Entry;
    FILE *fout;
} Entry_FILE;

typedef struct _Entry_Proc_ {
    uint_fast32_t Entry;
    const char *ProcName;
    int is_used;
    int is_ptr_used;
} Entry_Proc;


#if (OUTPUT_TYPE != OUT_ORIG)
static void SR_write_output_alias_global(alias_data *item, void *data)
{
#define DATA ((Entry_FILE *) data)

    fprintf(DATA->fout, "global %s\n", item->proc);

#undef DATA
}
#endif

static int SR_is_output_section(unsigned int Entry)
{
    return (section[Entry].type == ST_CODE ||
            section[Entry].type == ST_DATA ||
            section[Entry].type == ST_STACK ||
            section[Entry].type == ST_UDATA ||
            section[Entry].type == ST_RDATA)?1:0;
}

// is address the start of another section in output ? (then the label at section end is never used)
static int SR_is_start_of_other_section(unsigned int Entry, uint64_t address)
{
    unsigned int index;

    for (index = 0; index < num_sections; index++)
    {
        if (index == Entry) continue;
        if (section[index].start == address && section[index].size != 0 && SR_is_output_section(index)) return 1;
    }

    return 0;
}

static void SR_write_output_line(output_data *item, void *data)
{
    char cbuf[64];

#define DATA ((Entry_FILE *) data)

    if (item->type == OT_NONE && item->ofs != 0 && SR_is_start_of_other_section(DATA->Entry, section[DATA->Entry].start + item->ofs))
    {
        return;
    }

    if ( (item->str != NULL) || (item->type <= 0) || (item->type == OT_NONE))
    {
        if ( item->align != 0 )
        {
            if (section[DATA->Entry].type == ST_CODE)
            {
                fprintf(DATA->fout, "align %i\n", item->align);
            }
            else if (section[DATA->Entry].type == ST_UDATA)
            {
                fprintf(DATA->fout, "alignb %i\n", item->align);
            }
            else
            {
                fprintf(DATA->fout, "align %i, db 0\n", item->align);
            }
        }

        if ( (item->ofs == 0) || (item->has_label != 0) )
        {
#if (OUTPUT_TYPE != OUT_ORIG)
            alias_data *alias;

            alias = section_alias_list_FindEntryEqual(DATA->Entry, item->ofs);

            if (alias != NULL)
            {
                fprintf(DATA->fout, "%s:\n", alias->proc);
            }
            else
            {
                export_data *export;

                export = section_export_list_FindEntryEqual1(DATA->Entry, item->ofs);
                while ((export != NULL) && (export->name[0] != 0))
                {
                    fprintf(DATA->fout, "%s:\n", (export->internal != NULL)?export->internal:export->name);
                    export = export->next;
                };
            }

            SR_get_label(cbuf, section[DATA->Entry].start + item->ofs);
            if ((alias == NULL) || strcmp(cbuf, alias->proc))
            {
                fprintf(DATA->fout, "%s:\n", cbuf);
            }
#else
            SR_get_label(cbuf, section[DATA->Entry].start + item->ofs);
            fprintf(DATA->fout, "%s:\n", cbuf);
#endif
            if (add_tracing_to_code && item->type == OT_INSTRUCTION)
            {
                fprintf(DATA->fout, "TRACE_LABEL 0x%" PRIx64 "\n", section[DATA->Entry].start + item->ofs);
            }
        }

        if (item->str != NULL)
        {
            fprintf(DATA->fout, "%s\n", item->str);
        }
        else
        {
            if (item->type == OT_UNKNOWN)
            {
                fprintf(DATA->fout, "db %i\n", section[DATA->Entry].adr[item->ofs]);
            }
            else if (item->type != OT_NONE)
            {
                fprintf(DATA->fout, "resb 1\n");
            }
        }
    }

#undef DATA
}

#if (OUTPUT_TYPE != OUT_ORIG)
static void SR_write_output_export_def(export_data *item, void *data)
{
    char cbuf[64];
    alias_data *alias;

#define DATA ((Entry_FILE *) data)

    alias = section_alias_list_FindEntryEqual(DATA->Entry, item->ofs);

    if (alias != NULL)
    {
        if (item->name[0] != 0)
        {
            fprintf(DATA->fout, "%s = %s", item->name, alias->proc);
        }
        else
        {
            fprintf(DATA->fout, "%s", alias->proc);
        }
    }
    else if (item->name[0] == 0)
    {
        SR_get_label(cbuf, section[DATA->Entry].start + item->ofs);
        fprintf(DATA->fout, "%s", cbuf);
    }
    else if (item->internal != NULL)
    {
        fprintf(DATA->fout, "%s = %s", item->name, item->internal);
    }
    else
    {
        fprintf(DATA->fout, "%s", item->name);
    }

    if (item->ordinal != 0)
    {
        fprintf(DATA->fout, " @%i", (unsigned int)item->ordinal);
    }

    if (item->name[0] == 0)
    {
        fprintf(DATA->fout, " NONAME");
    }

    if (section[DATA->Entry].type != ST_CODE)
    {
        fprintf(DATA->fout, " DATA");
    }

    fprintf(DATA->fout, "\n");

#undef DATA
}
#endif

#if (OUTPUT_TYPE != OUT_ORIG)
static void SR_write_import_is_used(extrn_data *extrn, void *data)
{
#define DATA ((Entry_Proc *) data)

    if (!extrn->is_import) return;
    if (strcmp(extrn->proc, DATA->ProcName) != 0) return;

    if (extrn->is_used) DATA->is_used = 1;
    if (extrn->is_ptr_used) DATA->is_ptr_used = 1;

#undef DATA
}
#endif

static void SR_write_output_import(import_data *item, void *data)
{
#if (OUTPUT_TYPE != OUT_ORIG)
    Entry_Proc EP;
#endif

#define DATA ((Entry_FILE *) data)

#if (OUTPUT_TYPE != OUT_ORIG)
    // is import used ?
    EP.ProcName = item->ProcName;
    EP.is_used = 0;
    EP.is_ptr_used = 0;
    for (EP.Entry = 0; EP.Entry < num_sections; EP.Entry++)
    {
        section_extrn_list_ForEach(EP.Entry, &SR_write_import_is_used, (void *)&EP);
    }

    if (EP.is_used)
    {
        fprintf(DATA->fout, "extern %s ; %s", item->ProcName, item->DllName);
        if (item->Ordinal != 0) fprintf(DATA->fout, " ordinal %u", (unsigned int)item->Ordinal);
        fprintf(DATA->fout, "\n");
    }
    if (EP.is_ptr_used)
    {
        // pointer to import: provided by import library (win64) or defined at the end of this file (elf64)
        fprintf(DATA->fout, "%%ifidn __OUTPUT_FORMAT__, elf64\n");
        if (!EP.is_used)
        {
            fprintf(DATA->fout, "extern %s ; %s\n", item->ProcName, item->DllName);
        }
        fprintf(DATA->fout, "%%else\n");
        fprintf(DATA->fout, "extern __imp_%s ; %s", item->ProcName, item->DllName);
        if (item->Ordinal != 0) fprintf(DATA->fout, " ordinal %u", (unsigned int)item->Ordinal);
        fprintf(DATA->fout, "\n");
        fprintf(DATA->fout, "%%endif\n");
    }
#else
    fprintf(DATA->fout, "; import %s %s\n", item->ProcName, item->DllName);
#endif

#undef DATA
}

#if (OUTPUT_TYPE != OUT_ORIG)
// definitions of pointers to imports (__imp_Name) for elf64 output
static void SR_write_output_import_pointer(import_data *item, void *data)
{
    Entry_Proc EP;

#define DATA ((Entry_FILE *) data)

    EP.ProcName = item->ProcName;
    EP.is_used = 0;
    EP.is_ptr_used = 0;
    for (EP.Entry = 0; EP.Entry < num_sections; EP.Entry++)
    {
        section_extrn_list_ForEach(EP.Entry, &SR_write_import_is_used, (void *)&EP);
    }

    if (EP.is_ptr_used)
    {
        fprintf(DATA->fout, "__imp_%s: dq %s\n", item->ProcName, item->ProcName);
    }

#undef DATA
}
#endif

static void SR_write_output_export(export_data *item, void *data)
{
    char cbuf[64];

#define DATA ((Entry_FILE *) data)

#if (OUTPUT_TYPE != OUT_ORIG)
    if (item->name[0] != 0)
    {
        alias_data *alias;

        alias = section_alias_list_FindEntryEqual(DATA->Entry, item->ofs);

        if (alias == NULL)
        {
            fprintf(DATA->fout, "global %s\n", (item->internal != NULL)?item->internal:item->name);
        }
    }
    else
#endif
    {
        SR_get_label(cbuf, section[DATA->Entry].start + item->ofs);

        fprintf(DATA->fout, "global %s\n", cbuf);
    }

#undef DATA
}

// section names which have special meaning for linkers are renamed, invalid characters are replaced
static void SR_output_section_name(char *buf, unsigned int Entry)
{
    static const char *special_names[] = {
        ".eh_frame", ".init", ".fini", ".ctors", ".dtors", ".init_array", ".fini_array", ".preinit_array",
        ".tdata", ".tbss", ".got", ".plt", ".dynamic", ".interp", ".comment",
        ".idata", ".edata", ".rsrc", ".reloc", ".drectve",
        NULL
    };
    const char *src;
    char *dst;
    int index;

    src = section[Entry].name;

    if (src[0] == 0)
    {
        sprintf(buf, "seg%.2u", Entry + 1);
        return;
    }

    dst = buf;
    for (index = 0; special_names[index] != NULL; index++)
    {
        if (strcmp(src, special_names[index]) == 0) break;
    }
    if (special_names[index] != NULL || strncmp(src, ".note", 5) == 0 || strncmp(src, ".debug", 6) == 0)
    {
        dst += sprintf(dst, "srw");
    }

    for (; *src; src++)
    {
        *dst = ((*src >= 'a' && *src <= 'z') || (*src >= 'A' && *src <= 'Z') || (*src >= '0' && *src <= '9') || *src == '.' || *src == '_')?*src:'_';
        dst++;
    }
    *dst = 0;
}

/* resources (.rsrc section) are written as .res file which can be compiled into the recompiled executable */

typedef struct _res_writer_ {
    FILE *fout;
    unsigned int Entry;
    int count;
} res_writer;

static void SR_write_res_align(FILE *fout)
{
    long pos;

    pos = ftell(fout);
    while (pos & 3)
    {
        fputc(0, fout);
        pos++;
    }
}

static int SR_res_name_valid(unsigned int Entry, uint_fast32_t name_ofs)
{
    if (name_ofs + 2 > section[Entry].size) return 0;
    return (name_ofs + 2 + 2 * (uint_fast32_t)(*(uint16_t *)&(section[Entry].adr[name_ofs])) <= section[Entry].size)?1:0;
}

// writes type or name of resource (ordinal or string) to resource header buffer
static unsigned int SR_res_put_id(uint8_t *buf, unsigned int pos, unsigned int Entry, uint32_t name)
{
    if (name & 0x80000000)
    {
        uint_fast32_t name_ofs, length, index;

        name_ofs = name & 0x7fffffff;
        if (!SR_res_name_valid(Entry, name_ofs))
        {
            buf[pos] = 0; buf[pos + 1] = 0;
            return pos + 2;
        }

        length = *(uint16_t *)&(section[Entry].adr[name_ofs]);
        for (index = 0; index < 2 * length; index++)
        {
            buf[pos++] = section[Entry].adr[name_ofs + 2 + index];
        }
        buf[pos++] = 0;
        buf[pos++] = 0;
    }
    else
    {
        buf[pos++] = 0xff;
        buf[pos++] = 0xff;
        buf[pos++] = name & 0xff;
        buf[pos++] = (name >> 8) & 0xff;
    }

    return pos;
}

static void SR_write_res_entry(res_writer *rw, uint32_t type, uint32_t name, uint32_t language, uint_fast32_t data_entry_ofs)
{
    uint8_t header[1200];
    unsigned int pos;
    uint32_t data_rva, data_size, value;
    uint_fast32_t data_ofs;
    unsigned int Entry;

    Entry = rw->Entry;
    if (data_entry_ofs + 16 > section[Entry].size) return;

    data_rva = *(uint32_t *)&(section[Entry].adr[data_entry_ofs]);
    data_size = *(uint32_t *)&(section[Entry].adr[data_entry_ofs + 4]);

    if (data_rva < section[Entry].virtaddr || (uint64_t)data_rva + data_size > (uint64_t)section[Entry].virtaddr + section[Entry].size) return;
    data_ofs = data_rva - section[Entry].virtaddr;

    // RESOURCEHEADER: DataSize, HeaderSize, TYPE, NAME, DataVersion, MemoryFlags, LanguageId, Version, Characteristics
    pos = 8;
    pos = SR_res_put_id(header, pos, Entry, type);
    pos = SR_res_put_id(header, pos, Entry, name);
    while (pos & 3) header[pos++] = 0;
    memset(&(header[pos]), 0, 16);
    header[pos + 4] = 0x30; header[pos + 5] = 0x10; // MemoryFlags: MOVEABLE | PURE | DISCARDABLE
    header[pos + 6] = language & 0xff; header[pos + 7] = (language >> 8) & 0xff;
    pos += 16;

    value = data_size;
    memcpy(&(header[0]), &value, 4);
    value = pos;
    memcpy(&(header[4]), &value, 4);

    fwrite(header, 1, pos, rw->fout);
    fwrite(&(section[Entry].adr[data_ofs]), 1, data_size, rw->fout);
    SR_write_res_align(rw->fout);

    rw->count++;
}

static uint_fast32_t SR_res_directory_entries(unsigned int Entry, uint_fast32_t dir_ofs)
{
    if (dir_ofs + 16 > section[Entry].size) return 0;
    return (uint_fast32_t)(*(uint16_t *)&(section[Entry].adr[dir_ofs + 12])) + *(uint16_t *)&(section[Entry].adr[dir_ofs + 14]);
}

static int SR_write_resources(const char *fname, unsigned int Entry)
{
    static const uint8_t empty_header[32] = { 0, 0, 0, 0, 0x20, 0, 0, 0, 0xff, 0xff, 0, 0, 0xff, 0xff, 0, 0 };
    res_writer rw;
    uint_fast32_t num1, num2, num3, i1, i2, i3, e1, e2, e3;
    uint32_t type, name, language, ofs1, ofs2, ofs3;

    rw.fout = fopen(fname, "wb");
    if (rw.fout == NULL) return -1;
    rw.Entry = Entry;
    rw.count = 0;

    fwrite(empty_header, 1, 32, rw.fout);

    // resource directory tree: type -> name -> language -> data entry
    num1 = SR_res_directory_entries(Entry, 0);
    for (i1 = 0; i1 < num1; i1++)
    {
        e1 = 16 + 8 * i1;
        if (e1 + 8 > section[Entry].size) break;
        type = *(uint32_t *)&(section[Entry].adr[e1]);
        ofs1 = *(uint32_t *)&(section[Entry].adr[e1 + 4]);
        if (!(ofs1 & 0x80000000)) continue;
        ofs1 &= 0x7fffffff;

        num2 = SR_res_directory_entries(Entry, ofs1);
        for (i2 = 0; i2 < num2; i2++)
        {
            e2 = ofs1 + 16 + 8 * i2;
            if (e2 + 8 > section[Entry].size) break;
            name = *(uint32_t *)&(section[Entry].adr[e2]);
            ofs2 = *(uint32_t *)&(section[Entry].adr[e2 + 4]);
            if (!(ofs2 & 0x80000000)) continue;
            ofs2 &= 0x7fffffff;

            num3 = SR_res_directory_entries(Entry, ofs2);
            for (i3 = 0; i3 < num3; i3++)
            {
                e3 = ofs2 + 16 + 8 * i3;
                if (e3 + 8 > section[Entry].size) break;
                language = *(uint32_t *)&(section[Entry].adr[e3]);
                ofs3 = *(uint32_t *)&(section[Entry].adr[e3 + 4]);
                if (ofs3 & 0x80000000) continue;

                SR_write_res_entry(&rw, type, name, language & 0xffff, ofs3);
            }
        }
    }

    fclose(rw.fout);

    fprintf(stderr, "\tResources written to .res file: %i\n", rw.count);

    return 0;
}

static int SR_write_section_header(FILE *fout, unsigned int Entry, int first)
{
    char name[96];
    unsigned int align;

    SR_output_section_name(name, Entry);

    // data sections are page aligned and their contents keep the original offset in page,
    // so all alignments (up to page size) are preserved - also in parts of split sections
    align = (section[Entry].type == ST_CODE)?((section[Entry].align < 16)?16:(unsigned int)section[Entry].align):4096;

    if (!first)
    {
        // section with the same name was already declared
        fprintf(fout, "\nsection %s\n", name);
        if (section[Entry].type == ST_CODE)
        {
            fprintf(fout, "align %u\n", align);
        }
    }
    else
    {
        switch (section[Entry].type)
        {
            case ST_CODE:
                fprintf(fout, "\n%%ifidn __OUTPUT_FORMAT__, elf64\n");
                fprintf(fout, "section %s progbits alloc exec nowrite align=%u\n", name, align);
                fprintf(fout, "%%else\n");
                fprintf(fout, "section %s code align=%u\n", name, align);
                fprintf(fout, "%%endif\n");
                break;
            case ST_DATA:
            case ST_STACK:
                fprintf(fout, "\n%%ifidn __OUTPUT_FORMAT__, elf64\n");
                fprintf(fout, "section %s progbits alloc noexec write align=%u\n", name, align);
                fprintf(fout, "%%else\n");
                fprintf(fout, "section %s data align=%u\n", name, align);
                fprintf(fout, "%%endif\n");
                break;
            case ST_UDATA:
                fprintf(fout, "\n%%ifidn __OUTPUT_FORMAT__, elf64\n");
                fprintf(fout, "section %s nobits alloc noexec write align=%u\n", name, align);
                fprintf(fout, "%%else\n");
                fprintf(fout, "section %s bss align=%u\n", name, align);
                fprintf(fout, "%%endif\n");
                break;
            case ST_RDATA:
                fprintf(fout, "\n%%ifidn __OUTPUT_FORMAT__, elf64\n");
                fprintf(fout, "section %s progbits alloc noexec nowrite align=%u\n", name, align);
                fprintf(fout, "%%else\n");
                fprintf(fout, "section %s rdata align=%u\n", name, align);
                fprintf(fout, "%%endif\n");
                break;
            default:
                return 1;
        }
    }

    if (section[Entry].type != ST_CODE)
    {
        // keep original offset in page
        if (section[Entry].type == ST_UDATA)
        {
            fprintf(fout, "resb ((0x%x - ($ - $$)) & 0xfff)\n", (unsigned int)(section[Entry].start & 0xfff));
        }
        else
        {
            fprintf(fout, "times ((0x%x - ($ - $$)) & 0xfff) db 0\n", (unsigned int)(section[Entry].start & 0xfff));
        }
    }

    return 0;
}

int SR_write_output(const char *fname)
{
    Entry_FILE EF;
    unsigned int index;
    int first;
    char name1[96], name2[96];

#if (OUTPUT_TYPE != OUT_ORIG)
    // write module definition file with exports
    for (EF.Entry = 0; EF.Entry < num_sections; EF.Entry++)
    {
        if (section[EF.Entry].export_list != NULL) break;
    }
    if (EF.Entry < num_sections)
    {
        char *fdefname, *dotptr;

        fdefname = (char *) malloc(strlen(fname) + 1 + 4);
        if (fdefname == NULL) return -1;

        strcpy(fdefname, fname);

        dotptr = strrchr(fdefname, '.');
        if (dotptr != NULL)
        {
            strcpy(dotptr, ".def");
        }
        else
        {
            strcat(fdefname, ".def");
        }

        EF.fout = fopen(fdefname, "wt");
        free(fdefname);

        if ( EF.fout == NULL ) return -1;

        fprintf(EF.fout, "EXPORTS\n");

        for (EF.Entry = 0; EF.Entry < num_sections; EF.Entry++)
        {
            if (section[EF.Entry].export_list != NULL)
            {
                section_export_list_ForEach(EF.Entry, &SR_write_output_export_def, (void *) &EF);
            }
        }

        fclose(EF.fout);
    }
#endif


    EF.fout = fopen(fname, "wt");

    if ( EF.fout == NULL ) return -1;

    fprintf(EF.fout, "%s\n", "%include \"srw64inc.inc\"");
    fprintf(EF.fout, "%s\n", "%include \"misc.inc\"");
#if (OUTPUT_TYPE != OUT_ORIG)
    fprintf(EF.fout, "%s\n", "%include \"extern.inc\"");
#endif

#if (OUTPUT_TYPE == OUT_ORIG)
    {
        char cbuf[64];

        SR_get_label(cbuf, section[EIPObjectNum].start + EIP);
        fprintf(EF.fout, "global %s\n", cbuf);
    }
#else
    for (EF.Entry = 0; EF.Entry < num_sections; EF.Entry++)
    {
        section_alias_list_ForEach(EF.Entry, &SR_write_output_alias_global, (void *) &EF);
    }
#endif

    fprintf(EF.fout, "\n");
    import_list_ForEach(&SR_write_output_import, (void *) &EF);

    fprintf(EF.fout, "\n%%ifidn __OUTPUT_FORMAT__, elf64\n");
    fprintf(EF.fout, "section .note.GNU-stack noalloc noexec nowrite progbits\n");
    fprintf(EF.fout, "%%endif\n");

    for (EF.Entry = 0; EF.Entry < num_sections; EF.Entry++)
    {
        if (section[EF.Entry].type == ST_RSRC)
        {
            FILE *fres;
            char *resname, *dotptr;

            resname = (char *) malloc(strlen(fname) + 9);
            if (resname == NULL) return -1;

            strcpy(resname, fname);

            dotptr = strrchr(resname, '.');
            if (dotptr != NULL) *dotptr = 0;
            strcat(resname, ".resdump");

            fres = fopen(resname, "wb");
            free(resname);

            if ( fres == NULL ) return -1;

            fwrite(section[EF.Entry].adr, 1, section[EF.Entry].size, fres);

            fclose(fres);

            // resources in .res format
            resname = (char *) malloc(strlen(fname) + 9);
            if (resname == NULL) return -1;

            strcpy(resname, fname);

            dotptr = strrchr(resname, '.');
            if (dotptr != NULL) *dotptr = 0;
            strcat(resname, ".res");

            if (SR_write_resources(resname, EF.Entry))
            {
                free(resname);
                return -1;
            }

            free(resname);

            continue;
        }

        if (!SR_is_output_section(EF.Entry))
        {
            continue;
        }

        // first section with this name ?
        first = 1;
        SR_output_section_name(name1, EF.Entry);
        for (index = 0; index < EF.Entry; index++)
        {
            if (section[index].type == ST_CODE ||
                section[index].type == ST_DATA ||
                section[index].type == ST_STACK ||
                section[index].type == ST_UDATA ||
                section[index].type == ST_RDATA
               )
            {
                SR_output_section_name(name2, index);
                if (strcmp(name1, name2) == 0)
                {
                    first = 0;
                    break;
                }
            }
        }

        SR_write_section_header(EF.fout, EF.Entry, first);

        section_export_list_ForEach(EF.Entry, &SR_write_output_export, (void *) &EF);

        {
            FILE *fmain;
            char incname[16];

            sprintf((char *) &(incname[0]), "seg%.2i.inc", (unsigned int)(EF.Entry + 1));
            fprintf(EF.fout, "%%include \"%s\"\n", (char *) &(incname[0]));

            fmain = EF.fout;

            EF.fout = fopen((char *) &(incname[0]), "wt");

            if ( EF.fout == NULL ) return -1;

            section_output_list_ForEach(EF.Entry, &SR_write_output_line, (void *) &EF);

            fclose(EF.fout);

            EF.fout = fmain;
        }
    }

#if (OUTPUT_TYPE != OUT_ORIG)
    // pointers to imports (on Windows they are provided by import libraries)
    fprintf(EF.fout, "\n%%ifidn __OUTPUT_FORMAT__, elf64\n");
    fprintf(EF.fout, "section .data\n");
    fprintf(EF.fout, "align 8, db 0\n");
    import_list_ForEach(&SR_write_output_import_pointer, (void *) &EF);
    fprintf(EF.fout, "%%endif\n");
#endif

    fclose(EF.fout);

    return 0;
}
