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
 * Exception handling data of Linux executables (.eh_frame, .gcc_except_table).
 *
 * The original .eh_frame stays at its original address (the program registers it,
 * it describes the original code). A new .eh_frame is generated for the whole program:
 *  - FDEs of recompiled code: addresses are labels, code offsets in call frame
 *    instructions are label differences (DW_CFA_advance_loc4),
 *  - FDEs of original code which is kept (not recompiled): unchanged instructions.
 * LSDAs (C++ exception tables) of recompiled functions are generated again
 * (call-site table with label differences in udata4 format).
 * The program must be linked with --eh-frame-hdr, so the unwinder finds the new FDEs
 * using PT_GNU_EH_FRAME (after the registered original FDEs).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "SR_defs.h"
#include "SR_vars.h"

#define DW_EH_PE_absptr   0x00
#define DW_EH_PE_uleb128  0x01
#define DW_EH_PE_udata2   0x02
#define DW_EH_PE_udata4   0x03
#define DW_EH_PE_udata8   0x04
#define DW_EH_PE_sleb128  0x09
#define DW_EH_PE_sdata2   0x0a
#define DW_EH_PE_sdata4   0x0b
#define DW_EH_PE_sdata8   0x0c
#define DW_EH_PE_pcrel    0x10
#define DW_EH_PE_indirect 0x80
#define DW_EH_PE_omit     0xff

#define MAX_CIES 256

typedef struct _eh_buffer_ {
    char *data;
    size_t len, alloc;
} eh_buffer;

typedef struct _eh_reader_ {
    const uint8_t *data;    // data of section
    uint64_t start;         // address of section
    uint64_t pos;           // current offset
    uint64_t end;           // end offset of readable data
    int error;
} eh_reader;

typedef struct _eh_cie_ {
    uint64_t address;       // address of CIE (length field)
    uint64_t record_end;    // end of CIE (offset in section)
    uint64_t caf;           // code alignment factor
    uint8_t fde_enc;        // encoding of addresses in FDE ('R')
    uint8_t lsda_enc;       // encoding of LSDA pointer ('L')
    uint8_t pers_enc;       // encoding of personality routine pointer ('P')
    int has_z;              // augmentation data present
    int has_lsda;           // 'L'
    int has_pers;           // 'P'
    uint64_t pers_field;    // offset of personality routine pointer in section
    uint64_t pers_raw;      // raw value of personality routine pointer
    uint64_t pers_target;   // address of personality routine (or pointer to it)
    int index;              // index in output (-1 = not written)
    int supported;          // CIE is supported
} eh_cie;

typedef struct _eh_fde_ {
    uint64_t pos;           // offset of FDE in section
    uint64_t record_end;    // end of FDE (offset in section)
    uint64_t id_pos;        // offset of CIE pointer
    eh_cie *cie;
    uint64_t pc_begin;      // address of code
    uint64_t pc_range;      // size of code
    uint64_t aug_len;       // length of augmentation data
    uint64_t lsda_raw;      // raw value of LSDA pointer
    uint64_t lsda;          // address of LSDA
    uint64_t instr_pos;     // offset of call frame instructions
} eh_fde;

static eh_buffer eh_frame_text, eh_except_text;
static eh_cie eh_cies[MAX_CIES];
static unsigned int eh_num_cies;
static uint64_t *eh_landing_pads = NULL;   // landing pads of recompiled functions (code entries)
static unsigned int eh_num_landing_pads = 0, eh_alloc_landing_pads = 0;


static int eh_printf(eh_buffer *buf, const char *format, ...)
{
    va_list ap;
    int len;

    for (;;)
    {
        if (buf->alloc - buf->len < 256)
        {
            char *new_data;

            new_data = (char *) realloc(buf->data, buf->alloc + 65536);
            if (new_data == NULL) return -1;
            buf->data = new_data;
            buf->alloc += 65536;
        }

        va_start(ap, format);
        len = vsnprintf(buf->data + buf->len, buf->alloc - buf->len, format, ap);
        va_end(ap);

        if (len < 0) return -1;
        if ((size_t)len < buf->alloc - buf->len)
        {
            buf->len += len;
            return 0;
        }

        // not enough space
        {
            char *new_data;

            new_data = (char *) realloc(buf->data, buf->alloc + len + 65536);
            if (new_data == NULL) return -1;
            buf->data = new_data;
            buf->alloc += len + 65536;
        }
    }
}

static int eh_bytes(eh_buffer *buf, const uint8_t *bytes, uint64_t count)
{
    uint64_t index;

    for (index = 0; index < count; index++)
    {
        if (eh_printf(buf, ((index & 15) == 0)?"db 0x%02x":", 0x%02x", bytes[index])) return -1;
        if ((index & 15) == 15 || index + 1 == count)
        {
            if (eh_printf(buf, "\n")) return -1;
        }
    }

    return 0;
}

static int eh_uleb(eh_buffer *buf, uint64_t value)
{
    uint8_t bytes[16];
    unsigned int count;

    count = 0;
    do
    {
        bytes[count] = value & 0x7f;
        value >>= 7;
        if (value != 0) bytes[count] |= 0x80;
        count++;
    } while (value != 0);

    return eh_bytes(buf, bytes, count);
}

static unsigned int eh_uleb_size(uint64_t value)
{
    unsigned int count;

    count = 0;
    do
    {
        value >>= 7;
        count++;
    } while (value != 0);

    return count;
}


static int eh_reader_init(eh_reader *r, uint64_t address)
{
    uint_fast32_t SecNum, RelAdr;

    if (!SR_get_section_reladr_exact(address, &SecNum, &RelAdr)) return 1;

    r->data = section[SecNum].adr;
    r->start = section[SecNum].start;
    r->pos = RelAdr;
    r->end = section[SecNum].size_in_file;
    r->error = (r->pos > r->end)?1:0;

    return r->error;
}

static uint64_t rd_fixed(eh_reader *r, unsigned int size)
{
    uint64_t value;
    unsigned int index;

    if (r->error || r->pos + size > r->end)
    {
        r->error = 1;
        return 0;
    }

    value = 0;
    for (index = 0; index < size; index++)
    {
        value |= ((uint64_t)r->data[r->pos + index]) << (8 * index);
    }
    r->pos += size;

    return value;
}

static uint64_t rd_uleb(eh_reader *r)
{
    uint64_t value;
    unsigned int shift;
    uint8_t byte;

    value = 0;
    shift = 0;
    do
    {
        if (r->error || r->pos >= r->end)
        {
            r->error = 1;
            return 0;
        }
        byte = r->data[r->pos++];
        if (shift < 64) value |= ((uint64_t)(byte & 0x7f)) << shift;
        shift += 7;
    } while (byte & 0x80);

    return value;
}

static int64_t rd_sleb(eh_reader *r)
{
    uint64_t value;
    unsigned int shift;
    uint8_t byte;

    value = 0;
    shift = 0;
    do
    {
        if (r->error || r->pos >= r->end)
        {
            r->error = 1;
            return 0;
        }
        byte = r->data[r->pos++];
        if (shift < 64) value |= ((uint64_t)(byte & 0x7f)) << shift;
        shift += 7;
    } while (byte & 0x80);

    if (shift < 64 && (byte & 0x40)) value |= ~UINT64_C(0) << shift;

    return (int64_t)value;
}

// size of pointer with fixed size format (0 = variable size or unknown format)
static unsigned int eh_encoded_size(uint8_t enc)
{
    switch (enc & 0x0f)
    {
        case DW_EH_PE_absptr:
        case DW_EH_PE_udata8:
        case DW_EH_PE_sdata8:
            return 8;
        case DW_EH_PE_udata4:
        case DW_EH_PE_sdata4:
            return 4;
        case DW_EH_PE_udata2:
        case DW_EH_PE_sdata2:
            return 2;
        default:
            return 0;
    }
}

// reads encoded pointer: raw value and address (pcrel applied, indirection not applied), returns 0 on success
static int rd_encoded(eh_reader *r, uint8_t enc, uint64_t *raw, uint64_t *address)
{
    uint64_t field, value;

    field = r->start + r->pos;

    switch (enc & 0x0f)
    {
        case DW_EH_PE_absptr:
        case DW_EH_PE_udata8:
        case DW_EH_PE_sdata8:
            value = rd_fixed(r, 8);
            break;
        case DW_EH_PE_udata4:
            value = rd_fixed(r, 4);
            break;
        case DW_EH_PE_sdata4:
            value = (uint64_t)(int64_t)(int32_t)(uint32_t)rd_fixed(r, 4);
            break;
        case DW_EH_PE_udata2:
            value = rd_fixed(r, 2);
            break;
        case DW_EH_PE_sdata2:
            value = (uint64_t)(int64_t)(int16_t)(uint16_t)rd_fixed(r, 2);
            break;
        case DW_EH_PE_uleb128:
            value = rd_uleb(r);
            break;
        case DW_EH_PE_sleb128:
            value = (uint64_t)rd_sleb(r);
            break;
        default:
            return 1;
    }

    if (r->error) return 1;

    *raw = value;

    switch (enc & 0x70)
    {
        case 0:
            *address = value;
            break;
        case DW_EH_PE_pcrel:
            *address = (value != 0)?(field + value):0;
            break;
        default:
            // textrel, datarel, funcrel, aligned - not used on x86-64
            return 1;
    }

    return 0;
}

// label of position in code: recompiled code (raw label, set to be written in output) or original code which is kept
static void eh_code_label(char *buf, unsigned int SecNum, uint_fast32_t RelAdr)
{
    output_data *output;

    if (SR_is_ignored(SecNum, RelAdr))
    {
        sprintf(buf, "srw_orig_%u+0x%x", SecNum, (unsigned int)RelAdr);
        return;
    }

    // address which isn't start of instruction is rounded up to the next instruction
    // (label+displacement would be wrong - the recompiled instructions can have different sizes)
    output = section_output_list_FindEntryEqualOrHigher(SecNum, RelAdr);
    if (output != NULL && output->ofs < section[SecNum].size)
    {
        output->has_label = 1;
        SR_get_label_def(buf, section[SecNum].start + output->ofs);
        return;
    }

    // end of section
    sprintf(buf, "srw_end_%u", SecNum);
}

// label (expression) for address
static void eh_label(char *buf, uint64_t address)
{
    uint_fast32_t SecNum, RelAdr;

    if (SR_get_section_reladr_exact(address, &SecNum, &RelAdr))
    {
        if (section[SecNum].type == ST_CODE)
        {
            eh_code_label(buf, SecNum, RelAdr);
            return;
        }

        // data (LSDA, type information, pointer to personality routine)
        SR_get_label_expr(buf, SecNum, RelAdr, 1);
    }
    else
    {
        SR_get_label(buf, address);
    }
}

// label for end of code range (ref = address in the range):
// - end of section of the range: label at the end of the section
// - original code which is not recompiled (function kept in original code): the next recompiled instruction
static void eh_end_label(char *buf, uint64_t ref, uint64_t address)
{
    uint_fast32_t SecNum, RelAdr;
    output_data *output;

    if (SR_get_section_reladr_exact(ref, &SecNum, &RelAdr) && section[SecNum].type == ST_CODE && address >= section[SecNum].start)
    {
        if (address >= section[SecNum].start + section[SecNum].size)
        {
            sprintf(buf, "srw_end_%u", (unsigned int)SecNum);
            return;
        }

        RelAdr = (uint_fast32_t)(address - section[SecNum].start);
        if (SR_is_ignored(SecNum, RelAdr) && !SR_is_ignored(SecNum, (uint_fast32_t)(ref - section[SecNum].start)))
        {
            output = section_output_list_FindEntryEqualOrHigher(SecNum, RelAdr);
            if (output != NULL && output->ofs < section[SecNum].size)
            {
                output->has_label = 1;
                SR_get_label_def(buf, section[SecNum].start + output->ofs);
            }
            else
            {
                sprintf(buf, "srw_end_%u", (unsigned int)SecNum);
            }
            return;
        }

        eh_code_label(buf, SecNum, RelAdr);
        return;
    }

    eh_label(buf, address);
}

// is address in recompiled code ?
static int eh_is_recompiled_code(uint64_t address)
{
    uint_fast32_t SecNum, RelAdr;

    if (!SR_get_section_reladr_exact(address, &SecNum, &RelAdr)) return 0;
    if (section[SecNum].type != ST_CODE) return 0;
    if (SR_is_ignored(SecNum, RelAdr)) return 0;

    return 1;
}

// writes pointer with given encoding (fixed size formats), target is label expression or NULL (null pointer)
static int eh_write_encoded(eh_buffer *buf, uint8_t enc, const char *target)
{
    const char *directive;

    switch (eh_encoded_size(enc))
    {
        case 8: directive = "dq"; break;
        case 4: directive = "dd"; break;
        case 2: directive = "dw"; break;
        default: return 1;
    }

    if (target == NULL) return eh_printf(buf, "%s 0\n", directive);

    if ((enc & 0x70) == DW_EH_PE_pcrel)
    {
        return eh_printf(buf, "%s (%s) - $\n", directive, target);
    }
    else
    {
        return eh_printf(buf, "%s %s\n", directive, target);
    }
}


static eh_cie *eh_find_cie(uint64_t address)
{
    unsigned int index;

    for (index = 0; index < eh_num_cies; index++)
    {
        if (eh_cies[index].address == address) return &(eh_cies[index]);
    }

    return NULL;
}

// parses CIE, returns 0 if the CIE is supported
static int eh_parse_cie(eh_cie *cie, const eh_reader *section_reader, uint64_t address, uint64_t record_end)
{
    eh_reader r;
    const char *aug;
    uint64_t aug_start;
    uint8_t version;

    memset(cie, 0, sizeof(eh_cie));
    cie->address = address;
    cie->record_end = record_end;
    cie->index = -1;
    cie->fde_enc = DW_EH_PE_absptr;
    cie->lsda_enc = DW_EH_PE_omit;
    cie->pers_enc = DW_EH_PE_omit;

    r = *section_reader;
    r.end = record_end;
    r.pos = (address - r.start) + 8;    // length, CIE id

    version = (uint8_t)rd_fixed(&r, 1);
    if (r.error || (version != 1 && version != 3)) return 1;

    aug = (const char *) &(r.data[r.pos]);
    aug_start = r.pos;
    while (r.pos < r.end && r.data[r.pos] != 0) r.pos++;
    if (r.pos >= r.end || r.pos - aug_start > 16) return 1;
    r.pos++;

    if (aug[0] != 0 && aug[0] != 'z') return 1;     // old augmentations (eh)

    cie->caf = rd_uleb(&r);
    rd_sleb(&r);                                    // data alignment factor
    if (version == 1) rd_fixed(&r, 1); else rd_uleb(&r);   // return address register
    if (r.error) return 1;

    if (aug[0] == 'z')
    {
        uint64_t aug_len, aug_data_end;
        const char *ch;

        cie->has_z = 1;
        aug_len = rd_uleb(&r);
        aug_data_end = r.pos + aug_len;
        if (r.error || aug_data_end > r.end) return 1;

        for (ch = aug + 1; *ch; ch++)
        {
            switch (*ch)
            {
                case 'L':
                    cie->has_lsda = 1;
                    cie->lsda_enc = (uint8_t)rd_fixed(&r, 1);
                    break;
                case 'R':
                    cie->fde_enc = (uint8_t)rd_fixed(&r, 1);
                    break;
                case 'P':
                    cie->has_pers = 1;
                    cie->pers_enc = (uint8_t)rd_fixed(&r, 1);
                    cie->pers_field = r.pos;
                    if (rd_encoded(&r, cie->pers_enc, &(cie->pers_raw), &(cie->pers_target))) return 1;
                    break;
                case 'S':
                    break;
                default:
                    return 1;
            }
        }
        if (r.error || r.pos != aug_data_end) return 1;
    }

    if (cie->caf != 1) return 1;
    if (eh_encoded_size(cie->fde_enc) == 0) return 1;
    if (cie->has_lsda && eh_encoded_size(cie->lsda_enc) == 0) return 1;
    if (cie->has_pers && eh_encoded_size(cie->pers_enc) == 0) return 1;

    cie->supported = 1;
    return 0;
}

// parses all CIEs in .eh_frame
static void eh_parse_cies(const eh_reader *sr)
{
    eh_reader r;
    uint64_t pos, length, record_end;

    eh_num_cies = 0;
    r = *sr;
    pos = 0;
    while (pos + 4 <= r.end)
    {
        r.pos = pos;
        length = rd_fixed(&r, 4);
        if (length == 0)
        {
            pos += 4;
            continue;
        }
        if (length == 0xffffffff)
        {
            fprintf(stderr, "Warning: 64-bit .eh_frame records are not supported\n");
            break;
        }
        record_end = pos + 4 + length;
        if (record_end > r.end) break;

        if (rd_fixed(&r, 4) == 0)
        {
            if (eh_num_cies >= MAX_CIES)
            {
                fprintf(stderr, "Warning: too many CIEs in .eh_frame\n");
                break;
            }
            eh_parse_cie(&(eh_cies[eh_num_cies]), sr, r.start + pos, record_end);
            eh_num_cies++;
        }

        pos = record_end;
    }
}

// reads FDE at given offset: returns 0 = FDE, 1 = CIE or terminator, 2 = unsupported FDE, -1 = end of section
static int eh_read_fde(const eh_reader *sr, uint64_t pos, eh_fde *fde, uint64_t *next_pos)
{
    eh_reader r;
    uint64_t length, raw;
    uint32_t id;

    r = *sr;
    if (pos + 4 > r.end) return -1;

    r.pos = pos;
    length = rd_fixed(&r, 4);
    if (length == 0)
    {
        *next_pos = pos + 4;
        return 1;
    }
    if (length == 0xffffffff || pos + 4 + length > r.end) return -1;

    memset(fde, 0, sizeof(eh_fde));
    fde->pos = pos;
    fde->record_end = pos + 4 + length;
    *next_pos = fde->record_end;

    fde->id_pos = r.pos;
    id = (uint32_t)rd_fixed(&r, 4);
    if (id == 0) return 1;

    fde->cie = eh_find_cie(r.start + fde->id_pos - id);
    if (fde->cie == NULL || !fde->cie->supported) return 2;

    r.end = fde->record_end;
    if (rd_encoded(&r, fde->cie->fde_enc, &raw, &(fde->pc_begin))) return 2;
    if (rd_encoded(&r, fde->cie->fde_enc & 0x0f, &(fde->pc_range), &raw)) return 2;

    if (fde->cie->has_z)
    {
        uint64_t aug_end;

        fde->aug_len = rd_uleb(&r);
        aug_end = r.pos + fde->aug_len;
        if (r.error || aug_end > fde->record_end) return 2;
        if (fde->cie->has_lsda)
        {
            if (rd_encoded(&r, fde->cie->lsda_enc, &(fde->lsda_raw), &(fde->lsda))) return 2;
        }
        r.pos = aug_end;
    }

    fde->instr_pos = r.pos;
    return 0;
}

// writes copy of CIE (personality routine pointer is converted)
static int eh_write_cie(eh_cie *cie, const eh_reader *sr, int index)
{
    char label[256];
    uint64_t start, raw;

    cie->index = index;
    start = (cie->address - sr->start) + 4;     // CIE id

    if (eh_printf(&eh_frame_text, "\nsrw_ehc_%i:\ndd srw_ehc_%i_e - srw_ehc_%i_s\nsrw_ehc_%i_s:\n", index, index, index, index)) return -1;

    if (cie->has_pers)
    {
        // CIE id .. personality encoding
        if (eh_bytes(&eh_frame_text, &(sr->data[start]), cie->pers_field - start)) return -1;

        // personality routine (or pointer to it)
        if (cie->pers_raw == 0)
        {
            if (eh_write_encoded(&eh_frame_text, cie->pers_enc, NULL)) return -1;
        }
        else
        {
            eh_label(label, cie->pers_target);
            if (eh_write_encoded(&eh_frame_text, cie->pers_enc, label)) return -1;
        }

        // rest of augmentation data and initial instructions
        raw = cie->pers_field + eh_encoded_size(cie->pers_enc);
        if (eh_bytes(&eh_frame_text, &(sr->data[raw]), cie->record_end - raw)) return -1;
    }
    else
    {
        if (eh_bytes(&eh_frame_text, &(sr->data[start]), cie->record_end - start)) return -1;
    }

    if (eh_printf(&eh_frame_text, "align 8, db 0\nsrw_ehc_%i_e:\n", index)) return -1;

    return 0;
}

// finds .eh_frame section
static int eh_find_section(eh_reader *sr)
{
    unsigned int Entry;

    for (Entry = 0; Entry < num_sections; Entry++)
    {
        if (strcmp(section[Entry].name, ".eh_frame") == 0) break;
    }
    if (Entry >= num_sections || section[Entry].size_in_file == 0) return 1;

    sr->data = section[Entry].adr;
    sr->start = section[Entry].start;
    sr->pos = 0;
    sr->end = section[Entry].size_in_file;
    sr->error = 0;

    return 0;
}

static int eh_add_landing_pad(uint64_t address)
{
    if (eh_num_landing_pads >= eh_alloc_landing_pads)
    {
        uint64_t *new_pads;

        new_pads = (uint64_t *) realloc(eh_landing_pads, (eh_alloc_landing_pads + 1024) * sizeof(uint64_t));
        if (new_pads == NULL) return -1;
        eh_landing_pads = new_pads;
        eh_alloc_landing_pads += 1024;
    }

    eh_landing_pads[eh_num_landing_pads++] = address;
    return 0;
}

// finds landing pads in LSDA (C++ exception table)
static int eh_scan_lsda(uint64_t lsda, uint64_t func_start)
{
    eh_reader r;
    uint8_t lpstart_enc, ttype_enc, cs_enc;
    uint64_t raw, lpstart, cs_end;

    if (eh_reader_init(&r, lsda)) return 1;

    lpstart_enc = (uint8_t)rd_fixed(&r, 1);
    lpstart = func_start;
    if (lpstart_enc != DW_EH_PE_omit)
    {
        if (rd_encoded(&r, lpstart_enc, &raw, &lpstart)) return 1;
    }

    ttype_enc = (uint8_t)rd_fixed(&r, 1);
    if (ttype_enc != DW_EH_PE_omit) rd_uleb(&r);

    cs_enc = (uint8_t)rd_fixed(&r, 1);
    cs_end = rd_uleb(&r);
    if (r.error || (cs_enc & 0x70) != 0) return 1;
    cs_end += r.pos;
    if (cs_end > r.end) return 1;
    r.end = cs_end;

    while (r.pos < cs_end)
    {
        uint64_t start, len, lp;

        if (rd_encoded(&r, cs_enc, &start, &raw)) return 1;
        if (rd_encoded(&r, cs_enc, &len, &raw)) return 1;
        if (rd_encoded(&r, cs_enc, &lp, &raw)) return 1;
        rd_uleb(&r);
        if (r.error) return 1;

        if (lp != 0)
        {
            if (eh_add_landing_pad(lpstart + lp)) return -1;
        }
    }

    return 0;
}

// processing before disassembly: functions (FDEs) and landing pads (LSDAs) from exception handling data
int SR_elf_eh_prescan(uint64_t **ranges, unsigned int *num_ranges)
{
    eh_reader sr;
    eh_fde fde;
    uint64_t pos, next_pos;
    unsigned int count, alloc;
    int ret;

    *ranges = NULL;
    *num_ranges = 0;

    if (eh_find_section(&sr)) return 0;

    eh_parse_cies(&sr);

    count = 0;
    alloc = 0;
    pos = 0;
    while ((ret = eh_read_fde(&sr, pos, &fde, &next_pos)) >= 0)
    {
        pos = next_pos;
        if (ret != 0 || fde.pc_begin == 0) continue;

        // address range of function
        if (count >= alloc)
        {
            uint64_t *new_ranges;

            new_ranges = (uint64_t *) realloc(*ranges, 2 * (alloc + 1024) * sizeof(uint64_t));
            if (new_ranges == NULL) return -1;
            *ranges = new_ranges;
            alloc += 1024;
        }
        (*ranges)[2 * count] = fde.pc_begin;
        (*ranges)[2 * count + 1] = fde.pc_begin + fde.pc_range;
        count++;

        // landing pads
        if (fde.lsda_raw != 0)
        {
            if (eh_scan_lsda(fde.lsda, fde.pc_begin) < 0) return -1;
        }
    }

    *num_ranges = count;

    if (eh_num_landing_pads != 0)
    {
        fprintf(stderr, "\tLanding pads in C++ exception tables: %u\n", eh_num_landing_pads);
    }

    return 0;
}

// landing pads are code entries
int SR_elf_eh_add_entries(void)
{
    unsigned int index;
    uint_fast32_t SecNum, RelAdr;
    output_data *output;

    for (index = 0; index < eh_num_landing_pads; index++)
    {
        if (!SR_get_section_reladr_exact(eh_landing_pads[index], &SecNum, &RelAdr)) continue;
        if (section[SecNum].type != ST_CODE || SR_is_ignored(SecNum, RelAdr)) continue;

        section_entry_list_Insert(SecNum, RelAdr);

        output = section_output_list_FindEntryEqual(SecNum, RelAdr);
        if (output != NULL) output->has_label = 1;
    }

    return 0;
}

// converts call frame instructions (code offsets as label differences), returns 0 on success
static int eh_convert_instructions(eh_buffer *buf, eh_reader *r, uint64_t loc, uint64_t loc_end, uint8_t fde_enc, int convert)
{
    uint64_t loc_begin = loc;
    uint64_t pending_start, new_loc, raw;
    char label1[256], label2[256];
    uint8_t op;

    pending_start = r->pos;

    while (r->pos < r->end)
    {
        uint64_t op_start;
        int advance;

        op_start = r->pos;
        op = (uint8_t)rd_fixed(r, 1);
        advance = 0;
        new_loc = loc;

        switch (op & 0xc0)
        {
            case 0x40:  // DW_CFA_advance_loc
                advance = 1;
                new_loc = loc + (op & 0x3f);
                break;
            case 0x80:  // DW_CFA_offset
                rd_uleb(r);
                break;
            case 0xc0:  // DW_CFA_restore
                break;
            default:
                switch (op)
                {
                    case 0x00:  // DW_CFA_nop
                    case 0x0a:  // DW_CFA_remember_state
                    case 0x0b:  // DW_CFA_restore_state
                    case 0x2d:  // DW_CFA_GNU_window_save
                        break;
                    case 0x01:  // DW_CFA_set_loc
                        advance = 2;
                        if (rd_encoded(r, fde_enc, &raw, &new_loc)) return 1;
                        break;
                    case 0x02:  // DW_CFA_advance_loc1
                        advance = 1;
                        new_loc = loc + rd_fixed(r, 1);
                        break;
                    case 0x03:  // DW_CFA_advance_loc2
                        advance = 1;
                        new_loc = loc + rd_fixed(r, 2);
                        break;
                    case 0x04:  // DW_CFA_advance_loc4
                        advance = 1;
                        new_loc = loc + rd_fixed(r, 4);
                        break;
                    case 0x06:  // DW_CFA_restore_extended
                    case 0x07:  // DW_CFA_undefined
                    case 0x08:  // DW_CFA_same_value
                    case 0x0d:  // DW_CFA_def_cfa_register
                    case 0x0e:  // DW_CFA_def_cfa_offset
                    case 0x2e:  // DW_CFA_GNU_args_size
                        rd_uleb(r);
                        break;
                    case 0x13:  // DW_CFA_def_cfa_offset_sf
                        rd_sleb(r);
                        break;
                    case 0x05:  // DW_CFA_offset_extended
                    case 0x09:  // DW_CFA_register
                    case 0x0c:  // DW_CFA_def_cfa
                    case 0x14:  // DW_CFA_val_offset
                    case 0x2f:  // DW_CFA_GNU_negative_offset_extended
                        rd_uleb(r);
                        rd_uleb(r);
                        break;
                    case 0x11:  // DW_CFA_offset_extended_sf
                    case 0x12:  // DW_CFA_def_cfa_sf
                    case 0x15:  // DW_CFA_val_offset_sf
                        rd_uleb(r);
                        rd_sleb(r);
                        break;
                    case 0x0f:  // DW_CFA_def_cfa_expression
                        raw = rd_uleb(r);
                        if (r->error || r->pos + raw > r->end) return 1;
                        r->pos += raw;
                        break;
                    case 0x10:  // DW_CFA_expression
                    case 0x16:  // DW_CFA_val_expression
                        rd_uleb(r);
                        raw = rd_uleb(r);
                        if (r->error || r->pos + raw > r->end) return 1;
                        r->pos += raw;
                        break;
                    default:
                        return 1;
                }
                break;
        }

        if (r->error) return 1;

        // address in DW_CFA_set_loc relative to its position can't be copied
        if (advance == 2 && !convert && (fde_enc & 0x70) == DW_EH_PE_pcrel) return 1;

        if (advance && convert)
        {
            if (new_loc < loc || new_loc > loc_end) return 1;

            // pending instructions
            if (op_start > pending_start)
            {
                if (eh_bytes(buf, &(r->data[pending_start]), op_start - pending_start)) return -1;
            }
            pending_start = r->pos;

            if (!eh_is_recompiled_code(new_loc) && new_loc != loc_end) return 1;

            if (new_loc == loc_end) eh_end_label(label2, loc_begin, new_loc); else eh_label(label2, new_loc);
            if (advance == 2)
            {
                if (eh_printf(buf, "db 0x01\n")) return -1;
                if (eh_write_encoded(buf, fde_enc, label2)) return 1;
            }
            else if (new_loc != loc)
            {
                eh_label(label1, loc);
                if (eh_printf(buf, "db 0x04\ndd (%s) - (%s)\n", label2, label1)) return -1;
            }
        }

        loc = new_loc;
    }

    if (r->pos > pending_start)
    {
        if (eh_bytes(buf, &(r->data[pending_start]), r->pos - pending_start)) return -1;
    }

    return 0;
}

// generates LSDA (C++ exception table) for recompiled function, returns 0 on success
static int eh_convert_lsda(eh_buffer *buf, unsigned int fde_index, uint64_t lsda, uint64_t func_start)
{
    eh_reader r;
    uint8_t lpstart_enc, ttype_enc, cs_enc;
    uint64_t raw, lpstart, ttype_base, cs_len, cs_start, cs_end, action_end, spec_end, type_start, new_cs_len;
    uint64_t max_type, iterations, data_end;
    unsigned int ttype_size;
    char label_func[256], label1[256], label2[256];

    if (eh_reader_init(&r, lsda)) return 1;
    data_end = r.end;

    lpstart_enc = (uint8_t)rd_fixed(&r, 1);
    lpstart = func_start;
    if (lpstart_enc != DW_EH_PE_omit)
    {
        if (rd_encoded(&r, lpstart_enc, &raw, &lpstart)) return 1;
    }

    ttype_enc = (uint8_t)rd_fixed(&r, 1);
    ttype_base = 0;
    ttype_size = 0;
    if (ttype_enc != DW_EH_PE_omit)
    {
        raw = rd_uleb(&r);
        ttype_base = r.pos + raw;
        ttype_size = eh_encoded_size(ttype_enc);
        if (ttype_size == 0) return 1;
        if ((ttype_enc & 0x70) != 0 && (ttype_enc & 0x70) != DW_EH_PE_pcrel) return 1;
    }

    cs_enc = (uint8_t)rd_fixed(&r, 1);
    cs_len = rd_uleb(&r);
    if (r.error || (cs_enc & 0x70) != 0) return 1;
    cs_start = r.pos;
    cs_end = r.pos + cs_len;
    if (cs_end > r.end) return 1;

    // call-site table: size of new table, action table extent
    action_end = cs_end;
    spec_end = ttype_base;
    max_type = 0;
    new_cs_len = 0;
    r.end = cs_end;
    while (r.pos < cs_end)
    {
        uint64_t start, len, lp, action, rec;

        if (rd_encoded(&r, cs_enc, &start, &raw)) return 1;
        if (rd_encoded(&r, cs_enc, &len, &raw)) return 1;
        if (rd_encoded(&r, cs_enc, &lp, &raw)) return 1;
        action = rd_uleb(&r);
        if (r.error) return 1;

        new_cs_len += 12 + eh_uleb_size(action);

        // actions
        rec = (action != 0)?(cs_end + action - 1):0;
        iterations = 0;
        while (rec != 0)
        {
            eh_reader ra;
            int64_t filter, disp;
            uint64_t disp_pos;

            if (++iterations > 10000) return 1;

            if (eh_reader_init(&ra, r.start + rec)) return 1;
            filter = rd_sleb(&ra);
            disp_pos = ra.pos;
            disp = rd_sleb(&ra);
            if (ra.error) return 1;
            if (ra.pos > action_end) action_end = ra.pos;

            if (filter > 0)
            {
                if ((uint64_t)filter > max_type) max_type = (uint64_t)filter;
            }
            else if (filter < 0)
            {
                // exception specification: list of type indexes after type table base
                eh_reader rs;
                uint64_t type_index;

                if (ttype_enc == DW_EH_PE_omit) return 1;
                if (eh_reader_init(&rs, r.start + ttype_base + (uint64_t)(-filter - 1))) return 1;
                do
                {
                    type_index = rd_uleb(&rs);
                    if (rs.error) return 1;
                    if (type_index > max_type) max_type = type_index;
                } while (type_index != 0);
                if (rs.pos > spec_end) spec_end = rs.pos;
            }

            rec = (disp != 0)?(uint64_t)((int64_t)disp_pos + disp):0;
        }
    }

    type_start = action_end;
    if (ttype_enc != DW_EH_PE_omit)
    {
        if (max_type * ttype_size > ttype_base) return 1;
        type_start = ttype_base - max_type * ttype_size;
        if (type_start < action_end) return 1;
        if (ttype_base > data_end || spec_end > data_end) return 1;
    }
    if (action_end > data_end) return 1;

    // write new LSDA
    eh_label(label_func, func_start);

    if (eh_printf(buf, "\nsrw_lsda_%u:\ndb 0xff, 0x%02x\n", fde_index, ttype_enc)) return -1;
    if (ttype_enc != DW_EH_PE_omit)
    {
        uint64_t new_offset;

        new_offset = 1 + eh_uleb_size(new_cs_len) + new_cs_len + (action_end - cs_end) + (ttype_base - action_end);
        if (eh_uleb(buf, new_offset)) return -1;
    }
    if (eh_printf(buf, "db 0x%02x\n", DW_EH_PE_udata4)) return -1;
    if (eh_uleb(buf, new_cs_len)) return -1;

    // call-site table
    r.pos = cs_start;
    while (r.pos < cs_end)
    {
        uint64_t start, len, lp, action;

        rd_encoded(&r, cs_enc, &start, &raw);
        rd_encoded(&r, cs_enc, &len, &raw);
        rd_encoded(&r, cs_enc, &lp, &raw);
        action = rd_uleb(&r);

        if (!eh_is_recompiled_code(func_start + start)) return 1;
        if (len != 0 && !eh_is_recompiled_code(func_start + start + len - 1)) return 1;

        eh_label(label1, func_start + start);
        eh_end_label(label2, func_start + start, func_start + start + len);
        if (eh_printf(buf, "dd (%s) - (%s)\ndd (%s) - (%s)\n", label1, label_func, label2, label1)) return -1;

        if (lp != 0)
        {
            if (!eh_is_recompiled_code(lpstart + lp)) return 1;
            eh_label(label1, lpstart + lp);
            if (eh_printf(buf, "dd (%s) - (%s)\n", label1, label_func)) return -1;
        }
        else
        {
            if (eh_printf(buf, "dd 0\n")) return -1;
        }

        if (eh_uleb(buf, action)) return -1;
    }

    // action table, alignment
    if (eh_bytes(buf, &(r.data[cs_end]), type_start - cs_end)) return -1;

    if (ttype_enc != DW_EH_PE_omit)
    {
        // type table
        r.end = ttype_base;
        r.pos = type_start;
        while (r.pos < ttype_base)
        {
            uint64_t target;

            if ((ttype_enc & 0x70) == DW_EH_PE_pcrel)
            {
                if (rd_encoded(&r, ttype_enc, &raw, &target)) return 1;
                if (raw == 0)
                {
                    if (eh_write_encoded(buf, ttype_enc, NULL)) return -1;
                }
                else
                {
                    eh_label(label1, target);
                    if (eh_write_encoded(buf, ttype_enc, label1)) return -1;
                }
            }
            else
            {
                if (eh_bytes(buf, &(r.data[r.pos]), ttype_size)) return -1;
                r.pos += ttype_size;
            }
        }

        // exception specifications
        if (spec_end > ttype_base)
        {
            if (eh_bytes(buf, &(r.data[ttype_base]), spec_end - ttype_base)) return -1;
        }
    }

    return 0;
}

// processing after disassembly: generates new .eh_frame and LSDAs
int SR_elf_eh_post_process(void)
{
    unsigned int index, fde_index, converted, kept, failed, lsdas;
    int cie_count, ret;
    eh_reader sr;
    eh_fde fde;
    uint64_t pos, next_pos;

    eh_frame_text.len = 0;
    eh_except_text.len = 0;

    if (InputFormat != INPUT_ELF || !convert_exception_data) return 0;

    if (eh_find_section(&sr)) return 0;

    eh_parse_cies(&sr);

    // copies of CIEs
    cie_count = 0;
    for (index = 0; index < eh_num_cies; index++)
    {
        if (!eh_cies[index].supported) continue;
        if (eh_write_cie(&(eh_cies[index]), &sr, cie_count)) return -1;
        cie_count++;
    }

    // FDEs
    fde_index = 0;
    converted = 0;
    kept = 0;
    failed = 0;
    lsdas = 0;
    pos = 0;
    while ((ret = eh_read_fde(&sr, pos, &fde, &next_pos)) >= 0)
    {
        eh_reader r;
        eh_buffer fde_text;
        char label_begin[256], label_end[256], label[256];
        int convert;
        uint_fast32_t SecNum, RelAdr;

        pos = next_pos;
        if (ret == 1) continue;
        if (ret == 2)
        {
            failed++;
            continue;
        }
        if (fde.pc_begin == 0) continue;   // deleted FDE

        // FDEs of code which is not in code sections are not needed
        if (!SR_get_section_reladr_exact(fde.pc_begin, &SecNum, &RelAdr) || section[SecNum].type != ST_CODE) continue;

        convert = eh_is_recompiled_code(fde.pc_begin);

        // code which is not recompiled is described only if it's kept in original code
        if (!convert && !keep_original_code) continue;

        memset(&fde_text, 0, sizeof(fde_text));

        ret = eh_printf(&fde_text, "\ndd srw_ehf_%u_e - srw_ehf_%u_s\nsrw_ehf_%u_s:\ndd srw_ehf_%u_s - srw_ehc_%i\n", fde_index, fde_index, fde_index, fde_index, fde.cie->index);

        // address range
        eh_label(label_begin, fde.pc_begin);
        if (ret == 0) ret = eh_write_encoded(&fde_text, fde.cie->fde_enc, label_begin);
        if (ret == 0)
        {
            if (convert)
            {
                eh_end_label(label_end, fde.pc_begin, fde.pc_begin + fde.pc_range);
                ret = eh_printf(&fde_text, "%s (%s) - (%s)\n", (eh_encoded_size(fde.cie->fde_enc) == 8)?"dq":"dd", label_end, label_begin);
            }
            else
            {
                ret = eh_bytes(&fde_text, &(sr.data[fde.id_pos + 4 + eh_encoded_size(fde.cie->fde_enc)]), eh_encoded_size(fde.cie->fde_enc));
            }
        }

        // augmentation data (LSDA)
        if (ret == 0 && fde.cie->has_z)
        {
            ret = eh_uleb(&fde_text, fde.aug_len);
            if (ret == 0 && fde.cie->has_lsda)
            {
                if (fde.lsda_raw == 0)
                {
                    ret = eh_write_encoded(&fde_text, fde.cie->lsda_enc, NULL);
                }
                else if (convert)
                {
                    eh_buffer lsda_text;

                    memset(&lsda_text, 0, sizeof(lsda_text));
                    ret = eh_convert_lsda(&lsda_text, fde_index, fde.lsda, fde.pc_begin);
                    if (ret == 0)
                    {
                        ret = eh_printf(&eh_except_text, "%s", (lsda_text.data != NULL)?lsda_text.data:"");
                        sprintf(label, "srw_lsda_%u", fde_index);
                        if (ret == 0) ret = eh_write_encoded(&fde_text, fde.cie->lsda_enc, label);
                        lsdas++;
                    }
                    else if (ret > 0)
                    {
                        fprintf(stderr, "Warning: unable to convert LSDA: 0x%" PRIx64 " (function 0x%" PRIx64 ")\n", fde.lsda, fde.pc_begin);
                    }
                    if (lsda_text.data != NULL) free(lsda_text.data);
                }
                else
                {
                    // original code uses original LSDA
                    eh_label(label, fde.lsda);
                    ret = eh_write_encoded(&fde_text, fde.cie->lsda_enc, label);
                }
            }
        }

        // call frame instructions
        if (ret == 0)
        {
            r = sr;
            r.pos = fde.instr_pos;
            r.end = fde.record_end;
            ret = eh_convert_instructions(&fde_text, &r, fde.pc_begin, fde.pc_begin + fde.pc_range, fde.cie->fde_enc, convert);
        }

        if (ret == 0) ret = eh_printf(&fde_text, "align 8, db 0\nsrw_ehf_%u_e:\n", fde_index);

        if (ret < 0)
        {
            if (fde_text.data != NULL) free(fde_text.data);
            return -1;
        }

        if (ret == 0)
        {
            if (eh_printf(&eh_frame_text, "%s", fde_text.data)) return -1;
            if (convert) converted++; else kept++;
            fde_index++;
        }
        else
        {
            fprintf(stderr, "Warning: unable to convert .eh_frame FDE: 0x%" PRIx64 " (function 0x%" PRIx64 ")\n", sr.start + fde.pos, fde.pc_begin);
            failed++;
        }

        if (fde_text.data != NULL) free(fde_text.data);
    }

    fprintf(stderr, "\tFDEs in .eh_frame: converted %u, original code %u, failed %u, converted LSDAs %u\n", converted, kept, failed, lsdas);

    return 0;
}

// writes new .eh_frame and LSDAs
int SR_elf_eh_write(FILE *fout)
{
    if (eh_frame_text.len == 0) return 0;

    fprintf(fout, "\n; exception handling data for recompiled code (link with --eh-frame-hdr)\n");
    fprintf(fout, "section .eh_frame progbits alloc noexec nowrite align=8\n");
    fwrite(eh_frame_text.data, 1, eh_frame_text.len, fout);
    fprintf(fout, "dd 0\n");

    if (eh_except_text.len != 0)
    {
        fprintf(fout, "\nsection srw.except progbits alloc noexec nowrite align=4\n");
        fwrite(eh_except_text.data, 1, eh_except_text.len, fout);
    }

    return 0;
}

int SR_elf_eh_present(void)
{
    return (eh_frame_text.len != 0)?1:0;
}

void SR_elf_eh_free(void)
{
    if (eh_landing_pads != NULL) free(eh_landing_pads);
    eh_landing_pads = NULL;
    eh_num_landing_pads = 0;
    eh_alloc_landing_pads = 0;

    if (eh_frame_text.data != NULL) free(eh_frame_text.data);
    if (eh_except_text.data != NULL) free(eh_except_text.data);
    memset(&eh_frame_text, 0, sizeof(eh_frame_text));
    memset(&eh_except_text, 0, sizeof(eh_except_text));
    eh_num_cies = 0;
}
