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

// imported functions which don't return
static const char *noret_imports[] = {
    "ExitProcess",
    "ExitThread",
    "FreeLibraryAndExitThread",
    "RaiseFailFastException",
    "RtlExitUserProcess",
    "RtlExitUserThread",
    "exit",
    "_exit",
    "_Exit",
    "quick_exit",
    "abort",
    "_amsg_exit",
    "longjmp",
    "_longjmp",
    "terminate",
    "_invalid_parameter_noinfo_noreturn",
    "_invoke_watson",
    "_CxxThrowException",
    "__std_terminate",
    "__CxxFrameHandler_unwind",
    NULL
};

void SR_disassemble_add_address(unsigned int Entry, uint64_t address)
{
    output_data *output;
    uint_fast32_t SecNum, RelAdr;

    if (address >= section[Entry].start &&
        address <= section[Entry].start + section[Entry].size)
    {
        SecNum = Entry;
        RelAdr = (uint_fast32_t)(address - section[Entry].start);
        section_entry_list_Insert(SecNum, RelAdr);
    }
    else
    {
        if (SR_get_section_reladr(address, &SecNum, &RelAdr))
        {
            section_entry_list_Insert(SecNum, RelAdr);
        }
        else
        {
            fprintf(stderr, "Warning: code address outside of image: 0x%" PRIx64 "\n", address);
            return;
        }
    }

    output = section_output_list_FindEntryEqual(SecNum, RelAdr);
    if (output != NULL)
    {
        output->has_label = 1;
    }
}

int SR_disassemble_convert_cjump(char *dst, const char *modifier, uint64_t address, extrn_data *extrn)
{
    char cbuf[64];
    const char *ostr, *str1;
    intptr_t length1;

    ostr = ud_insn_asm(&ud_obj);

    str1 = strchr(ostr, ' ');
    if (str1 == NULL)
    {
        fprintf(stderr, "Error: error converting cjump: %i\n", 2);
        return 1;
    }

    length1 = str1 - ostr;

    // copy first part of the string
    memcpy(dst, ostr, length1);

    // copy second part of the string
    strcpy(&(dst[length1]), modifier);

    if (extrn != NULL)
    {
        extrn->is_used = 1;

        // print label to string
        sprintf(&(dst[strlen(dst)]), " %s", extrn->proc);
    }
    else
    {
        // print label to string
        SR_get_label(cbuf, address);

        sprintf(&(dst[strlen(dst)]), " %s", cbuf);
    }

    return 0;
}

// converts value of fixup (relocation) inside instruction to label
int SR_disassemble_convert_fixup(const char *ostr, char *dst, fixup_data *fixup, extrn_data *extrn, int Entry, uint_fast32_t offset, int decoded_length)
{
    char cbuf[64];
    char cLabel[256];
    const char *str1, *str2;
    uint64_t value;
    intptr_t length1;

    if (fixup->tsec == SEC_HEADER)
    {
        value = ImageBase + fixup->tofs;
    }
    else
    {
        value = section[fixup->tsec].start + fixup->tofs;
    }

    if (fixup->type == FT_NORMAL)
    {
        value &= UINT64_C(0xffffffff);
    }

    sprintf(cbuf, "0x%" PRIx64, value);

    // find substring (must be exactly one occurence)
    str1 = strstr(ostr, cbuf);
    while (str1 != NULL)
    {
        // check that the substring is not part of longer number
        str2 = str1 + strlen(cbuf);
        if ((*str2 >= '0' && *str2 <= '9') || (*str2 >= 'a' && *str2 <= 'f'))
        {
            str1 = strstr(str2, cbuf);
            continue;
        }
        break;
    }

    if (str1 == NULL)
    {
        fprintf(stderr, "Error: error converting fixup: %i - %s - %s (%i: 0x%" PRIx64 ")\n", 1, ostr, cbuf, Entry, section[Entry].start + offset);

        return 1;
    }
    length1 = str1 - ostr;

    str2 = str1 + strlen(cbuf);

    if ( strstr(str2, cbuf) != NULL )
    {
        fprintf(stderr, "Error: two string occurences in fixup: 0x%" PRIx64 " - %s - %s\n", section[Entry].start + offset, cbuf, ostr);
        return 2;
    }

    // print label to string
    if (extrn != NULL)
    {
        if (extrn->is_import)
        {
            extrn->is_ptr_used = 1;
            sprintf(cLabel, "__imp_%s", extrn->proc);
        }
        else
        {
            extrn->is_used = 1;
            strcpy(cLabel, extrn->proc);
        }
    }
    else
    {
        SR_get_label_expr(cLabel, fixup->tsec, fixup->tofs, 1);
    }

    // copy first part of the string
    memcpy(dst, ostr, length1);
    dst[length1] = 0;

    if (strchr(cLabel, '+') != NULL || strchr(cLabel, '-') != NULL)
    {
        sprintf(&(dst[length1]), "(%s)", cLabel);
    }
    else
    {
        strcpy(&(dst[length1]), cLabel);
    }

    // copy second part of the string
    strcat(dst, str2);

    return 0;
}

int SR_disassemble_find_noret(uint64_t address)
{
    uint_fast32_t SecNum, RelAdr;
    extrn_data *extrn;
    int index;

    if (SR_get_section_reladr(address, &SecNum, &RelAdr))
    {
        if (section_noret_list_CanFindEntryEqual(SecNum, RelAdr))
        {
            return 1;
        }

        extrn = section_extrn_list_FindEntryEqual(SecNum, RelAdr);
        if (extrn != NULL && InputFormat == INPUT_ELF)
        {
            // Linux executables: imported function (GOT entry, PLT stub)
            if (SR_elf_is_noret_name((extrn->proc[0] == '$')?(extrn->proc + 1):extrn->proc)) return 1;
        }
        else if (extrn != NULL && extrn->is_import)
        {
            for (index = 0; noret_imports[index] != NULL; index++)
            {
                if (strcmp(extrn->proc, noret_imports[index]) == 0) return 1;
            }
        }
    }

    return 0;
}

extrn_data *SR_disassemble_find_proc(unsigned int Entry, uint64_t address)
{
    extrn_data *extrn;
    uint_fast32_t SecNum, RelAdr;

    extrn = NULL;

    if (address >= section[Entry].start &&
        address <= section[Entry].start + section[Entry].size)
    {
        extrn = section_extrn_list_FindEntryEqual(Entry, (uint_fast32_t)(address - section[Entry].start));
    }
    else
    {
        if (SR_get_section_reladr_exact(address, &SecNum, &RelAdr))
        {
            extrn = section_extrn_list_FindEntryEqual(SecNum, RelAdr);
        }
    }

    if (extrn != NULL && !extrn->is_import)
    {
        extrn->is_used = 1;
    }

    return extrn;
}

// finds import (IAT slot) at given address
extrn_data *SR_disassemble_find_import(uint64_t address)
{
    extrn_data *extrn;
    uint_fast32_t SecNum, RelAdr;

    if (!SR_get_section_reladr_exact(address, &SecNum, &RelAdr))
    {
        // Linux executables: GOT entry outside of sections (e.g. in dynamic linking information, which isn't part of the output)
        return (InputFormat == INPUT_ELF)?SR_elf_dyn_find_outside_import(address):NULL;
    }
    if (section[SecNum].type != ST_IDATA && InputFormat != INPUT_ELF) return NULL;   // Linux executables: GOT

    extrn = section_extrn_list_FindEntryEqual(SecNum, RelAdr);
    if (extrn != NULL && extrn->is_import) return extrn;

    return NULL;
}
