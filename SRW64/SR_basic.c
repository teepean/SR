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

int SR_initial_disassembly(void)
{
    uint_fast32_t Entry, ofs;
    output_data *output;

    for (Entry = 0; Entry < num_sections; Entry++)
    {
        // debug information is not part of output
        if (section[Entry].type == ST_DEBUG) continue;

        // Linux executables: uninitialized data is one output (written as resb <size>, labels are relative to the start)
        if (InputFormat == INPUT_ELF && section[Entry].type == ST_UDATA && section[Entry].size != 0)
        {
            output = section_output_list_Insert(Entry, 0);
            if (output == NULL) return -1;
            output->type = OT_UNITIALIZED;
            output->len = section[Entry].size - 1;
        }
        else
        for (ofs = 0; ofs < section[Entry].size; ofs++)
        {
            output = section_output_list_Insert(Entry, ofs);
            if ( output == NULL )
            {
                return -1;
            }
            else
            {
                if (section[Entry].type == ST_UDATA)
                {
                    output->type = OT_UNITIALIZED;
                }
            }
        }

        output = section_output_list_Insert(Entry, section[Entry].size);
        if ( output == NULL )
        {
            return -1;
        }
        else
        {
            output->type = OT_NONE;
            output->has_label = 1;
            output->len = (uint_fast32_t) -1;
        }
    }

    return 0;
}

// label definition (name of label in output)
void SR_get_label_def(char *cbuf, uint64_t Address)
{
    sprintf(cbuf, "loc_%" PRIX64, Address);
}

// reference to label
void SR_get_label(char *cbuf, uint64_t Address)
{
    // Linux executables: original code which is not recompiled, addresses outside of sections
    if (InputFormat == INPUT_ELF && SR_elf_absolute_label(cbuf, Address)) return;

    sprintf(cbuf, "loc_%" PRIX64, Address);
}

void SR_format_label_expr(char *cbuf, uint64_t LabelAddress, int64_t displacement)
{
    SR_get_label(cbuf, LabelAddress);

    if (displacement > 0)
    {
        sprintf(&(cbuf[strlen(cbuf)]), "+%" PRIi64, displacement);
    }
    else if (displacement < 0)
    {
        sprintf(&(cbuf[strlen(cbuf)]), "-%" PRIi64, -displacement);
    }
}

// creates label expression (label+displacement) for given address, returns 1 if the address has its own label
int SR_get_label_expr(char *cbuf, uint_fast32_t SecNum, int_fast64_t RelAdr, int set_label)
{
    int *label_value;
    output_data *output;
    uint_fast32_t sec, ofs;
    uint64_t target, label_address;

    if (SecNum == SEC_HEADER)
    {
        if (RelAdr > 0)
        {
            sprintf(cbuf, "__ImageBase+%" PRIi64, (int64_t)RelAdr);
        }
        else if (RelAdr < 0)
        {
            sprintf(cbuf, "__ImageBase-%" PRIi64, -(int64_t)RelAdr);
        }
        else
        {
            strcpy(cbuf, "__ImageBase");
        }
        return 1;
    }

    target = section[SecNum].start + RelAdr;

    // Linux executables: original code which is not recompiled (absolute address)
    if (InputFormat == INPUT_ELF && SR_elf_absolute_label(cbuf, target)) return 1;

    label_value = (RelAdr >= 0)?section_label_list_FindEntryEqual(SecNum, (Word_t)RelAdr):NULL;

    sec = SecNum;
    output = NULL;
    if (label_value != NULL)
    {
        if (SR_get_section_reladr(target + *label_value, &sec, &ofs))
        {
            output = section_output_list_FindEntryEqualOrLower(sec, ofs);
        }
    }
    else
    {
        if (RelAdr < 0)
        {
            output = section_output_list_FindEntryFirst(SecNum);
        }
        else
        {
            output = section_output_list_FindEntryEqualOrLower(SecNum, (Word_t)RelAdr);
        }
    }

    if (output != NULL)
    {
        if (set_label)
        {
            output->has_label = 1;
        }
        label_address = section[sec].start + output->ofs;
    }
    else
    {
        label_address = target;
    }

    SR_format_label_expr(cbuf, label_address, (int64_t)(target - label_address));

    return (label_address == target)?1:0;
}

#if (OUTPUT_TYPE != OUT_ORIG)
static void SR_apply_fixup_alias_init(alias_data *item, void *data)
{
    output_data *output;

#define DATA ((uint_fast32_t)(uintptr_t) data)

    output = section_output_list_FindEntryEqual(DATA, item->ofs);
    if (output != NULL)
    {
        output->has_label = 1;
    }

    if (section[DATA].type == ST_CODE)
    {
        section_entry_list_Insert(DATA, item->ofs);
    }

#undef DATA
}
#endif

static void SR_apply_fixup_export_init(export_data *item, void *data)
{
    output_data *output;

#define DATA ((uint_fast32_t)(uintptr_t) data)

    output = section_output_list_FindEntryEqual(DATA, item->ofs);
    if (output != NULL)
    {
        output->has_label = 1;
    }

    if (section[DATA].type == ST_CODE)
    {
        section_entry_list_Insert(DATA, item->ofs);
    }

#undef DATA
}

static void SR_apply_fixup_data_init(fixup_data *item, void *data)
{
    output_data *output;
    unsigned int index, size;

#define DATA ((uint_fast32_t)(uintptr_t) data)

    size = FIXUP_SIZE(item->type);

    for (index = size - 1; index != 0; index--)
    {
        section_output_list_Delete(DATA, item->sofs + index);
    }

    output = section_output_list_FindEntryEqual(DATA, item->sofs);
    if (output != NULL)
    {
        output->len = size - 1;
    }
#undef DATA
}

static void SR_apply_fixup_data_offset(fixup_data *item, void *data)
{
    char cbuf[256];
    char cbuf3[592];
    output_data *output;
    fixup_data *fixup;
    extrn_data *extrn;
#if (OUTPUT_TYPE != OUT_ORIG)
    replace_data *replace;
#endif
    int validentry, size;

#define DATA ((Entry_FILE *) data)

    size = FIXUP_SIZE(item->type);

#if (OUTPUT_TYPE != OUT_ORIG)
    replace = section_replace_list_FindEntryEqualOrLower(DATA->Entry, item->sofs);
    if ((replace != NULL) && (replace->empty) && (replace->ofs + replace->length >= item->sofs + size)) // source is in empty replaced area
    {
        return;
    }
#endif

    extrn = NULL;

    if (item->tsec == SEC_HEADER)
    {
        SR_get_label_expr(cbuf, SEC_HEADER, item->tofs, 0);
    }
    else
    {
        extrn = section_extrn_list_FindEntryEqual(item->tsec, item->tofs);

        if (extrn != NULL)
        {
            if (extrn->is_import)
            {
                extrn->is_ptr_used = 1;
                sprintf(cbuf, "__imp_%s", extrn->proc);
            }
            else
            {
                extrn->is_used = 1;
                strcpy(cbuf, extrn->proc);
            }
        }
        else
        {
            SR_get_label_expr(cbuf, item->tsec, item->tofs, 1);
        }

        // if target is not external procedure and
        // target section is code section and ...
        if (extrn == NULL &&
            section[item->tsec].type == ST_CODE ) // code section
        {
            // ... and target is not another fixup
            fixup = section_fixup_list_FindEntryEqual(item->tsec, item->tofs);
            if ( fixup == NULL )
            {
                if (item->flags & FF_NOENTRY)
                {
                    validentry = 0;
                }
                else if ( section[DATA->Entry].type == ST_CODE ) // code section
                {
                    // fixups from code section to code section are valid only if originating from jump table,
                    // or from function start (from exception directory), or from list of valid code entries
                    validentry = 0;

                    if (section_code_list_CanFindEntryEqual(item->tsec, item->tofs))
                    {
                        validentry = 1;
                    }

                    if (validentry == 0 && use_pdata && SR_find_pdata_begin(section[item->tsec].virtaddr + item->tofs) != NULL)
                    {
                        validentry = 1;
                    }

                    if (validentry == 0)
                    {
                        fixup = section_fixup_list_FindEntryHigher(DATA->Entry, item->sofs);
                        if (fixup != NULL)
                        {
                            if (fixup->sofs == item->sofs + size && fixup->type == item->type)
                            {
                                validentry = 1;
                            }
                        }
                    }

                    if (validentry == 0 && item->sofs != 0)
                    {
                        fixup = section_fixup_list_FindEntryLower(DATA->Entry, item->sofs);
                        if (fixup != NULL)
                        {
                            if (fixup->sofs + size == item->sofs && fixup->type == item->type)
                            {
                                validentry = 1;
                            }
                        }
                    }

                    if (list_invalid_code_fixups)
                    {
                        if (validentry == 0)
                        {
                            char cbuf2[32];
                            SR_get_label(cbuf2, section[item->tsec].start + item->tofs);
                            fprintf(DATA->fout, "%s\n", cbuf2);
                        }
                    }
                }
                else // data section
                {
                    // each fixup from data section to code section is valid
                    validentry = 1;

                    if (list_data_to_code_fixups)
                    {
                        char cbuf2[32];
                        SR_get_label(cbuf2, section[item->tsec].start + item->tofs);
                        fprintf(DATA->fout, "%s\n", cbuf2);
                    }
                }

                if (validentry)
                {
                    if (section_nocode_list_CanFindEntryEqual(item->tsec, item->tofs))
                    {
                        validentry = 0;
                    }
                }

#if (OUTPUT_TYPE != OUT_ORIG)
                if (validentry)
                {
                    replace = section_replace_list_FindEntryEqualOrLower(item->tsec, item->tofs);
                    if ((replace != NULL) && ((int_fast32_t)(replace->ofs + replace->length) > item->tofs)) // target is in replaced area
                    {
                        replace = section_replace_list_FindEntryEqualOrLower(DATA->Entry, item->sofs);
                        if ((replace != NULL) && (replace->ofs + replace->length > item->sofs)) // source is in replaced area
                        {
                            validentry = 0;
                        }
                    }
                }
#endif

                if (validentry)
                {
                    section_entry_list_Insert(item->tsec, item->tofs);
                }
            }
        }
    }

    output = section_output_list_FindEntryEqual(DATA->Entry, item->sofs);
    if (output != NULL)
    {
        if (output->str != NULL)
        {
            free(output->str);
            output->str = NULL;
        }

        switch (item->type)
        {
            case FT_DIR64:
                sprintf(cbuf3, "dq %s", cbuf);
                break;
            case FT_RVA32:
                sprintf(cbuf3, "dd RVA(%s)", cbuf);
                break;
            default:
                sprintf(cbuf3, "dd %s", cbuf);
                break;
        }

        output->str = strdup(cbuf3);
        output->type = OT_OFFSET;
    }

#undef DATA
}

int SR_apply_fixup_info(void)
{
    Entry_FILE EF;
    int fout_opened;

#if (OUTPUT_TYPE == OUT_ORIG)
    output_data *output;

    output = section_output_list_FindEntryEqual(EIPObjectNum, EIP);
    if (output != NULL)
    {
        output->has_label = 1;
    }

    // insert program entry point into list of entries
    section_entry_list_Insert(EIPObjectNum, EIP);
#endif

    for (EF.Entry = 0; EF.Entry < num_sections; EF.Entry++)
    {
#if (OUTPUT_TYPE != OUT_ORIG)
        section_alias_list_ForEach(EF.Entry, &SR_apply_fixup_alias_init, (void *)(uintptr_t) EF.Entry);
#endif

        section_export_list_ForEach(EF.Entry, &SR_apply_fixup_export_init, (void *)(uintptr_t) EF.Entry);
        section_fixup_list_ForEach(EF.Entry, &SR_apply_fixup_data_init, (void *)(uintptr_t) EF.Entry);
    }

    EF.fout = NULL;
    if (list_invalid_code_fixups)
    {
        // file stays open during full disassembly
        invalid_code_fixups_file = fopen(invalid_code_fixups_name, "wt");
        EF.fout = invalid_code_fixups_file;
        if (EF.fout != NULL)
        {
            fout_opened = 2;
        }
        else
        {
            fout_opened = 0;
            list_invalid_code_fixups = 0;
        }
    }
    else if (list_data_to_code_fixups)
    {
        EF.fout = fopen(data_to_code_fixups_name, "wt");
        if (EF.fout != NULL)
        {
            fout_opened = 1;
        }
        else
        {
            fout_opened = -2;
            list_data_to_code_fixups = 0;
        }
    }
    else
    {
        fout_opened = -1;
    }

    for (EF.Entry = 0; EF.Entry < num_sections; EF.Entry++)
    {
        section_fixup_list_ForEach(EF.Entry, &SR_apply_fixup_data_offset, (void *) &EF);
    }

    if (fout_opened == 0)
    {
        list_invalid_code_fixups = 1;
    }
    else if (fout_opened == -2)
    {
        list_data_to_code_fixups = 1;
    }
    else if (fout_opened == 1)
    {
        fclose(EF.fout);
    }


    return 0;
}
