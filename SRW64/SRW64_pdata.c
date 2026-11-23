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
 * Processing of the exception directory (.pdata) and related data:
 *  - functions from the exception directory are used as code entries
 *  - image relative addresses in exception directory, unwind info,
 *    SEH scope tables (__C_specific_handler) and C++ exception handling
 *    data (__CxxFrameHandler3) are converted to labels
 *  - prolog offsets in unwind info are converted to label differences
 *  - functions which load image base into register are found
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SR_defs.h"
#include "SR_vars.h"

#define UNW_FLAG_EHANDLER  0x01
#define UNW_FLAG_UHANDLER  0x02
#define UNW_FLAG_CHAININFO 0x04

#define MAX_EH_ENTRIES 65536

static unsigned int pdata_scope_tables = 0;
static unsigned int pdata_cxx_funcinfos = 0;
static unsigned int pdata_unknown_handler_data = 0;

#define MAX_UNKNOWN_HANDLERS 32
static uint_fast32_t unknown_handler_rva[MAX_UNKNOWN_HANDLERS];
static unsigned int unknown_handler_count[MAX_UNKNOWN_HANDLERS];
static unsigned int num_unknown_handlers = 0;


static int pdata_compare(const void *p1, const void *p2)
{
    const pdata_entry *e1 = (const pdata_entry *)p1;
    const pdata_entry *e2 = (const pdata_entry *)p2;

    if (e1->begin < e2->begin) return -1;
    if (e1->begin > e2->begin) return 1;
    return 0;
}

static int uint_compare(const void *p1, const void *p2)
{
    uint_fast32_t v1 = *(const uint_fast32_t *)p1;
    uint_fast32_t v2 = *(const uint_fast32_t *)p2;

    if (v1 < v2) return -1;
    if (v1 > v2) return 1;
    return 0;
}

const pdata_entry *SR_find_pdata(uint_fast32_t rva)
{
    unsigned int low, high, middle;

    if (pdata_count == 0) return NULL;

    low = 0;
    high = pdata_count;
    while (low < high)
    {
        middle = (low + high) / 2;
        if (pdata_list[middle].begin <= rva)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }

    // low is the index of the first entry with begin > rva
    if (low == 0) return NULL;
    if (rva < pdata_list[low - 1].end) return &(pdata_list[low - 1]);

    return NULL;
}

const pdata_entry *SR_find_pdata_begin(uint_fast32_t rva)
{
    const pdata_entry *entry;

    entry = SR_find_pdata(rva);
    if (entry != NULL && entry->begin == rva) return entry;

    return NULL;
}

static uint32_t read_dword(uint_fast32_t rva, int *valid)
{
    uint32_t *ptr;

    ptr = (uint32_t *) SR_rva_to_ptr(rva, 4);
    if (ptr == NULL)
    {
        *valid = 0;
        return 0;
    }

    return *ptr;
}

static int is_code_rva(uint_fast32_t rva)
{
    uint_fast32_t SecNum, RelAdr;

    if (!SR_get_section_reladr_exact(ImageBase + rva, &SecNum, &RelAdr)) return 0;

    return (section[SecNum].type == ST_CODE)?1:0;
}

static int is_image_rva(uint_fast32_t rva)
{
    uint_fast32_t SecNum, RelAdr;

    if (rva < FirstSectionRVA || rva >= SizeOfImage) return 0;

    return SR_get_section_reladr_exact(ImageBase + rva, &SecNum, &RelAdr);
}

// adds image relative fixup at given rva (value at rva is the target rva)
static int add_rva_fixup(uint_fast32_t src_rva, int flags)
{
    uint_fast32_t SourceObject, SourceOffset, TargetObject, TargetOffset;
    uint32_t value;
    fixup_data *fixup;
    int valid;

    valid = 1;
    value = read_dword(src_rva, &valid);
    if (!valid) return 0;

    if (value == 0) return 0;

    if (!SR_get_section_reladr_exact(ImageBase + src_rva, &SourceObject, &SourceOffset)) return 0;

    if ((section[SourceObject].type == ST_IDATA) ||
        (section[SourceObject].type == ST_EDATA) ||
        (section[SourceObject].type == ST_RELOC) ||
        (section[SourceObject].type == ST_RSRC)
       )
    {
        return 0;
    }

    if (SourceOffset + 4 > section[SourceObject].size) return 0;

    if (!SR_get_section_reladr(ImageBase + value, &TargetObject, &TargetOffset))
    {
        fprintf(stderr, "Warning: image relative address outside of image: 0x%" PRIx64 " -> 0x%x\n", ImageBase + src_rva, (unsigned int)value);
        return 0;
    }

    fixup = section_fixup_list_FindEntryEqual(SourceObject, SourceOffset);
    if (fixup == NULL)
    {
        // check overlapping fixups
        fixup = section_fixup_list_FindEntryLower(SourceObject, SourceOffset);
        if (fixup != NULL && fixup->sofs + FIXUP_SIZE(fixup->type) > SourceOffset)
        {
            fprintf(stderr, "Warning: image relative address overlaps relocation: 0x%" PRIx64 "\n", ImageBase + src_rva);
            return 0;
        }
        fixup = section_fixup_list_FindEntryHigher(SourceObject, SourceOffset);
        if (fixup != NULL && fixup->sofs < SourceOffset + 4)
        {
            fprintf(stderr, "Warning: image relative address overlaps relocation: 0x%" PRIx64 "\n", ImageBase + src_rva);
            return 0;
        }

        fixup = section_fixup_list_Insert(SourceObject, SourceOffset);
        if (fixup == NULL)
        {
            fprintf(stderr, "Error: error adding image relative fixup: 0x%" PRIx64 "\n", ImageBase + src_rva);
            return -1;
        }

        fixup->tofs = TargetOffset;
        fixup->tsec = TargetObject;
        fixup->type = FT_RVA32;
        fixup->flags = flags;
    }
    else
    {
        if (fixup->type != FT_RVA32 || fixup->tofs != (int_fast32_t)TargetOffset || fixup->tsec != TargetObject)
        {
            fprintf(stderr, "Warning: conflicting fixup at: 0x%" PRIx64 "\n", ImageBase + src_rva);
            return 0;
        }

        fixup->flags &= flags;
    }

    return 0;
}

// SEH scope table (__C_specific_handler, __GSHandlerCheck_SEH)
static int check_scope_table(uint_fast32_t rva, const pdata_entry *function)
{
    uint_fast32_t count, index, begin, end, handler, target;
    int valid;

    valid = 1;
    count = read_dword(rva, &valid);
    if (!valid || count == 0 || count > 256) return 0;

    for (index = 0; index < count; index++)
    {
        begin = read_dword(rva + 4 + 16 * index, &valid);
        end = read_dword(rva + 4 + 16 * index + 4, &valid);
        handler = read_dword(rva + 4 + 16 * index + 8, &valid);
        target = read_dword(rva + 4 + 16 * index + 12, &valid);

        if (!valid) return 0;

        // scope must be inside the function (or its chained parts)
        if (begin >= end) return 0;
        if (!is_code_rva(begin) || !is_code_rva(end - 1)) return 0;
        if (function != NULL)
        {
            const pdata_entry *scope_function;

            scope_function = SR_find_pdata(begin);
            if (scope_function == NULL || scope_function->primary != function->primary) return 0;
        }

        // handler is filter function, finally function, or constant (EXCEPTION_EXECUTE_HANDLER = 1, ...)
        if (handler > 0xff && !is_code_rva(handler)) return 0;
        // target is exception handler address or 0 (finally)
        if (target != 0 && !is_code_rva(target)) return 0;
    }

    return 1;
}

static int process_scope_table(uint_fast32_t rva)
{
    uint_fast32_t count, index, handler;
    int valid, ret;

    valid = 1;
    count = read_dword(rva, &valid);

    for (index = 0; index < count; index++)
    {
        ret = add_rva_fixup(rva + 4 + 16 * index, FF_NOENTRY);
        if (ret) return ret;
        ret = add_rva_fixup(rva + 4 + 16 * index + 4, FF_NOENTRY);
        if (ret) return ret;

        handler = read_dword(rva + 4 + 16 * index + 8, &valid);
        if (handler > 0xff)
        {
            ret = add_rva_fixup(rva + 4 + 16 * index + 8, 0);
            if (ret) return ret;
        }

        ret = add_rva_fixup(rva + 4 + 16 * index + 12, 0);
        if (ret) return ret;
    }

    pdata_scope_tables++;

    return 0;
}

// C++ exception handling data (__CxxFrameHandler3, __GSHandlerCheck_EH)
static int is_cxx_funcinfo(uint_fast32_t rva)
{
    uint32_t magic;
    int valid;

    valid = 1;
    magic = read_dword(rva, &valid);
    if (!valid) return 0;

    magic &= 0x1fffffff;

    return (magic == 0x19930520 || magic == 0x19930521 || magic == 0x19930522)?1:0;
}

static int process_cxx_funcinfo(uint_fast32_t rva)
{
    uint32_t magic, maxState, nTryBlocks, nIPMapEntries, dispUnwindMap, dispTryBlockMap, dispIPToStateMap, dispESTypeList;
    uint_fast32_t index, index2, nCatches, dispHandlerArray;
    fixup_data *fixup;
    uint_fast32_t SecNum, RelAdr;
    int valid, ret;

    // check if FuncInfo was already processed
    if (SR_get_section_reladr_exact(ImageBase + rva + 8, &SecNum, &RelAdr))
    {
        fixup = section_fixup_list_FindEntryEqual(SecNum, RelAdr);
        if (fixup != NULL && fixup->type == FT_RVA32) return 0;
    }

    valid = 1;
    magic = read_dword(rva, &valid) & 0x1fffffff;
    maxState = read_dword(rva + 4, &valid);
    dispUnwindMap = read_dword(rva + 8, &valid);
    nTryBlocks = read_dword(rva + 12, &valid);
    dispTryBlockMap = read_dword(rva + 16, &valid);
    nIPMapEntries = read_dword(rva + 20, &valid);
    dispIPToStateMap = read_dword(rva + 24, &valid);
    dispESTypeList = (magic >= 0x19930521)?read_dword(rva + 32, &valid):0;

    if (!valid || maxState > MAX_EH_ENTRIES || nTryBlocks > MAX_EH_ENTRIES || nIPMapEntries > MAX_EH_ENTRIES) return 0;
    if ((dispUnwindMap != 0 && !is_image_rva(dispUnwindMap)) ||
        (dispTryBlockMap != 0 && !is_image_rva(dispTryBlockMap)) ||
        (dispIPToStateMap != 0 && !is_image_rva(dispIPToStateMap)) ||
        (dispESTypeList != 0 && !is_image_rva(dispESTypeList))
       )
    {
        return 0;
    }

    pdata_cxx_funcinfos++;

    // FuncInfo
    if ((ret = add_rva_fixup(rva + 8, FF_NOENTRY))) return ret;
    if ((ret = add_rva_fixup(rva + 16, FF_NOENTRY))) return ret;
    if ((ret = add_rva_fixup(rva + 24, FF_NOENTRY))) return ret;
    if (magic >= 0x19930521)
    {
        if ((ret = add_rva_fixup(rva + 32, FF_NOENTRY))) return ret;
    }

    // UnwindMapEntry: toState, action
    if (dispUnwindMap != 0)
    {
        for (index = 0; index < maxState; index++)
        {
            if ((ret = add_rva_fixup(dispUnwindMap + 8 * index + 4, 0))) return ret;
        }
    }

    // TryBlockMapEntry: tryLow, tryHigh, catchHigh, nCatches, dispHandlerArray
    if (dispTryBlockMap != 0)
    {
        for (index = 0; index < nTryBlocks; index++)
        {
            nCatches = read_dword(dispTryBlockMap + 20 * index + 12, &valid);
            dispHandlerArray = read_dword(dispTryBlockMap + 20 * index + 16, &valid);
            if (!valid || nCatches > MAX_EH_ENTRIES) break;

            if ((ret = add_rva_fixup(dispTryBlockMap + 20 * index + 16, FF_NOENTRY))) return ret;

            // HandlerType: adjectives, dispType, dispCatchObj, dispOfHandler, dispFrame
            if (dispHandlerArray != 0 && is_image_rva(dispHandlerArray))
            {
                for (index2 = 0; index2 < nCatches; index2++)
                {
                    if ((ret = add_rva_fixup(dispHandlerArray + 20 * index2 + 4, FF_NOENTRY))) return ret;
                    if ((ret = add_rva_fixup(dispHandlerArray + 20 * index2 + 12, 0))) return ret;
                }
            }
        }
    }

    // IptoStateMapEntry: Ip, State
    if (dispIPToStateMap != 0)
    {
        for (index = 0; index < nIPMapEntries; index++)
        {
            if ((ret = add_rva_fixup(dispIPToStateMap + 8 * index, FF_NOENTRY))) return ret;
        }
    }

    // ESTypeList: nCount, dispTypeArray
    if (dispESTypeList != 0)
    {
        uint_fast32_t nCount, dispTypeArray;

        nCount = read_dword(dispESTypeList, &valid);
        dispTypeArray = read_dword(dispESTypeList + 4, &valid);

        if (valid && nCount <= MAX_EH_ENTRIES)
        {
            if ((ret = add_rva_fixup(dispESTypeList + 4, FF_NOENTRY))) return ret;

            if (dispTypeArray != 0 && is_image_rva(dispTypeArray))
            {
                for (index = 0; index < nCount; index++)
                {
                    if ((ret = add_rva_fixup(dispTypeArray + 20 * index + 4, FF_NOENTRY))) return ret;
                    if ((ret = add_rva_fixup(dispTypeArray + 20 * index + 12, 0))) return ret;
                }
            }
        }
    }

    return 0;
}

static int process_unwind_info(uint_fast32_t unwind, const pdata_entry *function)
{
    uint8_t *info;
    uint_fast32_t version, flags, count, after;
    uint32_t value;
    int valid, ret;

    info = (uint8_t *) SR_rva_to_ptr(unwind, 4);
    if (info == NULL)
    {
        fprintf(stderr, "Warning: unwind info is not inside any section: 0x%x\n", (unsigned int)unwind);
        return 0;
    }

    version = info[0] & 7;
    flags = info[0] >> 3;
    count = info[2];

    if (version != 1 && version != 2)
    {
        fprintf(stderr, "Warning: unknown unwind info version: 0x%x - %i\n", (unsigned int)unwind, (int)version);
        return 0;
    }

    after = unwind + 4 + 2 * ((count + 1) & ~1);

    if (flags & UNW_FLAG_CHAININFO)
    {
        // chained RUNTIME_FUNCTION
        if ((ret = add_rva_fixup(after, 0))) return ret;
        if ((ret = add_rva_fixup(after + 4, FF_NOENTRY))) return ret;
        if ((ret = add_rva_fixup(after + 8, FF_NOENTRY))) return ret;
    }
    else if (flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER))
    {
        valid = 1;
        value = read_dword(after, &valid);
        if (!valid) return 0;

        if (!is_code_rva(value))
        {
            fprintf(stderr, "Warning: exception handler is not in code section: 0x%x - 0x%x\n", (unsigned int)unwind, (unsigned int)value);
            return 0;
        }

        // exception handler
        if ((ret = add_rva_fixup(after, 0))) return ret;

        // language specific handler data
        value = read_dword(after + 4, &valid);
        if (!valid) return 0;

        if (is_image_rva(value) && is_cxx_funcinfo(value))
        {
            if ((ret = add_rva_fixup(after + 4, FF_NOENTRY))) return ret;
            if ((ret = process_cxx_funcinfo(value))) return ret;
        }
        else if (check_scope_table(after + 4, function))
        {
            if ((ret = process_scope_table(after + 4))) return ret;
        }
        else
        {
            uint_fast32_t index;

            // handler data is not recognized (e.g. __GSHandlerCheck data doesn't contain addresses)
            pdata_unknown_handler_data++;

            value = read_dword(after, &valid);
            for (index = 0; index < num_unknown_handlers; index++)
            {
                if (unknown_handler_rva[index] == value) break;
            }
            if (index < num_unknown_handlers)
            {
                unknown_handler_count[index]++;
            }
            else if (num_unknown_handlers < MAX_UNKNOWN_HANDLERS)
            {
                unknown_handler_rva[num_unknown_handlers] = value;
                unknown_handler_count[num_unknown_handlers] = 1;
                num_unknown_handlers++;
            }
        }
    }

    return 0;
}

static uint_fast32_t find_primary_function(uint_fast32_t begin, uint_fast32_t unwind)
{
    uint8_t *info;
    uint_fast32_t depth, count;
    int valid;

    for (depth = 0; depth < 32; depth++)
    {
        if (unwind & 1)
        {
            // unwind data points to another RUNTIME_FUNCTION
            valid = 1;
            begin = read_dword(unwind & ~1, &valid);
            unwind = read_dword((unwind & ~1) + 8, &valid);
            if (!valid) break;
            continue;
        }

        info = (uint8_t *) SR_rva_to_ptr(unwind, 4);
        if (info == NULL) break;

        if (!((info[0] >> 3) & UNW_FLAG_CHAININFO)) break;

        count = info[2];
        valid = 1;
        begin = read_dword(unwind + 4 + 2 * ((count + 1) & ~1), &valid);
        unwind = read_dword(unwind + 4 + 2 * ((count + 1) & ~1) + 8, &valid);
        if (!valid) break;
    }

    return begin;
}

// name of exception handler (imported function called through thunk) or its label
static void SR_handler_name(char *name, uint_fast32_t rva)
{
    uint8_t *code;
    int32_t disp;
    extrn_data *extrn;

    code = (uint8_t *) SR_rva_to_ptr(rva, 6);
    if (code != NULL && code[0] == 0xff && code[1] == 0x25)
    {
        // jmp [rip+disp32]
        memcpy(&disp, &(code[2]), 4);
        extrn = SR_disassemble_find_import(ImageBase + rva + 6 + (int64_t)disp);
        if (extrn != NULL)
        {
            strcpy(name, extrn->proc);
            return;
        }
    }

    SR_get_label(name, ImageBase + rva);
}

// image relative addresses in delay load import descriptors (used by __delayLoadHelper2)
int SR_process_delay_imports(void)
{
    uint_fast32_t descriptor, field, lookup;
    uint32_t attributes, name, lookup_rva;
    uint64_t *entry;
    unsigned int count;
    int valid, ret;

    if (DelayImportDirRVA == 0 || DelayImportDirSize == 0) return 0;

    count = 0;
    for (descriptor = DelayImportDirRVA; descriptor + 32 <= DelayImportDirRVA + DelayImportDirSize; descriptor += 32)
    {
        valid = 1;
        attributes = read_dword(descriptor, &valid);
        name = read_dword(descriptor + 4, &valid);
        if (!valid || name == 0) break;

        // old format with virtual addresses is handled by relocations
        if (!(attributes & 1)) continue;

        // rvaDLLName, rvaHmod, rvaIAT, rvaINT, rvaBoundIAT, rvaUnloadIAT
        for (field = 1; field <= 6; field++)
        {
            if ((ret = add_rva_fixup(descriptor + 4 * field, FF_NOENTRY))) return ret;
        }

        // import name table: RVAs of IMAGE_IMPORT_BY_NAME or ordinals
        lookup_rva = read_dword(descriptor + 16, &valid);
        if (lookup_rva != 0)
        {
            for (lookup = lookup_rva; ; lookup += 8)
            {
                entry = (uint64_t *) SR_rva_to_ptr(lookup, 8);
                if (entry == NULL || *entry == 0) break;

                if (!(*entry & UINT64_C(0x8000000000000000)))
                {
                    if ((ret = add_rva_fixup(lookup, FF_NOENTRY))) return ret;
                }
            }
        }

        count++;
    }

    fprintf(stderr, "\tDelay load import descriptors: %u\n", count);

    return 0;
}

int SR_process_pdata(void)
{
    uint32_t *table;
    uint_fast32_t index, num_entries, SecNum, RelAdr;
    output_data *output;
    int ret;

    if (ExceptionDirRVA == 0 || ExceptionDirSize < 12) return 0;

    num_entries = ExceptionDirSize / 12;
    table = (uint32_t *) SR_rva_to_ptr(ExceptionDirRVA, num_entries * 12);
    if (table == NULL)
    {
        fprintf(stderr, "Warning: exception directory is not inside any section\n");
        return 0;
    }

    pdata_list = (pdata_entry *) malloc(num_entries * sizeof(pdata_entry));
    if (pdata_list == NULL)
    {
        fprintf(stderr, "Error: not enough memory\n");
        return -1;
    }

    // read entries
    pdata_count = 0;
    for (index = 0; index < num_entries; index++)
    {
        pdata_entry *entry;

        entry = &(pdata_list[pdata_count]);

        entry->begin = table[3 * index];
        entry->end = table[3 * index + 1];
        entry->unwind = table[3 * index + 2];

        if (entry->begin == 0 && entry->end == 0) continue;

        if (entry->begin >= entry->end || !is_code_rva(entry->begin))
        {
            fprintf(stderr, "Warning: invalid exception directory entry: %i - 0x%x - 0x%x\n", (unsigned int)index, (unsigned int)entry->begin, (unsigned int)entry->end);
            continue;
        }

        if (!SR_get_section_reladr_exact(ImageBase + ExceptionDirRVA + 12 * index, &SecNum, &RelAdr))
        {
            continue;
        }

        entry->psec = SecNum;
        entry->pofs = RelAdr;
        entry->primary = find_primary_function(entry->begin, entry->unwind);

        SR_get_section_reladr_exact(ImageBase + entry->begin, &SecNum, &RelAdr);
        entry->ignored = SR_is_ignored(SecNum, RelAdr);

        pdata_count++;
    }

    qsort(pdata_list, pdata_count, sizeof(pdata_entry), pdata_compare);

    fprintf(stderr, "\tFunctions in exception directory: %u\n", pdata_count);

    // functions are code entries
    if (use_pdata)
    {
        for (index = 0; index < pdata_count; index++)
        {
            if (pdata_list[index].ignored) continue;

            if (!SR_get_section_reladr_exact(ImageBase + pdata_list[index].begin, &SecNum, &RelAdr)) continue;
            if (section[SecNum].type != ST_CODE) continue;
            if (section_nocode_list_CanFindEntryEqual(SecNum, RelAdr)) continue;

            section_entry_list_Insert(SecNum, RelAdr);

            output = section_output_list_FindEntryEqual(SecNum, RelAdr);
            if (output != NULL)
            {
                output->has_label = 1;
            }
        }
    }

    if (!convert_exception_data) return 0;

    // exception directory entries
    for (index = 0; index < pdata_count; index++)
    {
        pdata_entry *entry;
        uint_fast32_t entry_rva;

        entry = &(pdata_list[index]);
        entry_rva = section[entry->psec].virtaddr + entry->pofs;

        if (entry->ignored)
        {
            // function is not part of output - remove it from exception directory
            output = SR_disassemble_offset_init_output(entry->psec, entry->pofs, 12);
            if (output != NULL)
            {
                output->str = strdup("dd 0, 0, 0 ; ignored function");
            }
            continue;
        }

        if ((ret = add_rva_fixup(entry_rva, 0))) return ret;
        if ((ret = add_rva_fixup(entry_rva + 4, FF_NOENTRY))) return ret;
        if ((ret = add_rva_fixup(entry_rva + 8, FF_NOENTRY))) return ret;
    }

    // unwind info (every unwind info only once)
    {
        uint_fast32_t *unwind_list, num_unwind;

        unwind_list = (uint_fast32_t *) malloc(2 * pdata_count * sizeof(uint_fast32_t) + 1);
        if (unwind_list == NULL)
        {
            fprintf(stderr, "Error: not enough memory\n");
            return -1;
        }

        num_unwind = 0;
        for (index = 0; index < pdata_count; index++)
        {
            if (pdata_list[index].ignored) continue;
            if (pdata_list[index].unwind & 1) continue;

            unwind_list[2 * num_unwind] = pdata_list[index].unwind;
            unwind_list[2 * num_unwind + 1] = index;
            num_unwind++;
        }

        qsort(unwind_list, num_unwind, 2 * sizeof(uint_fast32_t), uint_compare);

        for (index = 0; index < num_unwind; index++)
        {
            if (index != 0 && unwind_list[2 * index] == unwind_list[2 * (index - 1)]) continue;

            ret = process_unwind_info(unwind_list[2 * index], &(pdata_list[unwind_list[2 * index + 1]]));
            if (ret)
            {
                free(unwind_list);
                return ret;
            }
        }

        free(unwind_list);
    }

    fprintf(stderr, "\tSEH scope tables: %u\n", pdata_scope_tables);
    fprintf(stderr, "\tC++ exception handling data: %u\n", pdata_cxx_funcinfos);
    if (pdata_unknown_handler_data)
    {
        fprintf(stderr, "\tUnrecognized language specific handler data: %u\n", pdata_unknown_handler_data);
        for (index = 0; index < num_unknown_handlers; index++)
        {
            char name[256];

            SR_handler_name(name, unknown_handler_rva[index]);
            fprintf(stderr, "\t\thandler %s: %u\n", name, unknown_handler_count[index]);
        }
    }

    return 0;
}


static int unwind_offset_to_label(uint_fast32_t byte_rva, uint_fast32_t function_rva, unsigned int value, output_data *function_output)
{
    uint_fast32_t tsec, tofs, bsec, bofs;
    output_data *target_output, *byte_output;
    char cbuf[128], cbuf2[64];

    if (value == 0) return 1;

    if (!SR_get_section_reladr_exact(ImageBase + function_rva + value, &tsec, &tofs)) return 0;
    target_output = section_output_list_FindEntryEqual(tsec, tofs);
    if (target_output == NULL || target_output->type != OT_INSTRUCTION) return 0;

    if (!SR_get_section_reladr_exact(ImageBase + byte_rva, &bsec, &bofs)) return 0;
    byte_output = section_output_list_FindEntryEqual(bsec, bofs);
    if (byte_output == NULL || byte_output->type != OT_UNKNOWN || byte_output->str != NULL) return 0;

    target_output->has_label = 1;
    function_output->has_label = 1;

    SR_get_label(cbuf2, ImageBase + function_rva);
    SR_get_label(cbuf, ImageBase + function_rva + value);
    sprintf(&(cbuf[strlen(cbuf)]), " - %s", cbuf2);

    byte_output->str = (char *) malloc(strlen(cbuf) + 4);
    if (byte_output->str != NULL)
    {
        strcpy(byte_output->str, "db ");
        strcat(byte_output->str, cbuf);
    }

    return 1;
}

// convert prolog offsets in unwind info to label differences (instruction lengths can change after recompilation)
int SR_process_unwind_offsets(void)
{
    uint_fast32_t index, unwind, fsec, fofs, slot, count, num_slots;
    unsigned int converted, failed;
    uint8_t *info;
    output_data *function_output;
    uint_fast32_t *unwind_list, num_unwind;

    if (pdata_count == 0) return 0;

    unwind_list = (uint_fast32_t *) malloc(2 * pdata_count * sizeof(uint_fast32_t) + 1);
    if (unwind_list == NULL)
    {
        fprintf(stderr, "Error: not enough memory\n");
        return -1;
    }

    num_unwind = 0;
    for (index = 0; index < pdata_count; index++)
    {
        if (pdata_list[index].ignored) continue;
        if (pdata_list[index].unwind & 1) continue;

        unwind_list[2 * num_unwind] = pdata_list[index].unwind;
        unwind_list[2 * num_unwind + 1] = index;
        num_unwind++;
    }

    qsort(unwind_list, num_unwind, 2 * sizeof(uint_fast32_t), uint_compare);

    converted = 0;
    failed = 0;
    for (index = 0; index < num_unwind; index++)
    {
        const pdata_entry *entry;
        uint8_t *codes;
        int ok, version;

        unwind = unwind_list[2 * index];
        if (index != 0 && unwind == unwind_list[2 * (index - 1)]) continue;

        // (shared unwind info has identical prologs, so the first function is used)
        entry = &(pdata_list[unwind_list[2 * index + 1]]);

        info = (uint8_t *) SR_rva_to_ptr(unwind, 4);
        if (info == NULL) continue;

        version = info[0] & 7;
        count = info[2];

        if (version != 1 && version != 2) continue;
        if (info[1] == 0 && count == 0) continue;

        codes = (uint8_t *) SR_rva_to_ptr(unwind + 4, 2 * count);
        if (codes == NULL) continue;

        // epilog unwind codes (version 2) are relative to function end - keep unwind info unchanged
        if (version == 2)
        {
            for (slot = 0; slot < count; slot++)
            {
                if ((codes[2 * slot + 1] & 0x0f) == 6) break;
            }
            if (slot < count) continue;
        }

        if (!SR_get_section_reladr_exact(ImageBase + entry->begin, &fsec, &fofs)) continue;
        function_output = section_output_list_FindEntryEqual(fsec, fofs);
        if (function_output == NULL || function_output->type != OT_INSTRUCTION)
        {
            failed++;
            continue;
        }

        ok = unwind_offset_to_label(unwind + 1, entry->begin, info[1], function_output);

        for (slot = 0; slot < count; slot += num_slots)
        {
            switch (codes[2 * slot + 1] & 0x0f)
            {
                case 1: // UWOP_ALLOC_LARGE
                    num_slots = ((codes[2 * slot + 1] >> 4) == 0)?2:3;
                    break;
                case 4: // UWOP_SAVE_NONVOL
                case 8: // UWOP_SAVE_XMM128
                    num_slots = 2;
                    break;
                case 5: // UWOP_SAVE_NONVOL_FAR
                case 9: // UWOP_SAVE_XMM128_FAR
                    num_slots = 3;
                    break;
                default:
                    num_slots = 1;
                    break;
            }

            if (!unwind_offset_to_label(unwind + 4 + 2 * slot, entry->begin, codes[2 * slot], function_output))
            {
                ok = 0;
            }
        }

        if (ok) converted++;
        else failed++;
    }

    free(unwind_list);

    fprintf(stderr, "\tUnwind info with converted prolog offsets: %u\n", converted);
    if (failed)
    {
        fprintf(stderr, "\tUnwind info with unconverted prolog offsets: %u\n", failed);
    }

    return 0;
}

// find functions which load image base into register (lea reg, [rip+__ImageBase])
int SR_prescan_image_base(void)
{
    unsigned int Entry, count;
    uint_fast32_t ofs, psec, pofs;
    uint8_t *adr;
    int32_t disp;
    const pdata_entry *function;
    int *value, reg;

    count = 0;
    for (Entry = 0; Entry < num_sections; Entry++)
    {
        if (section[Entry].type != ST_CODE) continue;

        adr = section[Entry].adr;
        for (ofs = 0; ofs + 7 <= section[Entry].size_in_file; ofs++)
        {
            // REX.W 8D /r with RIP-relative addressing
            if ((adr[ofs] & 0xf8) != 0x48) continue;
            if (adr[ofs + 1] != 0x8d) continue;
            if ((adr[ofs + 2] & 0xc7) != 0x05) continue;

            memcpy(&disp, &(adr[ofs + 3]), 4);
            if (section[Entry].start + ofs + 7 + (int64_t)disp != ImageBase) continue;

            reg = ((adr[ofs + 2] >> 3) & 7) | ((adr[ofs] & 4)?8:0);

            function = SR_find_pdata(section[Entry].virtaddr + ofs);
            if (function == NULL) continue;

            if (!SR_get_section_reladr_exact(ImageBase + function->primary, &psec, &pofs)) continue;

            value = section_ibreg_list_FindEntryEqual(psec, pofs);
            if (value == NULL)
            {
                value = section_ibreg_list_Insert(psec, pofs);
                if (value == NULL) return -1;
                *value = 0;
            }
            *value |= 1 << reg;

            count++;
        }
    }

    fprintf(stderr, "\tImage base loads found: %u\n", count);

    return 0;
}
