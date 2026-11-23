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

// reads next line from file, returns 0 at end of file
static int SR_read_line(FILE *file, char *buf)
{
    size_t length;
    int items;

    while (!feof(file))
    {
        // read enters
        items = fscanf(file, "%8191[\n]", buf);
        // read line
        buf[0] = 0;
        items = fscanf(file, "%8191[^\n]", buf);
        if (items <= 0) continue;
        length = strlen(buf);
        if (length != 0 && buf[length - 1] == '\r')
        {
            length--;
            buf[length] = 0;
        }

        if (length == 0) continue;

        return 1;
    }

    return 0;
}

// parses address in format loc_XXXXXXXX, returns 0 if the address is not in any section
static int SR_parse_address(const char *buf, uint_fast32_t *SecNum, uint_fast32_t *RelAdr)
{
    uint64_t address;

    while (*buf == ' ' || *buf == '\t') buf++;

    if (sscanf(buf, "loc_%" SCNx64, &address) != 1)
    {
        return 0;
    }

    if (!SR_get_section_reladr(address, SecNum, RelAdr))
    {
        fprintf(stderr, "Warning: address outside of image: %s\n", buf);
        return 0;
    }

    return 1;
}

static int SR_LoadSCI_fixup_interpret_as_code(FILE *file)
{
    char buf[8192];
    uint_fast32_t SecNum, RelAdr;

    while (SR_read_line(file, buf))
    {
        if (SR_parse_address(buf, &SecNum, &RelAdr))
        {
            section_code_list_Insert(SecNum, RelAdr);
        }
    }

    return 0;
}

static int SR_LoadSCI_displaced_labels(FILE *file)
{
    char buf[8192];
    char *str1;
    int *label_value;
    uint_fast32_t SecNum, RelAdr;

    while (SR_read_line(file, buf))
    {
        str1 = strchr(buf, ',');
        if (str1 == NULL) continue;
        *str1 = 0;
        str1++;

        if (SR_parse_address(buf, &SecNum, &RelAdr))
        {
            label_value = section_label_list_Insert(SecNum, RelAdr);

            if (label_value == NULL) return 1;

            *label_value = (int)strtol(str1, NULL, 0);
        }
    }

    return 0;
}

static int SR_LoadSCI_noret_procedures(FILE *file)
{
    char buf[8192];
    uint_fast32_t SecNum, RelAdr;

    while (SR_read_line(file, buf))
    {
        if (SR_parse_address(buf, &SecNum, &RelAdr))
        {
            section_noret_list_Insert(SecNum, RelAdr);
        }
    }

    return 0;
}

static int SR_LoadSCI_fixup_do_not_interpret_as_code(FILE *file)
{
    char buf[8192];
    uint_fast32_t SecNum, RelAdr;

    while (SR_read_line(file, buf))
    {
        if (SR_parse_address(buf, &SecNum, &RelAdr))
        {
            section_nocode_list_Insert(SecNum, RelAdr);
        }
    }

    return 0;
}

static int SR_LoadSCI_external_procedures(FILE *file)
{
    char buf[8192];
    char *str1, *str2;
    extrn_data *extrn;
    uint_fast32_t SecNum, RelAdr;

    while (SR_read_line(file, buf))
    {
        str1 = strchr(buf, ',');

        if (str1 == NULL) continue;
        *str1 = 0;
        str1++;

        str2 = strchr(str1, ',');
        if (str2 != NULL)
        {
            *str2 = 0;
            str2++;

            while (*str2 == ' ') str2++;
            if (*str2 == 0) str2 = NULL;
        }

        if (SR_parse_address(buf, &SecNum, &RelAdr))
        {
            extrn = section_extrn_list_Insert(SecNum, RelAdr, str1, str2);
            if (extrn == NULL) return 1;
        }
    }

    return 0;
}

static int SR_LoadSCI_global_aliases(FILE *file)
{
    char buf[8192];
    char *str1;
    alias_data *alias;
    uint_fast32_t SecNum, RelAdr;

    while (SR_read_line(file, buf))
    {
        str1 = strchr(buf, ',');

        if (str1 == NULL) continue;
        *str1 = 0;
        str1++;

        if (SR_parse_address(buf, &SecNum, &RelAdr))
        {
            alias = section_alias_list_Insert(SecNum, RelAdr, str1);
            if (alias == NULL) return 1;
        }
    }

    return 0;
}

static int SR_LoadSCI_ignored_areas(FILE *file)
{
    char buf[8192];
    char *str1;
    int *ignored_area_value;
    uint_fast32_t SecNum, RelAdr;

    while (SR_read_line(file, buf))
    {
        str1 = strchr(buf, ',');
        if (str1 == NULL) continue;
        *str1 = 0;
        str1++;

        if (SR_parse_address(buf, &SecNum, &RelAdr))
        {
            ignored_area_value = section_ignored_list_Insert(SecNum, RelAdr);

            if (ignored_area_value == NULL) return 1;

            *ignored_area_value = (int)strtol(str1, NULL, 0);
        }
    }

    return 0;
}

static int SR_LoadSCI_instruction_replacements(FILE *file)
{
    char buf[8192];
    char *str1, *str2, *str3;
    replace_data *replace;
    uint_fast32_t SecNum, RelAdr;
    int instr_empty;
    unsigned int instr_len;

    while (SR_read_line(file, buf))
    {
        str1 = strchr(buf, ',');

        if (str1 == NULL) continue;
        *str1 = 0;
        str1++;

        str2 = strchr(str1, ',');

        if (str2 == NULL) continue;
        *str2 = 0;
        str2++;

        instr_empty = 0;

        str3 = strchr(str2, '|');
        if (str3 == NULL)
        {
            str3 = str2;
            while (*str3 == ' ') str3++;
            if ((*str3 == ';') || (*str3 == '@'))
            {
                instr_empty = 1;
            }
        }
        else
        {
            while (str3 != NULL)
            {
                *str3 = '\n';
                str3++;

                str3 = strchr(str3, '|');
            }
        }

        instr_len = (unsigned int)strtoul(str1, NULL, 0);

        if (SR_parse_address(buf, &SecNum, &RelAdr))
        {
            replace = section_replace_list_Insert(SecNum, RelAdr, str2, instr_len, instr_empty);
            if (replace == NULL) return 1;
        }
    }

    return 0;
}

// loc_XXXXXXXX,1 - convert displacement of instruction to image relative address
// loc_XXXXXXXX,0 - don't convert displacement of instruction to image relative address
static int SR_LoadSCI_rva_displacements(FILE *file)
{
    char buf[8192];
    char *str1;
    int *rvadisp_value;
    uint_fast32_t SecNum, RelAdr;

    while (SR_read_line(file, buf))
    {
        str1 = strchr(buf, ',');
        if (str1 == NULL) continue;
        *str1 = 0;
        str1++;

        if (SR_parse_address(buf, &SecNum, &RelAdr))
        {
            rvadisp_value = section_rvadisp_list_Insert(SecNum, RelAdr);

            if (rvadisp_value == NULL) return 1;

            *rvadisp_value = (strtol(str1, NULL, 0) != 0)?1:0;
        }
    }

    return 0;
}

// loc_XXXXXXXX,count[,n] - count of 32-bit image relative addresses (n - targets are not code entries)
static int SR_LoadSCI_rva_data(FILE *file)
{
    char buf[8192];
    char *str1, *str2;
    uint_fast32_t SecNum, RelAdr, TargetObject, TargetOffset, count, index;
    uint32_t value;
    fixup_data *fixup;
    int flags;

    while (SR_read_line(file, buf))
    {
        str1 = strchr(buf, ',');
        if (str1 == NULL) continue;
        *str1 = 0;
        str1++;

        flags = 0;
        str2 = strchr(str1, ',');
        if (str2 != NULL)
        {
            *str2 = 0;
            str2++;
            while (*str2 == ' ') str2++;
            if (*str2 == 'n' || *str2 == 'N') flags = FF_NOENTRY;
        }

        count = strtoul(str1, NULL, 0);

        if (!SR_parse_address(buf, &SecNum, &RelAdr)) continue;

        for (index = 0; index < count; index++)
        {
            if (RelAdr + 4 * index + 4 > section[SecNum].size) break;

            memcpy(&value, &(section[SecNum].adr[RelAdr + 4 * index]), 4);
            if (value == 0) continue;

            if (!SR_get_section_reladr(ImageBase + value, &TargetObject, &TargetOffset))
            {
                fprintf(stderr, "Warning: image relative address outside of image: 0x%" PRIx64 "\n", section[SecNum].start + RelAdr + 4 * index);
                continue;
            }

            fixup = section_fixup_list_Insert(SecNum, RelAdr + 4 * index);
            if (fixup == NULL) return 1;

            fixup->tofs = TargetOffset;
            fixup->tsec = TargetObject;
            fixup->type = FT_RVA32;
            fixup->flags = flags;
        }
    }

    return 0;
}

typedef int (*SCI_loader)(FILE *file);

static int SR_LoadSCI_file(const char *fname, SCI_loader loader)
{
    FILE *f;
    int ret;

    f = fopen(fname, "rt");
    if (f == NULL) return 0;

    fprintf(stderr, "\tLoading %s...\n", fname);

    ret = loader(f);

    fclose(f);

    if (ret)
    {
        fprintf(stderr, "Error: error loading %s\n", fname);
    }

    return ret;
}

int SR_LoadSCI(void)
{
    int ret;

    if ((ret = SR_LoadSCI_file("fixup_interpret_as_code.sci", SR_LoadSCI_fixup_interpret_as_code))) return ret;
    if ((ret = SR_LoadSCI_file("displaced_labels.sci", SR_LoadSCI_displaced_labels))) return ret;
    if ((ret = SR_LoadSCI_file("noret_procedures.sci", SR_LoadSCI_noret_procedures))) return ret;
    if ((ret = SR_LoadSCI_file("fixup_do_not_interpret_as_code.sci", SR_LoadSCI_fixup_do_not_interpret_as_code))) return ret;
    if ((ret = SR_LoadSCI_file("external_procedures.sci", SR_LoadSCI_external_procedures))) return ret;
    if ((ret = SR_LoadSCI_file("global_aliases.sci", SR_LoadSCI_global_aliases))) return ret;
    if ((ret = SR_LoadSCI_file("ignored_areas.sci", SR_LoadSCI_ignored_areas))) return ret;
    if ((ret = SR_LoadSCI_file("instruction_replacements.sci", SR_LoadSCI_instruction_replacements))) return ret;
    if ((ret = SR_LoadSCI_file("rva_displacements.sci", SR_LoadSCI_rva_displacements))) return ret;
    if ((ret = SR_LoadSCI_file("rva_data.sci", SR_LoadSCI_rva_data))) return ret;

    return 0;
}
