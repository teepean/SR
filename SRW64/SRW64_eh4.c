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
 * C++ exception handling data version 4 (__CxxFrameHandler4, __GSHandlerCheck_EH4),
 * used by Visual Studio 2019 and newer.
 *
 * The data is compressed (variable length unsigned integers and 32-bit image relative addresses):
 *
 *  FuncInfo4:        header (byte), [bbtFlags (uint)], [dispUnwindMap (int32)], [dispTryBlockMap (int32)],
 *                    dispIPtoStateMap (int32) or dispSegMap (int32) for separated code, [dispFrame (uint)]
 *  SegMap4:          count (uint), {segment start (int32), dispIPtoStateMap (int32)}
 *  UnwindMap4:       count (uint), {nextOffset << 2 | type (uint), [action (int32)], [object (uint)]}
 *  TryBlockMap4:     count (uint), {tryLow (uint), tryHigh (uint), catchHigh (uint), dispHandlerArray (int32)}
 *  HandlerMap4:      count (uint), {header (byte), [adjectives (uint)], [dispType (int32)], [dispCatchObj (uint)],
 *                    dispOfHandler (int32), [continuation addresses (int32 or uint)]}
 *  IPtoStateMap4:    count (uint), {ip offset delta (uint), state + 1 (uint)}
 *
 *  Compressed unsigned integer: the lowest bits of the first byte contain the length
 *  (xxx0 - 1 byte, xx01 - 2 bytes, x011 - 3 bytes, 0111 - 4 bytes, 1111 - 5 bytes).
 *  Generated offsets use the 4-byte form ((value << 4) | 7), because the 5-byte form
 *  is not supported by Wine.
 *
 * Image relative addresses are converted in place. IP to state maps and continuation addresses
 * relative to function start contain offsets in code, which change after recompilation,
 * so these maps are generated again (section srweh4) using label differences
 * and the references are changed to the new maps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SR_defs.h"
#include "SR_vars.h"

#define FH4_MAX_ENTRIES 65536

#define FH4_IS_CATCH        0x01
#define FH4_IS_SEPARATED    0x02
#define FH4_BBT             0x04
#define FH4_UNWIND_MAP      0x08
#define FH4_TRY_BLOCK_MAP   0x10
#define FH4_RESERVED        0x80

#define HT4_ADJECTIVES      0x01
#define HT4_DISP_TYPE       0x02
#define HT4_DISP_CATCH_OBJ  0x04
#define HT4_CONT_IS_RVA     0x08
#define HT4_CONT_ADDR_SHIFT 4

typedef struct _fh4_map_task_ {
    uint_fast32_t field_rva;    // address of the int32 which references the map
    uint_fast32_t map_rva;      // address of the map
    uint_fast32_t func_rva;     // function start (code offsets in the map are relative to it)
    int is_handler_map;         // 0 = IP to state map, 1 = handler map
    int status;                 // 0 = not processed, 1 = map generated, -1 = map not generated
} fh4_map_task;

static fh4_map_task *fh4_tasks = NULL;
static unsigned int fh4_num_tasks = 0, fh4_max_tasks = 0;

typedef struct _fh4_info_ {
    uint_fast32_t unwind_rva;   // unwind info
    uint_fast32_t data_rva;     // language specific handler data (int32 image relative address of FuncInfo4)
    uint_fast32_t func_rva;     // function, for which the data was converted in place
    int handler_kind;           // 0 = unknown handler, 1 = __CxxFrameHandler4, 2 = __GSHandlerCheck_EH4
} fh4_info;

static fh4_info *fh4_infos = NULL;
static unsigned int fh4_num_infos = 0, fh4_max_infos = 0;
static unsigned int fh4_copies = 0, fh4_copies_failed = 0;

static char *fh4_data = NULL;
static size_t fh4_data_length = 0, fh4_data_size = 0;

static unsigned int fh4_funcinfos = 0;
static unsigned int fh4_maps_converted = 0, fh4_maps_failed = 0;


static uint8_t *fh4_ptr(uint_fast32_t rva, uint_fast32_t size)
{
    return (uint8_t *) SR_rva_to_ptr(rva, size);
}

// reads compressed unsigned integer, returns its length in bytes (0 = error)
static unsigned int fh4_read_unsigned(uint_fast32_t rva, uint32_t *value)
{
    uint8_t *ptr;
    unsigned int length;

    ptr = fh4_ptr(rva, 1);
    if (ptr == NULL) return 0;

    if ((ptr[0] & 1) == 0) length = 1;
    else if ((ptr[0] & 2) == 0) length = 2;
    else if ((ptr[0] & 4) == 0) length = 3;
    else if ((ptr[0] & 8) == 0) length = 4;
    else length = 5;

    ptr = fh4_ptr(rva, length);
    if (ptr == NULL) return 0;

    switch (length)
    {
        case 1:
            *value = ptr[0] >> 1;
            break;
        case 2:
            *value = ((uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8)) >> 2;
            break;
        case 3:
            *value = ((uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16)) >> 3;
            break;
        case 4:
            *value = ((uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24)) >> 4;
            break;
        default:
            *value = (uint32_t)ptr[1] | ((uint32_t)ptr[2] << 8) | ((uint32_t)ptr[3] << 16) | ((uint32_t)ptr[4] << 24);
            break;
    }

    return length;
}

static int fh4_read_int(uint_fast32_t rva, int32_t *value)
{
    uint8_t *ptr;

    ptr = fh4_ptr(rva, 4);
    if (ptr == NULL) return 0;

    memcpy(value, ptr, 4);
    return 1;
}

static int fh4_is_rva(uint_fast32_t rva, int code)
{
    uint_fast32_t SecNum, RelAdr;

    if (rva < FirstSectionRVA || rva >= SizeOfImage) return 0;
    if (!SR_get_section_reladr_exact(ImageBase + rva, &SecNum, &RelAdr)) return 0;

    return (code == (section[SecNum].type == ST_CODE))?1:0;
}

static int fh4_add_task(uint_fast32_t field_rva, uint_fast32_t map_rva, uint_fast32_t func_rva, int is_handler_map)
{
    if (fh4_num_tasks >= fh4_max_tasks)
    {
        fh4_map_task *new_tasks;

        fh4_max_tasks = (fh4_max_tasks == 0)?256:(2 * fh4_max_tasks);
        new_tasks = (fh4_map_task *) realloc(fh4_tasks, fh4_max_tasks * sizeof(fh4_map_task));
        if (new_tasks == NULL)
        {
            fprintf(stderr, "Error: not enough memory\n");
            return -1;
        }
        fh4_tasks = new_tasks;
    }

    fh4_tasks[fh4_num_tasks].field_rva = field_rva;
    fh4_tasks[fh4_num_tasks].map_rva = map_rva;
    fh4_tasks[fh4_num_tasks].func_rva = func_rva;
    fh4_tasks[fh4_num_tasks].is_handler_map = is_handler_map;
    fh4_tasks[fh4_num_tasks].status = 0;
    fh4_num_tasks++;

    return 0;
}

static int fh4_append(const char *text)
{
    size_t length;

    length = strlen(text);
    if (fh4_data_length + length + 1 > fh4_data_size)
    {
        char *new_data;

        fh4_data_size = 2 * (fh4_data_length + length + 1) + 65536;
        new_data = (char *) realloc(fh4_data, fh4_data_size);
        if (new_data == NULL)
        {
            fprintf(stderr, "Error: not enough memory\n");
            return -1;
        }
        fh4_data = new_data;
    }

    memcpy(fh4_data + fh4_data_length, text, length + 1);
    fh4_data_length += length;

    return 0;
}

// appends original bytes as db directive
static int fh4_append_bytes(uint_fast32_t rva, unsigned int length)
{
    char buf[128];
    uint8_t *ptr;
    unsigned int index;

    ptr = fh4_ptr(rva, length);
    if (ptr == NULL) return -1;

    strcpy(buf, "db ");
    for (index = 0; index < length; index++)
    {
        sprintf(&(buf[strlen(buf)]), "%s0x%x", (index == 0)?"":", ", ptr[index]);
    }
    strcat(buf, "\n");

    return fh4_append(buf);
}

// checks IP to state map, returns 1 if the map is valid
static int fh4_check_ip_map(uint_fast32_t map_rva, uint_fast32_t func_rva, uint_fast32_t func_length, uint32_t max_state)
{
    uint32_t count, delta, state, index, ip;
    unsigned int length;
    uint_fast32_t pos;

    length = fh4_read_unsigned(map_rva, &count);
    if (length == 0 || count > FH4_MAX_ENTRIES) return 0;
    pos = map_rva + length;

    ip = 0;
    for (index = 0; index < count; index++)
    {
        length = fh4_read_unsigned(pos, &delta);
        if (length == 0) return 0;
        pos += length;
        length = fh4_read_unsigned(pos, &state);
        if (length == 0) return 0;
        pos += length;

        ip += delta;
        if (func_length != 0 && ip > func_length) return 0;
        if (state > max_state + 1) return 0;
        if (!fh4_is_rva(func_rva + ip, 1)) return 0;
    }

    return 1;
}

// processes handler map in place, returns 1 if the map contains continuation addresses relative to function start
static int fh4_process_handler_map(uint_fast32_t map_rva, uint_fast32_t func_rva, int *error)
{
    uint32_t count, index, value, cont_count, cont_index;
    unsigned int length;
    uint_fast32_t pos;
    uint8_t *header;
    int relative, ret;

    *error = 0;
    relative = 0;

    length = fh4_read_unsigned(map_rva, &count);
    if (length == 0 || count > FH4_MAX_ENTRIES) return 0;
    pos = map_rva + length;

    for (index = 0; index < count; index++)
    {
        header = fh4_ptr(pos, 1);
        if (header == NULL) return relative;
        pos++;

        if (*header & HT4_ADJECTIVES)
        {
            length = fh4_read_unsigned(pos, &value);
            if (length == 0) return relative;
            pos += length;
        }
        if (*header & HT4_DISP_TYPE)
        {
            if ((ret = SR_add_rva_fixup(pos, FF_NOENTRY))) { *error = ret; return relative; }
            pos += 4;
        }
        if (*header & HT4_DISP_CATCH_OBJ)
        {
            length = fh4_read_unsigned(pos, &value);
            if (length == 0) return relative;
            pos += length;
        }

        // catch funclet
        if ((ret = SR_add_rva_fixup(pos, 0))) { *error = ret; return relative; }
        pos += 4;

        cont_count = (*header >> HT4_CONT_ADDR_SHIFT) & 3;
        if (cont_count == 3) cont_count = 0;

        for (cont_index = 0; cont_index < cont_count; cont_index++)
        {
            if (*header & HT4_CONT_IS_RVA)
            {
                if ((ret = SR_add_rva_fixup(pos, 0))) { *error = ret; return relative; }
                pos += 4;
            }
            else
            {
                uint_fast32_t SecNum, RelAdr;

                length = fh4_read_unsigned(pos, &value);
                if (length == 0) return relative;
                pos += length;

                // continuation address is code entry (it isn't reachable otherwise)
                if (SR_get_section_reladr_exact(ImageBase + func_rva + value, &SecNum, &RelAdr) && section[SecNum].type == ST_CODE)
                {
                    section_entry_list_Insert(SecNum, RelAdr);
                }

                relative = 1;
            }
        }
    }

    return relative;
}

// processes C++ exception handling data version 4, returns 1 if the data was recognized
// data_rva - address of language specific handler data (int32 image relative address of FuncInfo4)
static int fh4_add_info(uint_fast32_t unwind_rva, uint_fast32_t data_rva, uint_fast32_t func_rva, int handler_kind)
{
    if (fh4_num_infos >= fh4_max_infos)
    {
        fh4_info *new_infos;

        fh4_max_infos = (fh4_max_infos == 0)?256:(2 * fh4_max_infos);
        new_infos = (fh4_info *) realloc(fh4_infos, fh4_max_infos * sizeof(fh4_info));
        if (new_infos == NULL)
        {
            fprintf(stderr, "Error: not enough memory\n");
            return -1;
        }
        fh4_infos = new_infos;
    }

    fh4_infos[fh4_num_infos].unwind_rva = unwind_rva;
    fh4_infos[fh4_num_infos].data_rva = data_rva;
    fh4_infos[fh4_num_infos].func_rva = func_rva;
    fh4_infos[fh4_num_infos].handler_kind = handler_kind;
    fh4_num_infos++;

    return 0;
}

int SR_fh4_process(uint_fast32_t unwind_info_rva, uint_fast32_t data_rva, const pdata_entry *function, int handler_kind)
{
    int handler_known;
    int32_t funcinfo_rva, value32;
    uint32_t value, num_unwind, num_try, index;
    uint8_t *header_ptr;
    uint8_t header;
    unsigned int length;
    uint_fast32_t pos, ipmap_field, unwind_rva, try_rva, func_rva, func_length;
    int ret, error;

    handler_known = (handler_kind != 0)?1:0;

    if (!fh4_read_int(data_rva, &funcinfo_rva)) return 0;
    if (!fh4_is_rva((uint32_t)funcinfo_rva, 0)) return 0;

    header_ptr = fh4_ptr((uint32_t)funcinfo_rva, 1);
    if (header_ptr == NULL) return 0;
    header = *header_ptr;

    if (header & FH4_RESERVED) return 0;

    func_rva = (function != NULL)?function->begin:0;
    func_length = (function != NULL)?(function->end - function->begin):0;

    // parse FuncInfo4
    pos = (uint32_t)funcinfo_rva + 1;
    if (header & FH4_BBT)
    {
        length = fh4_read_unsigned(pos, &value);
        if (length == 0) return 0;
        pos += length;
    }

    unwind_rva = 0;
    num_unwind = 0;
    if (header & FH4_UNWIND_MAP)
    {
        if (!fh4_read_int(pos, &value32)) return 0;
        unwind_rva = (uint32_t)value32;
        if (!fh4_is_rva(unwind_rva, 0)) return 0;
        if (fh4_read_unsigned(unwind_rva, &num_unwind) == 0 || num_unwind > FH4_MAX_ENTRIES) return 0;
        pos += 4;
    }

    try_rva = 0;
    num_try = 0;
    if (header & FH4_TRY_BLOCK_MAP)
    {
        if (!fh4_read_int(pos, &value32)) return 0;
        try_rva = (uint32_t)value32;
        if (!fh4_is_rva(try_rva, 0)) return 0;
        if (fh4_read_unsigned(try_rva, &num_try) == 0 || num_try > FH4_MAX_ENTRIES) return 0;
        pos += 4;
    }

    ipmap_field = pos;
    if (!fh4_read_int(pos, &value32)) return 0;
    pos += 4;

    if (value32 != 0)
    {
        if (!fh4_is_rva((uint32_t)value32, 0)) return 0;

        if (header & FH4_IS_SEPARATED)
        {
            uint32_t num_segments;
            uint_fast32_t seg_pos;
            int32_t seg_start, seg_map;

            if ((length = fh4_read_unsigned((uint32_t)value32, &num_segments)) == 0 || num_segments > FH4_MAX_ENTRIES) return 0;
            seg_pos = (uint32_t)value32 + length;
            for (index = 0; index < num_segments; index++)
            {
                if (!fh4_read_int(seg_pos, &seg_start) || !fh4_read_int(seg_pos + 4, &seg_map)) return 0;
                if (!fh4_is_rva((uint32_t)seg_start, 1)) return 0;
                if (seg_map != 0 && !fh4_check_ip_map((uint32_t)seg_map, (uint32_t)seg_start, 0, num_unwind)) return 0;
                seg_pos += 8;
            }
        }
        else
        {
            if (!fh4_check_ip_map((uint32_t)value32, func_rva, func_length, num_unwind)) return 0;
        }
    }
    else if (!handler_known && !(header & (FH4_UNWIND_MAP | FH4_TRY_BLOCK_MAP)))
    {
        // without known handler, don't accept data without any map
        return 0;
    }

    if (header & FH4_IS_CATCH)
    {
        length = fh4_read_unsigned(pos, &value);
        if (length == 0) return 0;
        pos += length;
    }

    // the data is valid FuncInfo4

    fh4_funcinfos++;

    if ((ret = fh4_add_info(unwind_info_rva, data_rva, func_rva, handler_kind))) return ret;

    // reference to FuncInfo4
    if ((ret = SR_add_rva_fixup(data_rva, FF_NOENTRY))) return ret;

    // unwind map (in place)
    if (unwind_rva != 0)
    {
        if ((ret = SR_add_rva_fixup((uint32_t)funcinfo_rva + 1 + ((header & FH4_BBT)?fh4_read_unsigned((uint32_t)funcinfo_rva + 1, &value):0), FF_NOENTRY))) return ret;

        pos = unwind_rva + fh4_read_unsigned(unwind_rva, &value);
        for (index = 0; index < num_unwind; index++)
        {
            uint32_t type;

            length = fh4_read_unsigned(pos, &value);
            if (length == 0) break;
            pos += length;

            type = value & 3;
            if (type == 1 || type == 2)
            {
                // destructor and object
                if ((ret = SR_add_rva_fixup(pos, 0))) return ret;
                pos += 4;
                length = fh4_read_unsigned(pos, &value);
                if (length == 0) break;
                pos += length;
            }
            else if (type == 3)
            {
                // unwind funclet
                if ((ret = SR_add_rva_fixup(pos, 0))) return ret;
                pos += 4;
            }
        }
    }

    // try block map (in place) and handler maps
    if (try_rva != 0)
    {
        uint_fast32_t try_field;

        try_field = (uint32_t)funcinfo_rva + 1;
        if (header & FH4_BBT) try_field += fh4_read_unsigned(try_field, &value);
        if (header & FH4_UNWIND_MAP) try_field += 4;
        if ((ret = SR_add_rva_fixup(try_field, FF_NOENTRY))) return ret;

        pos = try_rva + fh4_read_unsigned(try_rva, &value);
        for (index = 0; index < num_try; index++)
        {
            int32_t handler_rva;
            int item;

            // tryLow, tryHigh, catchHigh
            for (item = 0; item < 3; item++)
            {
                length = fh4_read_unsigned(pos, &value);
                if (length == 0) break;
                pos += length;
            }
            if (item < 3) break;

            if (!fh4_read_int(pos, &handler_rva)) break;

            if (handler_rva != 0 && fh4_is_rva((uint32_t)handler_rva, 0))
            {
                if (fh4_process_handler_map((uint32_t)handler_rva, func_rva, &error))
                {
                    // continuation addresses relative to function start - the map will be generated again
                    if ((ret = fh4_add_task(pos, (uint32_t)handler_rva, func_rva, 1))) return ret;
                }
                else
                {
                    if ((ret = SR_add_rva_fixup(pos, FF_NOENTRY))) return ret;
                }
                if (error) return error;
            }

            pos += 4;
        }
    }

    // IP to state map(s) - will be generated again
    if (value32 != 0)
    {
        if (header & FH4_IS_SEPARATED)
        {
            uint32_t num_segments;
            uint_fast32_t seg_pos;
            int32_t seg_start, seg_map;

            if ((ret = SR_add_rva_fixup(ipmap_field, FF_NOENTRY))) return ret;

            seg_pos = (uint32_t)value32 + fh4_read_unsigned((uint32_t)value32, &num_segments);
            for (index = 0; index < num_segments; index++)
            {
                if (!fh4_read_int(seg_pos, &seg_start) || !fh4_read_int(seg_pos + 4, &seg_map)) break;

                if ((ret = SR_add_rva_fixup(seg_pos, FF_NOENTRY))) return ret;
                if (seg_map != 0)
                {
                    if ((ret = fh4_add_task(seg_pos + 4, (uint32_t)seg_map, (uint32_t)seg_start, 0))) return ret;
                }

                seg_pos += 8;
            }
        }
        else
        {
            if ((ret = fh4_add_task(ipmap_field, (uint32_t)value32, func_rva, 0))) return ret;
        }
    }

    return 1;
}

// label for address (rva), the label is set in output
static int fh4_label(char *buf, uint_fast32_t rva, int require_instruction)
{
    uint_fast32_t SecNum, RelAdr;
    output_data *output;

    if (!SR_get_section_reladr_exact(ImageBase + rva, &SecNum, &RelAdr)) return 0;

    output = section_output_list_FindEntryEqual(SecNum, RelAdr);
    if (output == NULL) return 0;
    if (require_instruction && output->type != OT_INSTRUCTION) return 0;

    output->has_label = 1;
    SR_get_label(buf, ImageBase + rva);

    return 1;
}

// changes int32 at given address to image relative address of label
static int fh4_set_reference(uint_fast32_t field_rva, const char *label)
{
    uint_fast32_t SecNum, RelAdr;
    output_data *output;
    fixup_data *fixup;
    char buf[256];

    if (!SR_get_section_reladr_exact(ImageBase + field_rva, &SecNum, &RelAdr)) return -1;

    fixup = section_fixup_list_FindEntryEqual(SecNum, RelAdr);
    output = section_output_list_FindEntryEqual(SecNum, RelAdr);

    if (fixup == NULL || output == NULL || output->len != 3)
    {
        output = SR_disassemble_offset_init_output(SecNum, RelAdr, 4);
        if (output == NULL) return -1;
    }

    sprintf(buf, "dd RVA(%s)", label);

    if (output->str != NULL) free(output->str);
    output->str = strdup(buf);
    output->type = OT_OFFSET;

    return 0;
}

// generates IP to state map again, returns 1 on success
static int fh4_generate_ip_map(const fh4_map_task *task, const char *map_label)
{
    uint32_t count, delta, state, index, ip, prev_ip;
    unsigned int length, length2;
    uint_fast32_t pos;
    char buf[512], label1[64], label2[64];
    size_t saved_length;

    saved_length = fh4_data_length;

    length = fh4_read_unsigned(task->map_rva, &count);
    if (length == 0) return 0;

    sprintf(buf, "align 4, db 0\n%s:\n", map_label);
    if (fh4_append(buf)) return 0;
    if (fh4_append_bytes(task->map_rva, length)) return 0;
    pos = task->map_rva + length;

    if (!fh4_label(label1, task->func_rva, 1)) goto FAILED;

    ip = 0;
    prev_ip = 0;
    for (index = 0; index < count; index++)
    {
        length = fh4_read_unsigned(pos, &delta);
        if (length == 0) goto FAILED;
        length2 = fh4_read_unsigned(pos + length, &state);
        if (length2 == 0) goto FAILED;

        ip += delta;

        if (!fh4_label(label2, task->func_rva + ip, 0)) goto FAILED;
        if (index == 0) SR_get_label(label1, ImageBase + task->func_rva);
        else SR_get_label(label1, ImageBase + task->func_rva + prev_ip);

        // ip offset delta (compressed unsigned integer with length 4)
        sprintf(buf, "dd (((%s - %s) << 4) | 7)\n", label2, label1);
        if (fh4_append(buf)) return 0;

        // state
        if (fh4_append_bytes(pos + length, length2)) return 0;

        pos += length + length2;
        prev_ip = ip;
    }

    return 1;

FAILED:
    // remove generated text
    fh4_data_length = saved_length;
    if (fh4_data != NULL) fh4_data[fh4_data_length] = 0;
    return 0;
}

// generates handler map again (with continuation addresses as label differences), returns 1 on success
static int fh4_generate_handler_map(const fh4_map_task *task, const char *map_label)
{
    uint32_t count, index, value, cont_count, cont_index;
    unsigned int length;
    uint_fast32_t pos, SecNum, RelAdr;
    int32_t value32;
    uint8_t *header;
    char buf[512], label1[256], label2[64];
    size_t saved_length;

    saved_length = fh4_data_length;

    length = fh4_read_unsigned(task->map_rva, &count);
    if (length == 0) return 0;

    sprintf(buf, "align 4, db 0\n%s:\n", map_label);
    if (fh4_append(buf)) return 0;
    if (fh4_append_bytes(task->map_rva, length)) return 0;
    pos = task->map_rva + length;

    for (index = 0; index < count; index++)
    {
        header = fh4_ptr(pos, 1);
        if (header == NULL) goto FAILED;
        if (fh4_append_bytes(pos, 1)) return 0;
        pos++;

        if (*header & HT4_ADJECTIVES)
        {
            length = fh4_read_unsigned(pos, &value);
            if (length == 0) goto FAILED;
            if (fh4_append_bytes(pos, length)) return 0;
            pos += length;
        }
        if (*header & HT4_DISP_TYPE)
        {
            if (!fh4_read_int(pos, &value32)) goto FAILED;
            if (value32 == 0)
            {
                strcpy(buf, "dd 0\n");
            }
            else
            {
                if (!SR_get_section_reladr(ImageBase + (uint32_t)value32, &SecNum, &RelAdr)) goto FAILED;
                SR_get_label_expr(label1, SecNum, RelAdr, 1);
                sprintf(buf, "dd RVA(%s)\n", label1);
            }
            if (fh4_append(buf)) return 0;
            pos += 4;
        }
        if (*header & HT4_DISP_CATCH_OBJ)
        {
            length = fh4_read_unsigned(pos, &value);
            if (length == 0) goto FAILED;
            if (fh4_append_bytes(pos, length)) return 0;
            pos += length;
        }

        // catch funclet
        if (!fh4_read_int(pos, &value32)) goto FAILED;
        if (!fh4_label(label2, (uint32_t)value32, 1)) goto FAILED;
        sprintf(buf, "dd RVA(%s)\n", label2);
        if (fh4_append(buf)) return 0;
        pos += 4;

        cont_count = (*header >> HT4_CONT_ADDR_SHIFT) & 3;
        if (cont_count == 3) cont_count = 0;

        for (cont_index = 0; cont_index < cont_count; cont_index++)
        {
            if (*header & HT4_CONT_IS_RVA)
            {
                if (!fh4_read_int(pos, &value32)) goto FAILED;
                if (!fh4_label(label2, (uint32_t)value32, 1)) goto FAILED;
                sprintf(buf, "dd RVA(%s)\n", label2);
                pos += 4;
            }
            else
            {
                length = fh4_read_unsigned(pos, &value);
                if (length == 0) goto FAILED;
                if (!fh4_label(label2, task->func_rva + value, 1)) goto FAILED;
                if (!fh4_label(label1, task->func_rva, 1)) goto FAILED;
                sprintf(buf, "dd (((%s - %s) << 4) | 7)\n", label2, label1);
                pos += length;
            }
            if (fh4_append(buf)) return 0;
        }
    }

    return 1;

FAILED:
    fh4_data_length = saved_length;
    if (fh4_data != NULL) fh4_data[fh4_data_length] = 0;
    return 0;
}


/* copies of the data for functions which share unwind info (identical data folded by linker) */

// does the handler map contain continuation addresses relative to function start ?
static int fh4_handler_map_has_relative(uint_fast32_t map_rva)
{
    uint32_t count, index, value, cont_count, cont_index;
    unsigned int length;
    uint_fast32_t pos;
    uint8_t *header;

    length = fh4_read_unsigned(map_rva, &count);
    if (length == 0 || count > FH4_MAX_ENTRIES) return 0;
    pos = map_rva + length;

    for (index = 0; index < count; index++)
    {
        header = fh4_ptr(pos, 1);
        if (header == NULL) return 0;
        pos++;

        if (*header & HT4_ADJECTIVES)
        {
            if ((length = fh4_read_unsigned(pos, &value)) == 0) return 0;
            pos += length;
        }
        if (*header & HT4_DISP_TYPE) pos += 4;
        if (*header & HT4_DISP_CATCH_OBJ)
        {
            if ((length = fh4_read_unsigned(pos, &value)) == 0) return 0;
            pos += length;
        }
        pos += 4;

        cont_count = (*header >> HT4_CONT_ADDR_SHIFT) & 3;
        if (cont_count == 3) cont_count = 0;
        if (cont_count != 0 && !(*header & HT4_CONT_IS_RVA)) return 1;

        for (cont_index = 0; cont_index < cont_count; cont_index++) pos += 4;
    }

    return 0;
}

// label of original data (address with label in output)
static int fh4_data_label(char *buf, uint_fast32_t rva)
{
    uint_fast32_t SecNum, RelAdr;

    if (!SR_get_section_reladr(ImageBase + rva, &SecNum, &RelAdr)) return 0;
    SR_get_label_expr(buf, SecNum, RelAdr, 1);
    return 1;
}

// writes offset in function (unwind info prolog offset) as label difference, or original value
static int fh4_append_code_offset(uint_fast32_t func_rva, unsigned int value)
{
    char buf[160], label1[64], label2[64];

    if (value != 0 && fh4_label(label2, func_rva + value, 1) && fh4_label(label1, func_rva, 1))
    {
        sprintf(buf, "db %s - %s\n", label2, label1);
    }
    else
    {
        sprintf(buf, "db 0x%x\n", value);
    }

    return fh4_append(buf);
}

static int fh4_generate_unwind_info_copy(const fh4_info *info, uint_fast32_t func_rva, const char *label, const char *funcinfo_label)
{
    uint8_t *ui, *codes;
    unsigned int count, slot, num_slots, extra, index;
    int symbolic;
    uint32_t handler_rva, gs_value;
    char buf[640], handler_label[256];

    ui = fh4_ptr(info->unwind_rva, 4);
    if (ui == NULL) return 0;
    count = ui[2];
    codes = fh4_ptr(info->unwind_rva + 4, 2 * ((count + 1) & ~1));
    if (codes == NULL) return 0;

    // epilog unwind codes (version 2) are not converted
    symbolic = ((ui[0] & 7) == 1)?1:0;
    if ((ui[0] & 7) == 2)
    {
        symbolic = 1;
        for (slot = 0; slot < count; slot++)
        {
            if ((codes[2 * slot + 1] & 0x0f) == 6) symbolic = 0;
        }
    }

    if (!fh4_read_int(info->data_rva - 4, (int32_t *)&handler_rva)) return 0;
    if (!fh4_data_label(handler_label, handler_rva)) return 0;

    sprintf(buf, "align 4, db 0\n%s:\ndb 0x%x\n", label, ui[0]);
    if (fh4_append(buf)) return 0;

    if (symbolic)
    {
        if (fh4_append_code_offset(func_rva, ui[1])) return 0;
    }
    else
    {
        if (fh4_append_bytes(info->unwind_rva + 1, 1)) return 0;
    }
    if (fh4_append_bytes(info->unwind_rva + 2, 2)) return 0;

    for (slot = 0; slot < count; slot += num_slots)
    {
        switch (codes[2 * slot + 1] & 0x0f)
        {
            case 1: num_slots = ((codes[2 * slot + 1] >> 4) == 0)?2:3; break;
            case 4: case 8: num_slots = 2; break;
            case 5: case 9: num_slots = 3; break;
            default: num_slots = 1; break;
        }
        if (slot + num_slots > count) num_slots = count - slot;

        if (symbolic)
        {
            if (fh4_append_code_offset(func_rva, codes[2 * slot])) return 0;
            if (fh4_append_bytes(info->unwind_rva + 4 + 2 * slot + 1, 2 * num_slots - 1)) return 0;
        }
        else
        {
            if (fh4_append_bytes(info->unwind_rva + 4 + 2 * slot, 2 * num_slots)) return 0;
        }
    }
    if (count & 1)
    {
        if (fh4_append_bytes(info->unwind_rva + 4 + 2 * count, 2)) return 0;
    }

    sprintf(buf, "dd RVA(%s)\ndd RVA(%s)\n", handler_label, funcinfo_label);
    if (fh4_append(buf)) return 0;

    // GS data (__GSHandlerCheck_EH4) - 4 or 12 bytes, copied when the handler is unknown
    extra = 0;
    if (info->handler_kind == 2 || info->handler_kind == 0)
    {
        if (fh4_read_int(info->data_rva + 4, (int32_t *)&gs_value))
        {
            extra = (gs_value & 4)?12:4;
            for (index = extra; index != 0; index--)
            {
                if (fh4_ptr(info->data_rva + 4, index) != NULL) break;
            }
            extra = index;
        }
    }
    if (extra != 0)
    {
        if (fh4_append_bytes(info->data_rva + 4, extra)) return 0;
    }

    return 1;
}

static int fh4_generate_function_copy(const fh4_info *info, const pdata_entry *function)
{
    int32_t funcinfo_rva, value32;
    uint8_t header;
    uint32_t value, num_try, index;
    unsigned int length;
    uint_fast32_t pos, try_rva, ip_rva;
    char ui_label[96], fi_label[96], ip_label[96], tm_label[96], buf[512], label[256];
    size_t saved_length;
    int try_copy;

    saved_length = fh4_data_length;

    if (!fh4_read_int(info->data_rva, &funcinfo_rva)) return 0;
    header = *fh4_ptr((uint32_t)funcinfo_rva, 1);

    // separated code is not supported
    if (header & FH4_IS_SEPARATED) return 0;

    sprintf(ui_label, "srw_eh4_ui_%" PRIX64, ImageBase + function->begin);
    sprintf(fi_label, "srw_eh4_fi_%" PRIX64, ImageBase + function->begin);
    sprintf(ip_label, "srw_eh4_ipc_%" PRIX64, ImageBase + function->begin);
    sprintf(tm_label, "srw_eh4_tm_%" PRIX64, ImageBase + function->begin);

    // locate fields of FuncInfo4
    pos = (uint32_t)funcinfo_rva + 1;
    if (header & FH4_BBT) pos += fh4_read_unsigned(pos, &value);
    if (header & FH4_UNWIND_MAP) pos += 4;
    try_rva = 0;
    if (header & FH4_TRY_BLOCK_MAP)
    {
        if (!fh4_read_int(pos, &value32)) return 0;
        try_rva = (uint32_t)value32;
        pos += 4;
    }
    if (!fh4_read_int(pos, &value32)) return 0;
    ip_rva = (uint32_t)value32;

    // IP to state map relative to this function
    if (ip_rva != 0)
    {
        fh4_map_task task;

        task.field_rva = 0;
        task.map_rva = ip_rva;
        task.func_rva = function->begin;
        task.is_handler_map = 0;
        task.status = 0;
        if (!fh4_generate_ip_map(&task, ip_label)) goto FAILED;
    }

    // try block map (only when the handler maps contain continuation addresses relative to function start)
    try_copy = 0;
    if (try_rva != 0)
    {
        if ((length = fh4_read_unsigned(try_rva, &num_try)) == 0) goto FAILED;

        pos = try_rva + length;
        for (index = 0; index < num_try; index++)
        {
            int item;

            for (item = 0; item < 3; item++)
            {
                if ((length = fh4_read_unsigned(pos, &value)) == 0) goto FAILED;
                pos += length;
            }
            if (!fh4_read_int(pos, &value32)) goto FAILED;
            pos += 4;

            if (value32 != 0 && fh4_handler_map_has_relative((uint32_t)value32))
            {
                fh4_map_task task;

                task.field_rva = 0;
                task.map_rva = (uint32_t)value32;
                task.func_rva = function->begin;
                task.is_handler_map = 1;
                task.status = 0;

                sprintf(label, "srw_eh4_hmc_%" PRIX64 "_%u", ImageBase + function->begin, (unsigned int)index);
                if (!fh4_generate_handler_map(&task, label)) goto FAILED;
                try_copy = 1;
            }
        }

        if (try_copy)
        {
            pos = try_rva;
            length = fh4_read_unsigned(pos, &num_try);
            sprintf(buf, "align 4, db 0\n%s:\n", tm_label);
            if (fh4_append(buf)) return 0;
            if (fh4_append_bytes(pos, length)) return 0;
            pos += length;

            for (index = 0; index < num_try; index++)
            {
                int item;
                uint_fast32_t start;

                start = pos;
                for (item = 0; item < 3; item++)
                {
                    pos += fh4_read_unsigned(pos, &value);
                }
                if (fh4_append_bytes(start, pos - start)) return 0;

                fh4_read_int(pos, &value32);
                pos += 4;

                if (value32 == 0)
                {
                    strcpy(buf, "dd 0\n");
                }
                else if (fh4_handler_map_has_relative((uint32_t)value32))
                {
                    sprintf(buf, "dd RVA(srw_eh4_hmc_%" PRIX64 "_%u)\n", ImageBase + function->begin, (unsigned int)index);
                }
                else
                {
                    if (!fh4_data_label(label, (uint32_t)value32)) goto FAILED;
                    sprintf(buf, "dd RVA(%s)\n", label);
                }
                if (fh4_append(buf)) return 0;
            }
        }
    }

    // FuncInfo4
    sprintf(buf, "align 4, db 0\n%s:\n", fi_label);
    if (fh4_append(buf)) return 0;
    pos = (uint32_t)funcinfo_rva;
    if (fh4_append_bytes(pos, 1)) return 0;
    pos++;
    if (header & FH4_BBT)
    {
        length = fh4_read_unsigned(pos, &value);
        if (fh4_append_bytes(pos, length)) return 0;
        pos += length;
    }
    if (header & FH4_UNWIND_MAP)
    {
        if (!fh4_read_int(pos, &value32) || !fh4_data_label(label, (uint32_t)value32)) goto FAILED;
        sprintf(buf, "dd RVA(%s)\n", label);
        if (fh4_append(buf)) return 0;
        pos += 4;
    }
    if (header & FH4_TRY_BLOCK_MAP)
    {
        if (try_copy)
        {
            sprintf(buf, "dd RVA(%s)\n", tm_label);
        }
        else
        {
            if (!fh4_data_label(label, (uint32_t)try_rva)) goto FAILED;
            sprintf(buf, "dd RVA(%s)\n", label);
        }
        if (fh4_append(buf)) return 0;
        pos += 4;
    }
    if (ip_rva != 0)
    {
        sprintf(buf, "dd RVA(%s)\n", ip_label);
    }
    else
    {
        strcpy(buf, "dd 0\n");
    }
    if (fh4_append(buf)) return 0;
    pos += 4;
    if (header & FH4_IS_CATCH)
    {
        length = fh4_read_unsigned(pos, &value);
        if (fh4_append_bytes(pos, length)) return 0;
    }

    // unwind info
    if (!fh4_generate_unwind_info_copy(info, function->begin, ui_label, fi_label)) goto FAILED;

    // exception directory entry of the function uses the copy
    if (fh4_set_reference(section[function->psec].virtaddr + function->pofs + 8, ui_label)) return 0;

    return 1;

FAILED:
    fh4_data_length = saved_length;
    if (fh4_data != NULL) fh4_data[fh4_data_length] = 0;
    return 0;
}

static int fh4_generate_copies(void)
{
    unsigned int index, index2;

    for (index = 0; index < fh4_num_infos; index++)
    {
        for (index2 = 0; index2 < pdata_count; index2++)
        {
            const pdata_entry *function;

            function = &(pdata_list[index2]);

            if (function->ignored) continue;
            if (function->unwind != fh4_infos[index].unwind_rva) continue;
            if (function->begin == fh4_infos[index].func_rva) continue;

            if (fh4_generate_function_copy(&(fh4_infos[index]), function))
            {
                fh4_copies++;
            }
            else
            {
                fh4_copies_failed++;
                fprintf(stderr, "Warning: unable to create copy of shared C++ exception handling data for function 0x%" PRIx64 "\n", ImageBase + function->begin);
            }
        }
    }

    return 0;
}

// generates IP to state maps and handler maps (after disassembly)
int SR_fh4_post_process(void)
{
    unsigned int index, index2;
    char map_label[96], buf[256];
    int ret, done;

    for (index = 0; index < fh4_num_tasks; index++)
    {
        fh4_map_task *task;

        task = &(fh4_tasks[index]);

        sprintf(map_label, "srw_eh4_%s_%" PRIX64 "_%" PRIX64, (task->is_handler_map)?"hm":"ip", ImageBase + task->map_rva, ImageBase + task->func_rva);

        // the same map with the same function was already generated ?
        for (index2 = 0; index2 < index; index2++)
        {
            if (fh4_tasks[index2].map_rva == task->map_rva &&
                fh4_tasks[index2].func_rva == task->func_rva &&
                fh4_tasks[index2].is_handler_map == task->is_handler_map
               )
            {
                break;
            }
        }

        if (index2 < index)
        {
            // already generated (or failed)
            done = (fh4_tasks[index2].status > 0)?1:0;
        }
        else
        {
            done = (task->is_handler_map)?fh4_generate_handler_map(task, map_label):fh4_generate_ip_map(task, map_label);

            fh4_tasks[index].status = (done)?1:-1;

            if (done)
            {
                fh4_maps_converted++;
            }
            else
            {
                fh4_maps_failed++;
                fprintf(stderr, "Warning: unable to convert C++ exception handling %s map: 0x%" PRIx64 " (function 0x%" PRIx64 ")\n", (task->is_handler_map)?"handler":"IP to state", ImageBase + task->map_rva, ImageBase + task->func_rva);
            }
        }

        if (done)
        {
            ret = fh4_set_reference(task->field_rva, map_label);
        }
        else
        {
            // keep reference to the original map
            uint_fast32_t SecNum, RelAdr;

            if (!SR_get_section_reladr_exact(ImageBase + task->map_rva, &SecNum, &RelAdr)) continue;
            SR_get_label_expr(buf, SecNum, RelAdr, 1);
            ret = fh4_set_reference(task->field_rva, buf);
        }
        if (ret) return ret;
    }

    ret = fh4_generate_copies();
    if (ret) return ret;

    if (fh4_funcinfos)
    {
        fprintf(stderr, "\tC++ exception handling data (version 4): %u\n", fh4_funcinfos);
        fprintf(stderr, "\tC++ exception handling maps generated again: %u\n", fh4_maps_converted);
        if (fh4_copies)
        {
            fprintf(stderr, "\tC++ exception handling data copied for functions with shared data: %u\n", fh4_copies);
        }
        if (fh4_copies_failed)
        {
            fprintf(stderr, "\tC++ exception handling data not copied for functions with shared data: %u\n", fh4_copies_failed);
        }
        if (fh4_maps_failed)
        {
            fprintf(stderr, "\tC++ exception handling maps not converted: %u\n", fh4_maps_failed);
        }
    }

    return 0;
}

const char *SR_fh4_get_data(void)
{
    return (fh4_data_length != 0)?fh4_data:NULL;
}

void SR_fh4_free(void)
{
    if (fh4_infos != NULL)
    {
        free(fh4_infos);
        fh4_infos = NULL;
    }
    fh4_num_infos = 0;
    fh4_max_infos = 0;

    if (fh4_tasks != NULL)
    {
        free(fh4_tasks);
        fh4_tasks = NULL;
    }
    fh4_num_tasks = 0;
    fh4_max_tasks = 0;

    if (fh4_data != NULL)
    {
        free(fh4_data);
        fh4_data = NULL;
    }
    fh4_data_length = 0;
    fh4_data_size = 0;
}
