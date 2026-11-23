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
 * Conversion of x86-64 instructions to relocatable NASM source:
 *  - RIP-relative addresses are converted to labels ([rel loc_XXXXXXXX])
 *  - calls/jumps through import address table are converted to calls/jumps to imported functions
 *  - relocated values (DIR64, HIGHLOW) are converted to labels
 *  - image relative addressing ([reg_with_image_base + index*scale + RVA]) is converted to RVA(label)
 *  - jump tables which are not relocated (image relative or relative to table start) are converted
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SR_defs.h"
#include "SR_vars.h"

extern const char* ud_reg_tab[];

#define REG_RAX 0
#define REG_RCX 1
#define REG_RDX 2
#define REG_RBX 3
#define REG_RSP 4
#define REG_RBP 5
#define REG_RSI 6
#define REG_RDI 7
#define REG_R8  8
#define REG_R9  9
#define REG_R10 10
#define REG_R11 11

#define BIT(r) (1 << (r))

#define VOLATILE_MASK    (BIT(REG_RAX) | BIT(REG_RCX) | BIT(REG_RDX) | BIT(REG_R8) | BIT(REG_R9) | BIT(REG_R10) | BIT(REG_R11))
#define NONVOLATILE_MASK (0xffff & ~VOLATILE_MASK & ~BIT(REG_RSP))
#define STRING_MASK      (BIT(REG_RAX) | BIT(REG_RCX) | BIT(REG_RSI) | BIT(REG_RDI))

#define MAX_TABLE_ENTRIES 4096

// disassembly hint for code entry: registers containing image base (bits 0-15), jump table bound + 1 (bits 16-30)
#define HINT_MAKE(mask, bound) (((mask) & 0xffff) | ((((bound) < 0)?0:((((bound) + 1) > 0x7fff)?0x7fff:((bound) + 1))) << 16))
#define HINT_MASK(h) ((h) & 0xffff)
#define HINT_BOUND(h) (((((unsigned int)(h)) >> 16) & 0x7fff) - 1)

typedef enum _memory_kind_ {
    MEMK_NONE    = 0,   // no conversion
    MEMK_LABEL   = 1,   // RIP-relative address of label
    MEMK_IMPORT  = 2,   // RIP-relative address of import address table entry
    MEMK_EXTERN  = 3,   // RIP-relative address of external procedure
    MEMK_HEADER  = 4,   // RIP-relative address in PE header (image base)
    MEMK_RVA     = 5,   // image relative displacement
    MEMK_OUTSIDE = 6,   // RIP-relative address outside of image
    MEMK_SEGMENT = 7    // removed segment prefix
} memory_kind;

typedef struct _memory_info_ {
    int opnum;              // index of memory operand (-1 = none)
    memory_kind kind;       // kind of conversion
    uint64_t target;        // target address
    extrn_data *extrn;      // import or external procedure
} memory_info;

typedef enum _table_kind_ {
    TBL_NONE        = 0,
    TBL_RVA_LOADED  = 1,    // mov r32, [imagebase + index*4 + RVA(table)]
    TBL_RVA_READY   = 2,    // add r64, imagebase
    TBL_DIFF_LOADED = 3,    // movsxd r64, [table + index*4]
    TBL_DIFF_READY  = 4,    // add r64, table
    TBL_ABS_READY   = 5,    // jmp [index*8 + table] (table with absolute addresses)
    TBL_ABS_LOADED  = 6     // mov r64, [index*8 + table]
} table_kind;

typedef struct _run_state_ {
    int ib_mask;                // registers containing image base
    int lea_mask;               // registers containing address loaded by lea reg, [rip+X]
    uint64_t lea_target[16];    // addresses loaded by lea reg, [rip+X]
    int cmp_valid;              // previous instruction was cmp with immediate value
    int64_t cmp_imm;            // immediate value of previous cmp instruction
    int bound;                  // number of jump table entries (from cmp/ja), -1 = unknown
    int bound_mask;             // registers containing the index checked by cmp/ja
    int cmp_reg;                // register compared by previous cmp instruction (-1 = not register)
    table_kind tbl_kind;        // pending jump table
    int tbl_reg;                // register containing value from jump table
    int tbl_basereg;            // register containing image base / table address
    int tbl_basereg2;           // register containing copy of table address (-1 = none)
    uint64_t tbl_load_address;  // address of instruction which loads value from jump table
    uint64_t tbl_address;       // jump table address
    int tbl_bound;              // number of jump table entries
    int tbl_uses_bt;            // jump table index was loaded from byte table
    int bt_valid;               // byte index table was loaded
    int bt_reg;                 // register containing value from byte index table
    uint64_t bt_address;        // byte index table address
    int bt_bound;               // number of byte index table entries
} run_state;


static int reg_index(enum ud_type reg)
{
    if (reg >= UD_R_AL && reg <= UD_R_R15B)
    {
        int index = reg - UD_R_AL;
        if (index < 4) return index;    // al, cl, dl, bl
        if (index < 8) return index - 4; // ah, ch, dh, bh
        return index - 4;               // spl, bpl, sil, dil, r8b - r15b
    }
    if (reg >= UD_R_AX && reg <= UD_R_R15W) return reg - UD_R_AX;
    if (reg >= UD_R_EAX && reg <= UD_R_R15D) return reg - UD_R_EAX;
    if (reg >= UD_R_RAX && reg <= UD_R_R15) return reg - UD_R_RAX;
    return -1;
}

static int reg64_index(enum ud_type reg)
{
    if (reg >= UD_R_RAX && reg <= UD_R_R15) return reg - UD_R_RAX;
    return -1;
}

static void SR_init_run_state(unsigned int Entry, uint_fast32_t offset, run_state *state)
{
    int *hint, *facts;
    const pdata_entry *function;
    uint_fast32_t rva, psec, pofs;

    memset(state, 0, sizeof(run_state));
    state->bound = -1;
    state->bound_mask = 0;
    state->cmp_reg = -1;
    state->tbl_kind = TBL_NONE;

    hint = section_hint_list_FindEntryEqual(Entry, offset);
    if (hint != NULL)
    {
        state->ib_mask = HINT_MASK(*hint);
        state->bound = HINT_BOUND(*hint);
        // register containing the index is not known
        state->bound_mask = (state->bound >= 0)?0xffff:0;
        return;
    }

    rva = section[Entry].virtaddr + offset;
    function = SR_find_pdata(rva);
    if (function == NULL) return;

    // function start - no register contains image base
    if (function->begin == rva && function->primary == rva) return;

    // unknown state - assume that non-volatile registers loaded with image base in the function still contain image base
    if (SR_get_section_reladr_exact(ImageBase + function->primary, &psec, &pofs))
    {
        facts = section_ibreg_list_FindEntryEqual(psec, pofs);
        if (facts != NULL)
        {
            state->ib_mask = *facts & NONVOLATILE_MASK;
        }
    }
}

static void SR_add_jump_target(unsigned int Entry, uint64_t address, const run_state *state, int bound)
{
    uint_fast32_t SecNum, RelAdr;
    int *hint, mask, old_bound;

    SR_disassemble_add_address(Entry, address);

    if (!SR_get_section_reladr_exact(address, &SecNum, &RelAdr)) return;
    if (section[SecNum].type != ST_CODE) return;

    hint = section_hint_list_FindEntryEqual(SecNum, RelAdr);
    if (hint == NULL)
    {
        hint = section_hint_list_Insert(SecNum, RelAdr);
        if (hint == NULL) return;
        *hint = HINT_MAKE(state->ib_mask, bound);
    }
    else
    {
        mask = HINT_MASK(*hint) & state->ib_mask;
        old_bound = HINT_BOUND(*hint);
        *hint = HINT_MAKE(mask, (old_bound >= 0)?old_bound:bound);
    }
}

// memory operand without displacement (or with zero displacement - base r13/rbp needs displacement in encoding)
static int SR_no_displacement(const struct ud_operand *op)
{
    switch (op->offset)
    {
        case 0: return 1;
        case 8: return (op->lval.sbyte == 0)?1:0;
        case 16: return (op->lval.sword == 0)?1:0;
        case 32: return (op->lval.sdword == 0)?1:0;
        default: return 0;
    }
}

// register contains address of jump table (the base register of the table load or a copy of it - mov rsi, rcx; movsxd rcx, [rcx+rdx*4]; add rcx, rsi)
static int SR_is_table_base(const run_state *state, int old_lea_mask, int reg)
{
    if (reg < 0 || reg == state->tbl_reg) return 0;
    if (reg == state->tbl_basereg) return 1;
    if (state->tbl_address != 0)
    {
        return ((old_lea_mask & BIT(reg)) && state->lea_target[reg] == state->tbl_address)?1:0;
    }

    // address of table isn't known (it's searched for at the jump, validated by entries)
    return (InputFormat == INPUT_ELF)?1:0;
}

// bound of table index (from cmp/ja), if the index register was checked
// (state before the instruction - the instruction can overwrite the index register: movsxd rax, [rdx+rax*4])
static int SR_index_bound(int bound, int bound_mask, const struct ud_operand *op)
{
    int index;

    if (bound < 0) return -1;

    index = reg64_index(op->index);
    if (index < 0)
    {
        // jmp [index*8 + table] - index in base register
        index = reg64_index(op->base);
    }
    if (index < 0 || !(bound_mask & BIT(index))) return -1;

    return bound;
}

// registers written by current instruction
static int SR_written_registers_ud(const ud_t *u)
{
    const struct ud_operand *op0, *op1;
    int mask, reg;

    op0 = &(u->operand[0]);
    op1 = &(u->operand[1]);
    mask = 0;

    switch (u->mnemonic)
    {
        // instructions which don't write to first operand
        case UD_Icmp:
        case UD_Itest:
        case UD_Ibt:
        case UD_Ipush:
        case UD_Ijmp:
        case UD_Iout:
        case UD_Iucomiss:
        case UD_Iucomisd:
        case UD_Icomiss:
        case UD_Icomisd:
        case UD_Iptest:
        case UD_Ivucomiss:
        case UD_Ivucomisd:
        case UD_Ivcomiss:
        case UD_Ivcomisd:
        case UD_Ivptest:
        case UD_Ivtestps:
        case UD_Ivtestpd:
        case UD_Inop:
            return 0;

        // instructions with implicit operands
        case UD_Icall:
            // (System V ABI: rsi and rdi are volatile too)
            return (InputFormat == INPUT_ELF)?(VOLATILE_MASK | BIT(REG_RSI) | BIT(REG_RDI)):VOLATILE_MASK;
        case UD_Isyscall:
            return BIT(REG_RAX) | BIT(REG_RCX) | BIT(REG_R11);
        case UD_Icpuid:
            return BIT(REG_RAX) | BIT(REG_RBX) | BIT(REG_RCX) | BIT(REG_RDX);
        case UD_Irdtsc:
        case UD_Irdpmc:
        case UD_Irdmsr:
        case UD_Ixgetbv:
        case UD_Icmpxchg8b:
        case UD_Icmpxchg16b:
            return BIT(REG_RAX) | BIT(REG_RDX);
        case UD_Irdtscp:
            return BIT(REG_RAX) | BIT(REG_RCX) | BIT(REG_RDX);
        case UD_Icdq:
        case UD_Icqo:
        case UD_Icwd:
            return BIT(REG_RDX);
        case UD_Icdqe:
        case UD_Icwde:
        case UD_Icbw:
        case UD_Ixlatb:
        case UD_Ilahf:
        case UD_Iin:
            return BIT(REG_RAX);
        case UD_Imul:
        case UD_Idiv:
        case UD_Iidiv:
            return BIT(REG_RAX) | BIT(REG_RDX);
        case UD_Iimul:
            if (op1->type == UD_NONE) return BIT(REG_RAX) | BIT(REG_RDX);
            break;
        case UD_Icmpxchg:
            mask = BIT(REG_RAX);
            break;
        case UD_Ixchg:
        case UD_Ixadd:
        case UD_Imulx:
            if (op1->type == UD_OP_REG)
            {
                reg = reg_index(op1->base);
                if (reg >= 0) mask |= BIT(reg);
            }
            break;
        case UD_Iloop:
        case UD_Iloope:
        case UD_Iloopne:
            return BIT(REG_RCX);
        case UD_Ileave:
        case UD_Ienter:
            return BIT(REG_RBP) | BIT(REG_RSP);
        case UD_Imovsb:
        case UD_Imovsw:
        case UD_Imovsq:
        case UD_Icmpsb:
        case UD_Icmpsw:
        case UD_Icmpsq:
        case UD_Istosb:
        case UD_Istosw:
        case UD_Istosd:
        case UD_Istosq:
        case UD_Ilodsb:
        case UD_Ilodsw:
        case UD_Ilodsd:
        case UD_Ilodsq:
        case UD_Iscasb:
        case UD_Iscasw:
        case UD_Iscasd:
        case UD_Iscasq:
        case UD_Iinsb:
        case UD_Iinsw:
        case UD_Iinsd:
        case UD_Ioutsb:
        case UD_Ioutsw:
        case UD_Ioutsd:
            return STRING_MASK;
        case UD_Imovsd:
        case UD_Icmpsd:
            // string instructions have no explicit operands (sse instructions have)
            if (op0->type == UD_NONE) return STRING_MASK;
            break;
        default:
            break;
    }

    if (op0->type == UD_OP_REG)
    {
        reg = reg_index(op0->base);
        if (reg >= 0) mask |= BIT(reg);
    }

    return mask;
}

static int SR_written_registers(void)
{
    return SR_written_registers_ud(&ud_obj);
}

static void SR_print_mem_disp(char *buf, const struct ud_operand *op, int sign)
{
    if (op->base == UD_NONE && op->index == UD_NONE)
    {
        uint64_t v;

        switch (op->offset)
        {
            case 16: v = op->lval.uword; break;
            case 32: v = op->lval.udword; break;
            case 64: v = op->lval.uqword; break;
            default: v = 0; break;
        }
        sprintf(buf, "%s0x%" PRIx64, (op->offset == 64)?"qword ":"", v);
    }
    else
    {
        int64_t v;

        switch (op->offset)
        {
            case 8 : v = op->lval.sbyte; break;
            case 16: v = op->lval.sword; break;
            case 32: v = op->lval.sdword; break;
            default: v = 0; break;
        }

        if (v < 0)
        {
            sprintf(buf, "-0x%" PRIx64, (uint64_t)(-v));
        }
        else if (v > 0)
        {
            sprintf(buf, "%s0x%" PRIx64, (sign)?"+":"", (uint64_t)v);
        }
        else
        {
            buf[0] = 0;
        }
    }
}

// memory operand text as printed by udis86
static void SR_mem_operand_text(char *buf, const struct ud_operand *op)
{
    char disp[64];
    char *ptr;

    ptr = buf;
    ptr += sprintf(ptr, "[");
    if (ud_obj.pfx_seg)
    {
        ptr += sprintf(ptr, "%s:", ud_reg_tab[ud_obj.pfx_seg - UD_R_AL]);
    }
    if (op->base)
    {
        ptr += sprintf(ptr, "%s", ud_reg_tab[op->base - UD_R_AL]);
    }
    if (op->index)
    {
        ptr += sprintf(ptr, "%s%s", (op->base != UD_NONE)?"+":"", ud_reg_tab[op->index - UD_R_AL]);
        if (op->scale)
        {
            ptr += sprintf(ptr, "*%d", op->scale);
        }
    }
    if (op->offset != 0)
    {
        SR_print_mem_disp(disp, op, (op->base != UD_NONE || op->index != UD_NONE)?1:0);
        ptr += sprintf(ptr, "%s", disp);
    }
    sprintf(ptr, "]");
}

// memory operand text with segment prefix (only fs, gs), base, index and given displacement
static void SR_mem_operand_text_new(char *buf, const struct ud_operand *op, const char *disp)
{
    char *ptr;

    ptr = buf;
    ptr += sprintf(ptr, "[");
    if (ud_obj.pfx_seg == UD_R_FS || ud_obj.pfx_seg == UD_R_GS)
    {
        ptr += sprintf(ptr, "%s:", ud_reg_tab[ud_obj.pfx_seg - UD_R_AL]);
    }
    if (op->base)
    {
        ptr += sprintf(ptr, "%s", ud_reg_tab[op->base - UD_R_AL]);
    }
    if (op->index)
    {
        ptr += sprintf(ptr, "%s%s", (op->base != UD_NONE)?"+":"", ud_reg_tab[op->index - UD_R_AL]);
        if (op->scale)
        {
            ptr += sprintf(ptr, "*%d", op->scale);
        }
    }
    if (disp != NULL && disp[0] != 0)
    {
        ptr += sprintf(ptr, "%s%s", (op->base != UD_NONE || op->index != UD_NONE)?"+":"", disp);
    }
    sprintf(ptr, "]");
}

// does the address look like start of function ? (valid instruction, aligned or after padding)
static int SR_looks_like_function_start(unsigned int sec, uint_fast32_t ofs)
{
    ud_t ud_tmp;
    uint8_t prev;

    if (ofs >= section[sec].size_in_file) return 0;

    if ((ofs & 15) != 0 && ofs != 0)
    {
        prev = section[sec].adr[ofs - 1];
        if (prev != 0xcc && prev != 0x90 && prev != 0xc3) return 0;
    }

    ud_init(&ud_tmp);
    ud_set_mode(&ud_tmp, 64);
    ud_set_vendor(&ud_tmp, UD_VENDOR_ANY);
    ud_set_input_buffer(&ud_tmp, &(section[sec].adr[ofs]), section[sec].size_in_file - ofs);
    ud_set_pc(&ud_tmp, section[sec].start + ofs);

    if (ud_decode(&ud_tmp) == 0) return 0;
    if (ud_tmp.mnemonic == UD_Iinvalid || ud_tmp.mnemonic == UD_Iint3) return 0;

    return 1;
}

// address of code is used (lea reg, [rip+X]) - is it a function ?
static void SR_check_code_reference(unsigned int sec, uint_fast32_t ofs)
{
    uint_fast32_t rva;
    const pdata_entry *function;
    int valid;

    if (section_fixup_list_FindEntryEqual(sec, ofs) != NULL) return;

    rva = section[sec].virtaddr + ofs;

    valid = section_code_list_CanFindEntryEqual(sec, ofs);
    if (!valid && use_pdata)
    {
        function = SR_find_pdata_begin(rva);
        if (function != NULL && function->primary == rva) valid = 1;

        // functions without unwind info (leaf functions) are not in exception directory
        if (!valid && function == NULL && pdata_count != 0 && SR_find_pdata(rva) == NULL && SR_looks_like_function_start(sec, ofs))
        {
            valid = 1;
        }
    }

    if (valid && section_nocode_list_CanFindEntryEqual(sec, ofs)) valid = 0;
    if (valid && SR_is_ignored(sec, ofs)) valid = 0;

    if (valid)
    {
        SR_disassemble_add_address(sec, section[sec].start + ofs);
    }
    else if (list_invalid_code_fixups && invalid_code_fixups_file != NULL)
    {
        char cbuf[64];

        SR_get_label(cbuf, section[sec].start + ofs);
        fprintf(invalid_code_fixups_file, "%s\n", cbuf);
    }
}

static int SR_is_rva_displacement(unsigned int Entry, uint_fast32_t offset, const struct ud_operand *op, const run_state *state)
{
    int *forced, base_reg, index_reg;
    uint32_t disp;
    uint_fast32_t SecNum, RelAdr;
    int valid;

    if (op->offset != 32) return 0;
    if (op->base == UD_NONE && op->index == UD_NONE) return 0;
    if (op->base == UD_R_RIP) return 0;

    disp = op->lval.udword;
    valid = (disp >= FirstSectionRVA && disp < SizeOfImage && SR_get_section_reladr_exact(ImageBase + disp, &SecNum, &RelAdr))?1:0;

    forced = section_rvadisp_list_FindEntryEqual(Entry, offset);
    if (forced != NULL)
    {
        return (*forced && valid)?1:0;
    }

    if (!detect_image_relative || !valid) return 0;

    base_reg = reg64_index(op->base);
    index_reg = reg64_index(op->index);

    if (base_reg >= 0 && (state->ib_mask & BIT(base_reg))) return 1;
    if (index_reg >= 0 && op->scale <= 1 && (state->ib_mask & BIT(index_reg))) return 1;

    return 0;
}

// converts memory operand, returns 1 if instruction text was changed, 0 if not, negative value on error
static int SR_convert_memory_operand(unsigned int Entry, uint_fast32_t offset, const char *ostr, char *dst, const run_state *state, memory_info *mi)
{
    const struct ud_operand *op;
    char orig[256], repl[512], expr[256], disp[300];
    const char *pos;
    uint_fast32_t sec, ofs;
    int index;

    mi->opnum = -1;
    mi->kind = MEMK_NONE;
    mi->target = 0;
    mi->extrn = NULL;

    for (index = 0; index < 4; index++)
    {
        if (ud_obj.operand[index].type == UD_OP_MEM) break;
    }
    if (index == 4) return 0;

    op = &(ud_obj.operand[index]);
    mi->opnum = index;

    if (op->base == UD_R_RIP)
    {
        mi->target = ud_obj.pc + (int64_t)op->lval.sdword;

        if (ud_obj.pfx_seg == UD_R_FS || ud_obj.pfx_seg == UD_R_GS)
        {
            fprintf(stderr, "Error: RIP-relative address with segment prefix: 0x%" PRIx64 " - %s\n", section[Entry].start + offset, ostr);
            return -60;
        }

        mi->extrn = SR_disassemble_find_import(mi->target);
        if (mi->extrn != NULL)
        {
            mi->kind = MEMK_IMPORT;
            if (InputFormat == INPUT_ELF)
            {
                // GOT entry of dynamically linked executable - GOT of new executable
                sprintf(expr, "%s wrt %s", mi->extrn->proc, (SR_elf_dyn_is_tls_got(mi->target))?"..gottpoff":"..gotpc");
            }
            else
            {
                sprintf(expr, "__imp_%s", mi->extrn->proc);
            }
        }
        else if (SR_get_section_reladr_exact(mi->target, &sec, &ofs))
        {
            extrn_data *extrn;

            extrn = section_extrn_list_FindEntryEqual(sec, ofs);
            if (extrn != NULL && !extrn->is_import)
            {
                mi->kind = MEMK_EXTERN;
                mi->extrn = extrn;
                extrn->is_used = 1;
                strcpy(expr, extrn->proc);
            }
            else
            {
                mi->kind = MEMK_LABEL;
                SR_get_label_expr(expr, sec, ofs, 1);

                if (ud_obj.mnemonic == UD_Ilea && section[sec].type == ST_CODE)
                {
                    SR_check_code_reference(sec, ofs);
                }
            }
        }
        else if (mi->target >= ImageBase && mi->target < ImageBase + SizeOfHeaders)
        {
            mi->kind = MEMK_HEADER;
            SR_get_label_expr(expr, SEC_HEADER, (int_fast64_t)(mi->target - ImageBase), 0);
        }
        else if (InputFormat == INPUT_ELF)
        {
            // address outside of sections (e.g. ELF header, undefined weak symbol with address 0) - original address (srw_abs_<address>)
            mi->kind = MEMK_OUTSIDE;
            SR_get_label(expr, mi->target);
        }
        else if (SR_get_section_reladr(mi->target, &sec, &ofs))
        {
            mi->kind = MEMK_LABEL;
            SR_get_label_expr(expr, sec, ofs, 1);
        }
        else
        {
            mi->kind = MEMK_OUTSIDE;
            fprintf(stderr, "Warning: RIP-relative address outside of image: 0x%" PRIx64 " - 0x%" PRIx64 "\n", section[Entry].start + offset, mi->target);
            SR_get_label_expr(expr, SEC_HEADER, (int_fast64_t)(mi->target - ImageBase), 0);
        }

        sprintf(repl, "[rel %s]", expr);
    }
    else if (SR_is_rva_displacement(Entry, offset, op, state))
    {
        mi->kind = MEMK_RVA;
        mi->target = ImageBase + op->lval.udword;

        SR_get_section_reladr_exact(mi->target, &sec, &ofs);
        SR_get_label_expr(expr, sec, ofs, 1);

        sprintf(disp, "RVA(%s)", expr);
        SR_mem_operand_text_new(repl, op, disp);

        if (rva_displacements_file != NULL)
        {
            char cbuf[64];

            SR_get_label(cbuf, section[Entry].start + offset);
            fprintf(rva_displacements_file, "%s,1 ; %s\n", cbuf, ostr);
        }
    }
    else if (ud_obj.pfx_seg == UD_R_ES || ud_obj.pfx_seg == UD_R_CS || ud_obj.pfx_seg == UD_R_SS || ud_obj.pfx_seg == UD_R_DS)
    {
        // segment prefixes es, cs, ss, ds are ignored in 64-bit mode
        mi->kind = MEMK_SEGMENT;

        if (op->offset != 0)
        {
            SR_print_mem_disp(expr, op, 0);
            if (expr[0] == '+') memmove(expr, expr + 1, strlen(expr));
        }
        else
        {
            expr[0] = 0;
        }

        if (expr[0] == '-')
        {
            // negative displacement
            char *ptr;

            SR_mem_operand_text_new(repl, op, NULL);
            ptr = &(repl[strlen(repl) - 1]);
            sprintf(ptr, "%s]", expr);
        }
        else
        {
            SR_mem_operand_text_new(repl, op, expr);
        }
    }
    else
    {
        return 0;
    }

    SR_mem_operand_text(orig, op);

    pos = strstr(ostr, orig);
    if (pos == NULL || strstr(pos + 1, orig) != NULL)
    {
        fprintf(stderr, "Error: unable to convert memory operand: 0x%" PRIx64 " - %s - %s\n", section[Entry].start + offset, ostr, orig);
        return -61;
    }

    memcpy(dst, ostr, pos - ostr);
    strcpy(&(dst[pos - ostr]), repl);
    strcat(dst, pos + strlen(orig));

    return 1;
}

// replaces hexadecimal value in instruction text by label, returns 0 on success
static int SR_replace_value_text(const char *ostr, char *dst, uint64_t value, const char *label)
{
    char cbuf[32];
    const char *str1, *str2;

    sprintf(cbuf, "0x%" PRIx64, value);

    str1 = strstr(ostr, cbuf);
    while (str1 != NULL)
    {
        str2 = str1 + strlen(cbuf);
        if ((*str2 >= '0' && *str2 <= '9') || (*str2 >= 'a' && *str2 <= 'f'))
        {
            str1 = strstr(str2, cbuf);
            continue;
        }
        break;
    }
    if (str1 == NULL) return 1;
    if (strstr(str1 + strlen(cbuf), cbuf) != NULL) return 2;

    memcpy(dst, ostr, str1 - ostr);
    strcpy(&(dst[str1 - ostr]), label);
    strcat(dst, str1 + strlen(cbuf));

    return 0;
}

// is the address a thunk (jmp [rip+IAT]) to imported function which doesn't return ?
static int SR_is_noret_thunk(uint64_t address)
{
    uint_fast32_t SecNum, RelAdr;
    int32_t disp;

    if (!SR_get_section_reladr_exact(address, &SecNum, &RelAdr)) return 0;
    if (section[SecNum].type != ST_CODE) return 0;
    if (RelAdr + 6 > section[SecNum].size) return 0;

    if (section[SecNum].adr[RelAdr] != 0xff || section[SecNum].adr[RelAdr + 1] != 0x25) return 0;

    memcpy(&disp, &(section[SecNum].adr[RelAdr + 2]), 4);

    return SR_disassemble_find_noret(address + 6 + (int64_t)disp);
}

// data flow analysis of table addresses in registers (lea reg, [rip+table]) in a function:
// values are propagated along fallthrough and direct jumps from the function start (all registers unknown),
// code which is reached only by jump tables (or not at all) is not analyzed
#define DF_UNKNOWN 0
#define DF_MAX_INSTRUCTIONS 1000000

enum { DFK_NEXT, DFK_JMP, DFK_JCC, DFK_STOP, DFK_INDIRECT };

// targets of converted jump tables (edges for data flow analysis)
static struct {
    unsigned int num, max;
    uint64_t *from, *to;
} jt_edges;

static void SR_add_jump_table_edge(uint64_t from, uint64_t to)
{
    if (jt_edges.num == jt_edges.max)
    {
        unsigned int max;
        uint64_t *new_from, *new_to;

        max = (jt_edges.max == 0)?4096:2 * jt_edges.max;
        new_from = (uint64_t *) realloc(jt_edges.from, max * sizeof(uint64_t));
        if (new_from == NULL) return;
        jt_edges.from = new_from;
        new_to = (uint64_t *) realloc(jt_edges.to, max * sizeof(uint64_t));
        if (new_to == NULL) return;
        jt_edges.to = new_to;
        jt_edges.max = max;
    }

    jt_edges.from[jt_edges.num] = from;
    jt_edges.to[jt_edges.num] = to;
    jt_edges.num++;
}

typedef struct {
    uint64_t address;
    uint64_t target;            // jump target / table address loaded by lea
    int written;                // written registers
    int8_t kind;
    int8_t lea_reg;             // lea r64, [rip+table]
    int8_t mov_dst;             // mov r64, r64
    int8_t mov_src;
} df_instruction;

static struct {
    uint64_t begin, end;        // analyzed function
    unsigned int num_edges;     // number of jump table edges in function
    unsigned int num;           // number of instructions
    df_instruction *insn;
    uint64_t *values;           // register values at instruction start (16 per instruction)
    uint8_t *reached;
} df_function;

static int SR_df_find(uint64_t address)
{
    int low, high, middle;

    low = 0;
    high = (int)df_function.num - 1;
    while (low <= high)
    {
        middle = (low + high) / 2;
        if (df_function.insn[middle].address == address) return middle;
        if (df_function.insn[middle].address < address) low = middle + 1; else high = middle - 1;
    }
    return -1;
}

// merges values into state of instruction (returns 1 if the state changed)
static int SR_df_merge(int index, const uint64_t *values)
{
    uint64_t *dst;
    int reg, changed;

    dst = &(df_function.values[16 * (unsigned int)index]);
    if (!df_function.reached[index])
    {
        memcpy(dst, values, 16 * sizeof(uint64_t));
        df_function.reached[index] = 1;
        return 1;
    }

    changed = 0;
    for (reg = 0; reg < 16; reg++)
    {
        if (dst[reg] != values[reg] && dst[reg] != DF_UNKNOWN)
        {
            dst[reg] = DF_UNKNOWN;
            changed = 1;
        }
    }
    return changed;
}

static int SR_df_analyze_function(uint64_t begin, uint64_t end)
{
    uint_fast32_t SecNum, RelAdr, TblSec, TblOfs;
    unsigned int max_num, length, top, *stack, num_edges, edge;
    uint8_t *on_stack;
    uint64_t address, values[16];
    df_instruction *insn;
    int index, reg, next;
    ud_t ud_df;

    num_edges = 0;
    for (edge = 0; edge < jt_edges.num; edge++)
    {
        if (jt_edges.from[edge] >= begin && jt_edges.from[edge] < end) num_edges++;
    }

    if (df_function.insn != NULL && df_function.begin == begin && df_function.end == end && df_function.num_edges == num_edges) return 1;

    free(df_function.insn);
    free(df_function.values);
    free(df_function.reached);
    memset(&df_function, 0, sizeof(df_function));

    if (!SR_get_section_reladr_exact(begin, &SecNum, &RelAdr) || section[SecNum].type != ST_CODE) return 0;
    if (end > section[SecNum].start + section[SecNum].size_in_file) end = section[SecNum].start + section[SecNum].size_in_file;
    if (end <= begin) return 0;

    // instructions (linear decoding)
    ud_init(&ud_df);
    ud_set_mode(&ud_df, 64);
    ud_set_vendor(&ud_df, UD_VENDOR_ANY);
    ud_set_input_buffer(&ud_df, &(section[SecNum].adr[RelAdr]), (size_t)(end - begin));
    ud_set_pc(&ud_df, begin);

    max_num = 0;
    address = begin;
    while (address < end && (length = ud_decode(&ud_df)) != 0)
    {
        if (ud_df.mnemonic == UD_Iinvalid) break;
        if (df_function.num >= DF_MAX_INSTRUCTIONS) return 0;

        if (df_function.num == max_num)
        {
            max_num = (max_num == 0)?1024:2 * max_num;
            insn = (df_instruction *) realloc(df_function.insn, max_num * sizeof(df_instruction));
            if (insn == NULL) return 0;
            df_function.insn = insn;
        }

        insn = &(df_function.insn[df_function.num]);
        df_function.num++;

        insn->address = address;
        insn->target = 0;
        insn->written = SR_written_registers_ud(&ud_df);
        insn->kind = DFK_NEXT;
        insn->lea_reg = -1;
        insn->mov_dst = -1;
        insn->mov_src = -1;

        switch (ud_df.mnemonic)
        {
            case UD_Ijmp:
                if (ud_df.operand[0].type == UD_OP_JIMM)
                {
                    insn->kind = DFK_JMP;
                    insn->target = ud_df.pc + (int64_t)((ud_df.operand[0].size == 32)?ud_df.operand[0].lval.sdword:ud_df.operand[0].lval.sbyte);
                }
                else
                {
                    // targets from converted jump tables
                    insn->kind = DFK_INDIRECT;
                }
                break;
            case UD_Ijo:
            case UD_Ijno:
            case UD_Ijb:
            case UD_Ijae:
            case UD_Ijz:
            case UD_Ijnz:
            case UD_Ijbe:
            case UD_Ija:
            case UD_Ijs:
            case UD_Ijns:
            case UD_Ijp:
            case UD_Ijnp:
            case UD_Ijl:
            case UD_Ijge:
            case UD_Ijle:
            case UD_Ijg:
            case UD_Ijcxz:
            case UD_Ijecxz:
            case UD_Ijrcxz:
            case UD_Iloop:
            case UD_Iloope:
            case UD_Iloopne:
                if (ud_df.operand[0].type == UD_OP_JIMM)
                {
                    insn->kind = DFK_JCC;
                    insn->target = ud_df.pc + (int64_t)((ud_df.operand[0].size == 32)?ud_df.operand[0].lval.sdword:ud_df.operand[0].lval.sbyte);
                }
                break;
            case UD_Iret:
            case UD_Iretf:
            case UD_Iud2:
            case UD_Ihlt:
            case UD_Iint3:
                insn->kind = DFK_STOP;
                break;
            case UD_Ilea:
                if (ud_df.operand[0].type == UD_OP_REG && ud_df.operand[0].size == 64 &&
                    ud_df.operand[1].type == UD_OP_MEM && ud_df.operand[1].base == UD_R_RIP && ud_df.operand[1].index == UD_NONE
                   )
                {
                    uint64_t target = ud_df.pc + (int64_t)ud_df.operand[1].lval.sdword;

                    if (SR_get_section_reladr_exact(target, &TblSec, &TblOfs) && section[TblSec].type != ST_CODE)
                    {
                        insn->lea_reg = (int8_t)reg64_index(ud_df.operand[0].base);
                        insn->target = target;
                    }
                }
                break;
            case UD_Imov:
                if (ud_df.operand[0].type == UD_OP_REG && ud_df.operand[0].size == 64 &&
                    ud_df.operand[1].type == UD_OP_REG && ud_df.operand[1].size == 64
                   )
                {
                    insn->mov_dst = (int8_t)reg64_index(ud_df.operand[0].base);
                    insn->mov_src = (int8_t)reg64_index(ud_df.operand[1].base);
                }
                break;
            default:
                break;
        }

        address += length;
    }

    if (df_function.num == 0) return 0;

    df_function.begin = begin;
    df_function.end = end;
    df_function.num_edges = num_edges;
    df_function.values = (uint64_t *) malloc(16 * sizeof(uint64_t) * df_function.num);
    df_function.reached = (uint8_t *) calloc(df_function.num, 1);
    stack = (unsigned int *) malloc(sizeof(unsigned int) * df_function.num);
    on_stack = (uint8_t *) calloc(df_function.num, 1);
    if (df_function.values == NULL || df_function.reached == NULL || stack == NULL || on_stack == NULL)
    {
        free(stack);
        free(on_stack);
        free(df_function.insn);
        free(df_function.values);
        free(df_function.reached);
        memset(&df_function, 0, sizeof(df_function));
        return 0;
    }

    // propagation (worklist)
    for (reg = 0; reg < 16; reg++) values[reg] = DF_UNKNOWN;
    SR_df_merge(0, values);
    stack[0] = 0;
    on_stack[0] = 1;
    top = 1;

    while (top != 0)
    {
        top--;
        index = (int)stack[top];
        on_stack[index] = 0;
        insn = &(df_function.insn[index]);

        memcpy(values, &(df_function.values[16 * (unsigned int)index]), sizeof(values));
        if (insn->mov_dst >= 0 && insn->mov_src >= 0)
        {
            uint64_t value = values[(int)insn->mov_src];

            for (reg = 0; reg < 16; reg++) if (insn->written & BIT(reg)) values[reg] = DF_UNKNOWN;
            values[(int)insn->mov_dst] = value;
        }
        else
        {
            for (reg = 0; reg < 16; reg++) if (insn->written & BIT(reg)) values[reg] = DF_UNKNOWN;
            if (insn->lea_reg >= 0) values[(int)insn->lea_reg] = insn->target;
        }

        for (next = 0; next < 2; next++)
        {
            int succ;

            if (next == 0)
            {
                if (insn->kind == DFK_JMP || insn->kind == DFK_STOP || insn->kind == DFK_INDIRECT) continue;
                succ = index + 1;
                if (succ >= (int)df_function.num) continue;
            }
            else
            {
                if (insn->kind != DFK_JMP && insn->kind != DFK_JCC) continue;
                succ = SR_df_find(insn->target);
                if (succ < 0) continue;
            }

            if (SR_df_merge(succ, values) && !on_stack[succ])
            {
                stack[top++] = (unsigned int)succ;
                on_stack[succ] = 1;
            }
        }

        if (insn->kind == DFK_INDIRECT && num_edges != 0)
        {
            for (edge = 0; edge < jt_edges.num; edge++)
            {
                int succ;

                if (jt_edges.from[edge] != insn->address) continue;
                succ = SR_df_find(jt_edges.to[edge]);
                if (succ < 0) continue;

                if (SR_df_merge(succ, values) && !on_stack[succ])
                {
                    stack[top++] = (unsigned int)succ;
                    on_stack[succ] = 1;
                }
            }
        }
    }

    free(stack);
    free(on_stack);
    return 1;
}

// address of jump table relative to its start (from data flow analysis), 0 = unknown
static uint64_t SR_find_table_by_dataflow(uint64_t jump_address, const run_state *state)
{
    const pdata_entry *function;
    uint64_t value;
    int index, k;

    function = SR_find_pdata((uint_fast32_t)(jump_address - ImageBase));
    if (function == NULL) return 0;
    if (!SR_df_analyze_function(ImageBase + function->begin, ImageBase + function->end)) return 0;

    index = SR_df_find(state->tbl_load_address);
    if (index < 0 || !df_function.reached[index]) return 0;

    for (k = 0; k < 2; k++)
    {
        int reg = (k == 0)?state->tbl_basereg:state->tbl_basereg2;

        if (reg < 0) continue;
        value = df_function.values[16 * (unsigned int)index + reg];
        if (value == DF_UNKNOWN) continue;

        if (!SR_elf_jump_table_valid(value, jump_address, state->tbl_bound))
        {
            fprintf(stderr, "Warning: jump table found by data flow is not valid: 0x%" PRIx64 " - 0x%" PRIx64 "\n", jump_address, value);
            return 0;
        }

        fprintf(stderr, "Note: address of jump table found by data flow: 0x%" PRIx64 " - 0x%" PRIx64 "\n", jump_address, value);
        return value;
    }

    return 0;
}

// writes line into the list of jump tables (--list_jump_tables)
static void SR_list_jump_table(uint64_t jump_address, uint64_t table_address, const char *kind, int num_entries, int bound, const uint64_t *targets)
{
    int index;

    if (jump_tables_file == NULL) return;

    fprintf(jump_tables_file, "0x%" PRIx64 " 0x%" PRIx64 " %s %i %i", jump_address, table_address, kind, num_entries, (bound > 0)?bound:-1);
    for (index = 0; index < num_entries; index++)
    {
        fprintf(jump_tables_file, " 0x%" PRIx64, targets[index]);
    }
    fprintf(jump_tables_file, "\n");
}

static int SR_process_jump_table(unsigned int Entry, uint_fast32_t jmp_offset, const run_state *state)
{
    uint_fast32_t tsec, tofs, eofs, esec, etofs;
    int count, max_count, index, k, is_rva, is_abs, esize;
    const pdata_entry *function, *target_function;
    output_data *output, *output2;
    int32_t value;
    uint64_t target;
    const char *kind;
    char cLabel[64], cTable[64], cbuf[256];
    static uint64_t targets[MAX_TABLE_ENTRIES];

    is_rva = (state->tbl_kind == TBL_RVA_READY)?1:0;
    is_abs = (state->tbl_kind == TBL_ABS_READY || state->tbl_kind == TBL_ABS_LOADED)?1:0;
    esize = (is_abs)?8:4;
    kind = (is_abs)?"abs":((is_rva)?"rva":"diff");

    if (!SR_get_section_reladr_exact(state->tbl_address, &tsec, &tofs))
    {
        fprintf(stderr, "Warning: jump table outside of sections: 0x%" PRIx64 "\n", state->tbl_address);
        SR_list_jump_table(section[Entry].start + jmp_offset, state->tbl_address, kind, 0, state->tbl_bound, targets);
        return 0;
    }

    count = state->tbl_bound;
    if (is_rva && state->tbl_uses_bt)
    {
        uint_fast32_t bsec, bofs;

        count = -1;
        if (state->bt_bound > 0 && SR_get_section_reladr_exact(state->bt_address, &bsec, &bofs) && bofs + state->bt_bound <= section[bsec].size)
        {
            int max_value;

            max_value = 0;
            for (k = 0; k < state->bt_bound; k++)
            {
                if (section[bsec].adr[bofs + k] > max_value) max_value = section[bsec].adr[bofs + k];
            }
            count = max_value + 1;
        }
    }

    function = SR_find_pdata(section[Entry].virtaddr + jmp_offset);
    max_count = (count > 0)?count:MAX_TABLE_ENTRIES;

    SR_get_label(cTable, state->tbl_address);

    // table was already converted (table used by more jumps) or contains relocations (or code pointers found in data)
    output = section_output_list_FindEntryEqual(tsec, tofs);
    if (output != NULL && output->type == OT_OFFSET)
    {
        // targets of converted table (for data flow analysis)
        for (index = 0; index < max_count; index++)
        {
            eofs = tofs + esize * index;
            if (eofs + esize > section[tsec].size) break;

            output = section_output_list_FindEntryEqual(tsec, eofs);
            if (output == NULL || output->type != OT_OFFSET || output->str == NULL || output->len != (uint_fast32_t)(esize - 1)) break;
            if (index > 0 && output->has_label) break;
            if (!is_abs && !is_rva)
            {
                size_t len1 = strlen(output->str), len2 = strlen(cTable);
                if (len1 < len2 + 3 || strcmp(output->str + len1 - len2, cTable) != 0 || output->str[len1 - len2 - 2] != '-') break;
            }

            if (is_abs)
            {
                memcpy(&target, &(section[tsec].adr[eofs]), 8);
            }
            else
            {
                memcpy(&value, &(section[tsec].adr[eofs]), 4);
                target = (is_rva)?(ImageBase + (uint32_t)value):(state->tbl_address + (int64_t)value);
            }
            SR_add_jump_table_edge(section[Entry].start + jmp_offset, target);
            targets[index] = target;
        }

        SR_list_jump_table(section[Entry].start + jmp_offset, state->tbl_address, kind, index, count, targets);
        return 0;
    }

    for (index = 0; index < max_count; index++)
    {
        eofs = tofs + esize * index;
        if (eofs + esize > section[tsec].size) break;

        output = section_output_list_FindEntryEqual(tsec, eofs);
        if (output == NULL || output->type != OT_UNKNOWN || output->str != NULL) break;
        if (index > 0 && count <= 0 && output->has_label) break;

        for (k = 1; k < esize; k++)
        {
            output2 = section_output_list_FindEntryEqual(tsec, eofs + k);
            if (output2 == NULL || output2->type != OT_UNKNOWN || output2->has_label || output2->str != NULL) break;
        }
        if (k < esize) break;

        if (is_abs)
        {
            memcpy(&target, &(section[tsec].adr[eofs]), 8);
        }
        else
        {
            memcpy(&value, &(section[tsec].adr[eofs]), 4);
            target = (is_rva)?(ImageBase + (uint32_t)value):(state->tbl_address + (int64_t)value);
        }

        if (!SR_get_section_reladr_exact(target, &esec, &etofs) ||
            section[esec].type != ST_CODE ||
            section_nocode_list_CanFindEntryEqual(esec, etofs) ||
            SR_is_ignored(esec, etofs) ||
            (count <= 0 && !SR_elf_is_instruction_start(target))
           )
        {
            if (count > 0)
            {
                fprintf(stderr, "Warning: invalid jump table entry: 0x%" PRIx64 " - 0x%" PRIx64 "\n", section[tsec].start + eofs, target);
            }
            break;
        }

        if (function != NULL)
        {
            target_function = SR_find_pdata(section[esec].virtaddr + etofs);
            if (target_function == NULL || target_function->primary != function->primary)
            {
                if (count > 0)
                {
                    fprintf(stderr, "Warning: jump table entry outside of function: 0x%" PRIx64 " - 0x%" PRIx64 "\n", section[tsec].start + eofs, target);
                }
                break;
            }
        }

        for (k = 1; k < esize; k++)
        {
            section_output_list_Delete(tsec, eofs + k);
        }

        SR_get_label(cLabel, target);
        if (is_abs)
        {
            sprintf(cbuf, "dq %s", cLabel);
        }
        else if (is_rva)
        {
            sprintf(cbuf, "dd RVA(%s)", cLabel);
        }
        else
        {
            sprintf(cbuf, "dd %s - %s", cLabel, cTable);
        }

        output->len = esize - 1;
        output->type = OT_OFFSET;
        output->str = strdup(cbuf);

        SR_add_jump_target(Entry, target, state, -1);
        SR_add_jump_table_edge(section[Entry].start + jmp_offset, target);
        targets[index] = target;
    }

    output = section_output_list_FindEntryEqual(tsec, tofs);
    if (output != NULL)
    {
        output->has_label = 1;
    }

    SR_list_jump_table(section[Entry].start + jmp_offset, state->tbl_address, kind, index, count, targets);

    if (index == 0)
    {
        fprintf(stderr, "Warning: jump table not recognized: 0x%" PRIx64 " (jump at 0x%" PRIx64 ")\n", state->tbl_address, section[Entry].start + jmp_offset);
    }
    else if (count > 0 && index < count)
    {
        fprintf(stderr, "Warning: jump table shorter than expected: 0x%" PRIx64 " - %i/%i\n", state->tbl_address, index, count);
    }

    return 0;
}

static void SR_update_run_state(run_state *state, const memory_info *mi)
{
    const struct ud_operand *op0, *op1;
    int written, r0, r1, base, was_cmp, old_ib_mask, old_lea_mask, old_bound_mask, old_bound, table_written, table_kept;
    int64_t was_cmp_imm;

    op0 = &(ud_obj.operand[0]);
    op1 = &(ud_obj.operand[1]);

    was_cmp = state->cmp_valid;
    was_cmp_imm = state->cmp_imm;
    state->cmp_valid = 0;

    old_ib_mask = state->ib_mask;
    old_lea_mask = state->lea_mask;
    old_bound_mask = state->bound_mask;
    old_bound = state->bound;

    r0 = (op0->type == UD_OP_REG)?reg_index(op0->base):-1;
    r1 = (op1->type == UD_OP_REG)?reg_index(op1->base):-1;

    // kill written registers
    written = SR_written_registers();
    state->ib_mask &= ~written;
    state->lea_mask &= ~written;
    state->bound_mask &= ~written;

    // checked index is kept by zero/sign extension of itself (mov r32, r32 / movsxd r64, r32 / cdqe) and copied by mov
    if (r0 >= 0 && r1 >= 0 && (old_bound_mask & BIT(r1)) && op0->size >= 32 &&
        (((ud_obj.mnemonic == UD_Imov || ud_obj.mnemonic == UD_Imovsxd) && op1->size >= 32) || ud_obj.mnemonic == UD_Imovzx)
       )
    {
        state->bound_mask |= BIT(r0);
    }
    else if (ud_obj.mnemonic == UD_Icdqe && (old_bound_mask & BIT(0)))
    {
        state->bound_mask |= BIT(0);
    }
    if (state->bound_mask == 0)
    {
        state->bound = -1;
    }

    // (when the jump target is computed, the base register isn't needed anymore: add rsi, rdx; mov edx, [rbp-X]; jmp rsi)
    if (state->tbl_kind == TBL_RVA_READY || state->tbl_kind == TBL_DIFF_READY)
    {
        table_written = (written & BIT(state->tbl_reg))?1:0;
    }
    else
    {
        table_written = (state->tbl_kind != TBL_NONE && (written & (BIT(state->tbl_reg) | BIT(state->tbl_basereg))))?1:0;
    }
    table_kept = 0;

    if (state->bt_valid && (written & BIT(state->bt_reg)))
    {
        state->bt_valid = 0;
    }

    switch (ud_obj.mnemonic)
    {
        case UD_Icmp:
            if (op1->type == UD_OP_IMM && (op0->type == UD_OP_REG || op0->type == UD_OP_MEM))
            {
                int64_t value;

                switch (op1->size)
                {
                    case 8: value = op1->lval.sbyte; break;
                    case 16: value = op1->lval.sword; break;
                    case 32: value = op1->lval.sdword; break;
                    default: value = op1->lval.sqword; break;
                }

                switch (op0->size)
                {
                    case 8: value &= 0xff; break;
                    case 16: value &= 0xffff; break;
                    case 32: value &= 0xffffffff; break;
                    default: break;
                }

                state->cmp_valid = 1;
                state->cmp_imm = value;
                state->cmp_reg = (op0->type == UD_OP_REG)?reg_index(op0->base):-1;
            }
            break;

        case UD_Iand:
            // and reg, 2^n - 1 - the register contains index of table with 2^n entries
            if (r0 >= 0 && op1->type == UD_OP_IMM && op0->size >= 32)
            {
                uint64_t mask;

                mask = (op1->size == 8)?(uint64_t)(int64_t)op1->lval.sbyte:((op1->size == 16)?(uint64_t)(int64_t)op1->lval.sword:(uint64_t)(int64_t)op1->lval.sdword);
                if (op0->size == 32) mask &= 0xffffffff;
                if (mask != 0 && mask < MAX_TABLE_ENTRIES && (mask & (mask + 1)) == 0)
                {
                    state->bound = (int)(mask + 1);
                    state->bound_mask = BIT(r0);
                }
            }
            break;

        case UD_Ija:
            if (was_cmp)
            {
                state->bound = (was_cmp_imm >= 0 && was_cmp_imm < MAX_TABLE_ENTRIES)?(int)(was_cmp_imm + 1):-1;
                state->bound_mask = (state->cmp_reg >= 0)?BIT(state->cmp_reg):0xffff;
            }
            break;

        case UD_Ijae:
            if (was_cmp)
            {
                state->bound = (was_cmp_imm > 0 && was_cmp_imm <= MAX_TABLE_ENTRIES)?(int)was_cmp_imm:-1;
                state->bound_mask = (state->cmp_reg >= 0)?BIT(state->cmp_reg):0xffff;
            }
            break;

        case UD_Ilea:
            if (r0 >= 0 && op0->size == 64 && mi->opnum == 1)
            {
                if (mi->kind == MEMK_HEADER && mi->target == ImageBase)
                {
                    state->ib_mask |= BIT(r0);
                }
                else if (mi->kind == MEMK_LABEL)
                {
                    state->lea_mask |= BIT(r0);
                    state->lea_target[r0] = mi->target;
                }
                else if (mi->kind == MEMK_NONE && state->tbl_kind == TBL_DIFF_LOADED && SR_no_displacement(op1) && op1->scale <= 1 &&
                         ((reg64_index(op1->base) == state->tbl_reg && SR_is_table_base(state, old_lea_mask, reg64_index(op1->index))) ||
                          (reg64_index(op1->index) == state->tbl_reg && SR_is_table_base(state, old_lea_mask, reg64_index(op1->base)))
                         )
                        )
                {
                    // lea r64, [table + value]
                    base = (reg64_index(op1->base) == state->tbl_reg)?reg64_index(op1->index):reg64_index(op1->base);
                    if (base != state->tbl_basereg) state->tbl_basereg2 = base;
                    state->tbl_kind = TBL_DIFF_READY;
                    state->tbl_reg = r0;
                    table_kept = 1;
                }
            }
            break;

        case UD_Imov:
            if (r0 >= 0 && r1 >= 0 && op0->size == 64 && op1->size == 64)
            {
                // copy of register containing image base / table address
                if (old_ib_mask & BIT(r1)) state->ib_mask |= BIT(r0);
                if (old_lea_mask & BIT(r1))
                {
                    state->lea_mask |= BIT(r0);
                    state->lea_target[r0] = state->lea_target[r1];
                }
            }
            else if (r0 >= 0 && op0->size == 64 && mi->kind == MEMK_NONE && mi->opnum == 1 && op1->base == UD_NONE && op1->index != UD_NONE && op1->scale == 8 && op1->offset == 32)
            {
                // mov r64, [index*8 + table] (table with absolute addresses)
                state->tbl_kind = TBL_ABS_LOADED;
                state->tbl_reg = r0;
                state->tbl_basereg = r0;
                state->tbl_address = (uint64_t)(int64_t)op1->lval.sdword;
                state->tbl_bound = SR_index_bound(old_bound, old_bound_mask, op1);
                state->tbl_uses_bt = 0;
                table_kept = 1;
            }
            else if (r0 >= 0 && op0->size == 32 && mi->kind == MEMK_RVA && mi->opnum == 1 && op1->scale == 4 && op1->size == 32)
            {
                // mov r32, [imagebase + index*4 + RVA(table)]
                state->tbl_kind = TBL_RVA_LOADED;
                state->tbl_reg = r0;
                state->tbl_basereg = (reg64_index(op1->base) >= 0)?reg64_index(op1->base):r0;
                state->tbl_address = mi->target;
                state->tbl_bound = SR_index_bound(old_bound, old_bound_mask, op1);
                state->tbl_uses_bt = (state->bt_valid && reg64_index(op1->index) == state->bt_reg)?1:0;
                table_kept = 1;
            }
            break;

        case UD_Imovsxd:
            if (r0 >= 0 && mi->opnum == 1 && op1->scale == 4)
            {
                base = reg64_index(op1->base);

                if (mi->kind == MEMK_NONE && SR_no_displacement(op1) && base >= 0 && (old_lea_mask & BIT(base)))
                {
                    // movsxd r64, [table + index*4]
                    state->tbl_kind = TBL_DIFF_LOADED;
                    state->tbl_reg = r0;
                    state->tbl_basereg = base;
                    state->tbl_basereg2 = -1;
                    state->tbl_load_address = ud_insn_off(&ud_obj);
                    state->tbl_address = state->lea_target[base];
                    state->tbl_bound = SR_index_bound(old_bound, old_bound_mask, op1);
                    state->tbl_uses_bt = 0;
                    table_kept = 1;
                }
                else if (InputFormat == INPUT_ELF && mi->kind == MEMK_NONE && SR_no_displacement(op1) && base >= 0 && reg64_index(op1->index) >= 0)
                {
                    // movsxd r64, [table + index*4] - table address isn't known in this run (e.g. table address loaded before loop)
                    state->tbl_kind = TBL_DIFF_LOADED;
                    state->tbl_reg = r0;
                    state->tbl_basereg = base;
                    state->tbl_basereg2 = -1;
                    state->tbl_load_address = ud_insn_off(&ud_obj);
                    state->tbl_address = 0;
                    state->tbl_bound = SR_index_bound(old_bound, old_bound_mask, op1);
                    state->tbl_uses_bt = 0;
                    table_kept = 1;
                }
                else if (mi->kind == MEMK_RVA)
                {
                    // movsxd r64, [imagebase + index*4 + RVA(table)]
                    state->tbl_kind = TBL_RVA_LOADED;
                    state->tbl_reg = r0;
                    state->tbl_basereg = (base >= 0)?base:r0;
                    state->tbl_address = mi->target;
                    state->tbl_bound = SR_index_bound(old_bound, old_bound_mask, op1);
                    state->tbl_uses_bt = (state->bt_valid && reg64_index(op1->index) == state->bt_reg)?1:0;
                    table_kept = 1;
                }
            }
            break;

        case UD_Imovzx:
            if (r0 >= 0 && mi->kind == MEMK_RVA && mi->opnum == 1 && op1->size == 8)
            {
                // movzx r32, byte [imagebase + index + RVA(byte_table)]
                state->bt_valid = 1;
                state->bt_reg = r0;
                state->bt_address = mi->target;
                state->bt_bound = SR_index_bound(old_bound, old_bound_mask, op1);
            }
            break;

        case UD_Iadd:
            if (r0 >= 0 && r1 >= 0 && op0->size == 64 && op1->size == 64)
            {
                if (state->tbl_kind == TBL_RVA_LOADED)
                {
                    if (r0 == state->tbl_reg && (old_ib_mask & BIT(r1)))
                    {
                        state->tbl_kind = TBL_RVA_READY;
                        table_kept = 1;
                    }
                    else if (r1 == state->tbl_reg && (old_ib_mask & BIT(r0)))
                    {
                        state->tbl_kind = TBL_RVA_READY;
                        state->tbl_reg = r0;
                        table_kept = 1;
                    }
                }
                else if (state->tbl_kind == TBL_DIFF_LOADED)
                {
                    if (r0 == state->tbl_reg && SR_is_table_base(state, old_lea_mask, r1))
                    {
                        if (r1 != state->tbl_basereg) state->tbl_basereg2 = r1;
                        state->tbl_kind = TBL_DIFF_READY;
                        table_kept = 1;
                    }
                    else if (r1 == state->tbl_reg && SR_is_table_base(state, old_lea_mask, r0))
                    {
                        if (r0 != state->tbl_basereg) state->tbl_basereg2 = r0;
                        state->tbl_kind = TBL_DIFF_READY;
                        state->tbl_reg = r0;
                        table_kept = 1;
                    }
                }
            }
            break;

        default:
            break;
    }

    if (table_written && !table_kept)
    {
        state->tbl_kind = TBL_NONE;
    }
}

int SR_disassemble_offset_win64(unsigned int Entry, uint_fast32_t offset)
{
    output_data *output, *output2;
    fixup_data *fixup, *fixup2;
    extrn_data *extrn;
    int finished, decoded_length, ret, conv, index, bound;
    uint64_t address;
    const char *text;
    char cLabel[256];
    static char cResult[16384], cMem[16384], cFix[16384], cFix2[16384], cImm[16384], inner_labels[2048];
    memory_info mi;
    run_state state;

    SR_init_run_state(Entry, offset, &state);

    ud_set_input_buffer(&ud_obj, &(section[Entry].adr[offset]), section[Entry].size - offset);
    ud_set_pc(&ud_obj, section[Entry].start + offset);

    finished = 0;

    while (!finished)
    {
        output = section_output_list_FindEntryEqual(Entry, offset);

        if (output == NULL && SR_is_ignored(Entry, offset))
        {
            // code continues into ignored area (e.g. after call to function which doesn't return)
            fprintf(stderr, "Note: code continues into ignored area: 0x%" PRIx64 "\n", section[Entry].start + offset);
            finished = 1;
            break;
        }

        if (output == NULL)
        {
            fprintf(stderr, "Error: output not found - %i - 0x%" PRIx64 "\n", Entry, section[Entry].start + offset);

            return 1;
        }

        if (output->type > 0)
        {
            finished = 1;
            break;
        }

        // lock prefix is a separate instruction in Linux executables (glibc jumps over the prefix in single threaded programs)
        if (InputFormat == INPUT_ELF && section[Entry].adr[offset] == 0xf0 && offset + 1 < section[Entry].size)
        {
            output2 = section_output_list_FindEntryEqual(Entry, offset + 1);
            if (output2 != NULL)
            {
                output = SR_disassemble_offset_init_output(Entry, offset, 1);
                output->str = strdup("db 0xf0 ; lock");

                offset++;
                ud_set_input_buffer(&ud_obj, &(section[Entry].adr[offset]), section[Entry].size - offset);
                ud_set_pc(&ud_obj, section[Entry].start + offset);
                continue;
            }
        }

        decoded_length = ud_disassemble(&ud_obj);

        if (decoded_length == 0)
        {
            finished = 2;
            break;
        }

        if (ud_obj.mnemonic == UD_Iinvalid)
        {
            fprintf(stderr, "Error: invalid instruction: 0x%" PRIx64 " - %s\n", section[Entry].start + offset, ud_insn_hex(&ud_obj));
            return 3;
        }

        // find relocations inside instruction
        fixup2 = NULL;
        fixup = section_fixup_list_FindEntryEqualOrHigher(Entry, offset);

        if (fixup != NULL)
        {
            if (fixup->sofs >= offset + decoded_length) fixup = NULL;
            else
            {
                if (fixup->sofs + FIXUP_SIZE(fixup->type) > offset + decoded_length)
                {
                    fprintf(stderr, "Error: decoding fixup mismatch - %i - 0x%" PRIx64 "\n", Entry, section[Entry].start + offset);

                    return 2;
                }

                fixup2 = section_fixup_list_FindEntryHigher(Entry, fixup->sofs);

                if (fixup2 != NULL)
                {
                    if (fixup2->sofs >= offset + decoded_length) fixup2 = NULL;
                    else
                    {
                        if (fixup2->sofs + FIXUP_SIZE(fixup2->type) > offset + decoded_length)
                        {
                            fprintf(stderr, "Error: decoding fixup mismatch - %i - 0x%" PRIx64 "\n", Entry, section[Entry].start + offset);

                            return 2;
                        }
                    }
                }
            }
        }

        // check if the instruction overlaps already processed instructions or labels
        inner_labels[0] = 0;
        for (index = 1; index < decoded_length; index++)
        {
            if ((fixup != NULL && offset + index == fixup->sofs) || (fixup2 != NULL && offset + index == fixup2->sofs)) continue;

            output2 = section_output_list_FindEntryEqual(Entry, offset + index);
            if (output2 == NULL) continue;

            if (output2->type > 0)
            {
                fprintf(stderr, "Error: instruction overlaps another instruction: 0x%" PRIx64 " - 0x%" PRIx64 "\n", section[Entry].start + offset, section[Entry].start + offset + index);
                return 5;
            }

            if (output2->has_label)
            {
                // label inside instruction is defined relative to the instruction
                char label1[64], label2[64];

                fprintf(stderr, "Note: label inside instruction: 0x%" PRIx64 " - 0x%" PRIx64 "\n", section[Entry].start + offset, section[Entry].start + offset + index);

                SR_get_label_def(label1, section[Entry].start + offset + index);
                SR_get_label_def(label2, section[Entry].start + offset);
                if (strlen(inner_labels) + 160 < sizeof(inner_labels))
                {
                    sprintf(&(inner_labels[strlen(inner_labels)]), "\n%s equ %s+%i", label1, label2, index);
                }
            }
        }

        output = SR_disassemble_offset_init_output(Entry, offset, decoded_length);

        text = ud_insn_asm(&ud_obj);

        // memory operands
        conv = SR_convert_memory_operand(Entry, offset, text, cMem, &state, &mi);
        if (conv < 0) return conv;
        if (conv > 0) text = cMem;

        // relocations inside instruction
        if (fixup != NULL)
        {
            extrn = (fixup->tsec != SEC_HEADER)?section_extrn_list_FindEntryEqual(fixup->tsec, fixup->tofs):NULL;
            ret = SR_disassemble_convert_fixup(text, cFix, fixup, extrn, Entry, offset, decoded_length);
            if (ret) return ret;
            text = cFix;

            if (fixup2 != NULL)
            {
                extrn = (fixup2->tsec != SEC_HEADER)?section_extrn_list_FindEntryEqual(fixup2->tsec, fixup2->tofs):NULL;
                ret = SR_disassemble_convert_fixup(text, cFix2, fixup2, extrn, Entry, offset, decoded_length);
                if (ret) return ret;
                text = cFix2;
            }
        }

        // immediate values with addresses of functions (executables without relocations)
        if (InputFormat == INPUT_ELF && !ElfRelocatedPointers && fixup == NULL && (ud_obj.mnemonic == UD_Imov || ud_obj.mnemonic == UD_Icmp || ud_obj.mnemonic == UD_Ipush))
        {
            for (index = 0; index < 4; index++)
            {
                const struct ud_operand *op;
                uint64_t value;

                op = &(ud_obj.operand[index]);
                if (op->type != UD_OP_IMM || (op->size != 32 && op->size != 64)) continue;

                value = (op->size == 64)?op->lval.uqword:(uint64_t)op->lval.udword;

                // imported symbol (PLT stub, object copied into executable), external procedure, original code
                {
                    extrn_data *imm_extrn;

                    imm_extrn = SR_disassemble_find_proc(Entry, value);
                    if (imm_extrn != NULL || SR_elf_dyn_is_import_address(value))
                    {
                        if (imm_extrn != NULL) strcpy(cLabel, imm_extrn->proc);
                        else SR_get_label(cLabel, value);

                        if (SR_replace_value_text(text, cImm, value, cLabel) == 0)
                        {
                            text = cImm;
                        }
                        break;
                    }
                }

                if (!SR_elf_is_function_start(value))
                {
                    if (ud_obj.mnemonic != UD_Imov) continue;

                    // address inside the same function (e.g. computed jump in varargs prologue of older GCC)
                    const pdata_entry *function;
                    uint_fast32_t vsec, vofs;

                    function = SR_find_pdata(section[Entry].virtaddr + offset);
                    if (function == NULL || value < ImageBase + function->begin || value >= ImageBase + function->end) continue;
                    if (!SR_get_section_reladr_exact(value, &vsec, &vofs) || section[vsec].type != ST_CODE) continue;
                }

                SR_get_label(cLabel, value);
                ret = SR_replace_value_text(text, cImm, value, cLabel);
                if (ret == 0)
                {
                    SR_disassemble_add_address(Entry, value);
                    text = cImm;
                }
                break;
            }
        }

        switch (ud_obj.mnemonic)
        {
            case UD_Icall:
                if (ud_obj.operand[0].type == UD_OP_JIMM)
                {
                    address = ud_obj.pc + (int64_t)ud_obj.operand[0].lval.sdword;

                    extrn = SR_disassemble_find_proc(Entry, address);

                    if (extrn != NULL)
                    {
                        if (extrn->altaction != NULL)
                        {
                            strcpy(cResult, extrn->altaction);
                        }
                        else
                        {
                            sprintf(cResult, "call %s", extrn->proc);
                        }
                    }
                    else
                    {
                        SR_disassemble_add_address(Entry, address);

                        if (SR_disassemble_find_noret(address) || SR_is_noret_thunk(address))
                        {
                            finished = 1;
                        }

                        SR_get_label(cLabel, address);
                        sprintf(cResult, "call %s", cLabel);
                    }

                    text = cResult;
                }
                else if (mi.kind == MEMK_IMPORT && mi.opnum == 0)
                {
                    mi.extrn->is_used = 1;
                    sprintf(cResult, "call %s", mi.extrn->proc);
                    text = cResult;

                    if (SR_disassemble_find_noret(mi.target))
                    {
                        finished = 1;
                    }
                }
                else if (mi.kind == MEMK_IMPORT)
                {
                    mi.extrn->is_ptr_used = 1;
                }

                output->str = strdup(text);
                break;

            case UD_Ijmp:
                if (ud_obj.operand[0].type == UD_OP_JIMM)
                {
                    address = ud_obj.pc + (int64_t)((ud_obj.operand[0].size == 32)?ud_obj.operand[0].lval.sdword:ud_obj.operand[0].lval.sbyte);

                    extrn = SR_disassemble_find_proc(Entry, address);

                    if (extrn != NULL)
                    {
                        sprintf(cResult, "jmp %s", extrn->proc);
                    }
                    else
                    {
                        SR_add_jump_target(Entry, address, &state, -1);

                        SR_get_label(cLabel, address);
                        // short jump can't be used for jumps to absolute addresses (Linux executables: original code)
                        sprintf(cResult, "jmp %s%s", ((ud_obj.operand[0].size == 32 || strncmp(cLabel, "srw_abs_", 8) == 0 || strncmp(cLabel, "srw_orig_", 9) == 0)?"":"short "), cLabel);
                    }

                    text = cResult;
                }
                else if (mi.kind == MEMK_IMPORT && mi.opnum == 0)
                {
                    mi.extrn->is_used = 1;
                    sprintf(cResult, "jmp %s", mi.extrn->proc);
                    text = cResult;
                }
                else
                {
                    if (mi.kind == MEMK_IMPORT)
                    {
                        mi.extrn->is_ptr_used = 1;
                    }

                    if (ud_obj.operand[0].type == UD_OP_REG &&
                        detect_jump_tables &&
                        (state.tbl_kind == TBL_RVA_READY || state.tbl_kind == TBL_DIFF_READY || state.tbl_kind == TBL_ABS_LOADED) &&
                        reg_index(ud_obj.operand[0].base) == state.tbl_reg
                       )
                    {
                        if (state.tbl_kind == TBL_DIFF_READY && state.tbl_address == 0)
                        {
                            // address of table: data flow analysis of the function, or lea base, [rip+table] somewhere in the function (validated by entries)
                            state.tbl_address = SR_find_table_by_dataflow(section[Entry].start + offset, &state);
                            if (state.tbl_address == 0)
                            {
                                state.tbl_address = SR_elf_find_table_address(section[Entry].start + offset, state.tbl_basereg, state.tbl_basereg2, state.tbl_bound);
                            }
                            if (state.tbl_address == 0)
                            {
                                fprintf(stderr, "Warning: address of jump table not found: 0x%" PRIx64 "\n", section[Entry].start + offset);
                                SR_list_jump_table(section[Entry].start + offset, 0, "diff", 0, state.tbl_bound, NULL);
                            }
                        }

                        if (state.tbl_kind != TBL_DIFF_READY || state.tbl_address != 0)
                        {
                            ret = SR_process_jump_table(Entry, offset, &state);
                            if (ret) return ret;
                        }
                    }
                    else if (ud_obj.operand[0].type == UD_OP_MEM &&
                             detect_jump_tables &&
                             fixup == NULL &&
                             mi.kind == MEMK_NONE &&
                             ud_obj.operand[0].base == UD_NONE &&
                             ud_obj.operand[0].index != UD_NONE &&
                             ud_obj.operand[0].scale == 8 &&
                             ud_obj.operand[0].offset == 32
                            )
                    {
                        // jmp [index*8 + table] (table with absolute addresses)
                        run_state table_state;

                        table_state = state;
                        table_state.tbl_kind = TBL_ABS_READY;
                        table_state.tbl_address = (uint64_t)(int64_t)ud_obj.operand[0].lval.sdword;
                        table_state.tbl_bound = SR_index_bound(state.bound, state.bound_mask, &(ud_obj.operand[0]));
                        table_state.tbl_uses_bt = 0;

                        ret = SR_process_jump_table(Entry, offset, &table_state);
                        if (ret) return ret;
                    }
                }

                output->str = strdup(text);
                finished = 1;
                break;

            case UD_Ijo:
            case UD_Ijno:
            case UD_Ijb:
            case UD_Ijae:
            case UD_Ijz:
            case UD_Ijnz:
            case UD_Ijbe:
            case UD_Ija:
            case UD_Ijs:
            case UD_Ijns:
            case UD_Ijp:
            case UD_Ijnp:
            case UD_Ijl:
            case UD_Ijge:
            case UD_Ijle:
            case UD_Ijg:
            case UD_Ijcxz:
            case UD_Ijecxz:
            case UD_Ijrcxz:
            case UD_Iloop:
            case UD_Iloope:
            case UD_Iloopne:
                if (ud_obj.operand[0].type != UD_OP_JIMM)
                {
                    fprintf(stderr, "Error: unknown conditional jump: 0x%" PRIx64 " - %s\n", section[Entry].start + offset, text);
                    return -20;
                }

                address = ud_obj.pc + (int64_t)((ud_obj.operand[0].size == 32)?ud_obj.operand[0].lval.sdword:ud_obj.operand[0].lval.sbyte);

                extrn = SR_disassemble_find_proc(Entry, address);

                bound = -1;
                if (state.cmp_valid)
                {
                    if (ud_obj.mnemonic == UD_Ijbe)
                    {
                        bound = (state.cmp_imm >= 0 && state.cmp_imm < MAX_TABLE_ENTRIES)?(int)(state.cmp_imm + 1):-1;
                    }
                    else if (ud_obj.mnemonic == UD_Ijb)
                    {
                        bound = (state.cmp_imm > 0 && state.cmp_imm <= MAX_TABLE_ENTRIES)?(int)state.cmp_imm:-1;
                    }
                }

                if (extrn == NULL)
                {
                    SR_add_jump_target(Entry, address, &state, bound);
                }

                if (extrn == NULL)
                {
                    SR_get_label(cLabel, address);
                }

                if (ud_obj.operand[0].size == 32 ||
                    ((extrn != NULL || strncmp(cLabel, "srw_abs_", 8) == 0 || strncmp(cLabel, "srw_orig_", 9) == 0) &&
                     ud_obj.mnemonic != UD_Ijcxz && ud_obj.mnemonic != UD_Ijecxz && ud_obj.mnemonic != UD_Ijrcxz &&
                     ud_obj.mnemonic != UD_Iloop && ud_obj.mnemonic != UD_Iloope && ud_obj.mnemonic != UD_Iloopne
                    )
                   )
                {
                    // near jump (short jump can't be used for jumps to external procedures or absolute addresses)
                    ret = SR_disassemble_convert_cjump(cResult, " near", address, extrn);
                }
                else if (ud_obj.mnemonic == UD_Ijcxz || ud_obj.mnemonic == UD_Ijecxz || ud_obj.mnemonic == UD_Ijrcxz ||
                         ud_obj.mnemonic == UD_Iloop || ud_obj.mnemonic == UD_Iloope || ud_obj.mnemonic == UD_Iloopne)
                {
                    ret = SR_disassemble_convert_cjump(cResult, "", address, extrn);
                }
                else
                {
                    ret = SR_disassemble_convert_cjump(cResult, " short", address, extrn);
                }

                if (ret) return ret;

                output->str = strdup(cResult);
                break;

            case UD_Iret:
            case UD_Iretf:
            case UD_Iiretw:
            case UD_Iiretd:
            case UD_Iiretq:
            case UD_Isysret:
            case UD_Ihlt:
            case UD_Iud2:
                output->str = strdup(text);
                finished = 1;
                break;

            case UD_Iint3:
                // int3 is used as padding after calls to functions which don't return
                output->str = strdup("int3");
                finished = 1;
                break;

            case UD_Iint:
                output->str = strdup(text);
                if (ud_obj.operand[0].lval.ubyte == 0x29) // __fastfail
                {
                    finished = 1;
                }
                break;

            default:
                {
                    // other accesses to import address table use pointers to imports (__imp_Name)
                    // (imported variables are accessible only this way)
                    if (mi.kind == MEMK_IMPORT)
                    {
                        mi.extrn->is_ptr_used = 1;
                    }

                    if (ud_obj.mnemonic == UD_Ifld &&
                        ud_obj.operand[0].type == UD_OP_REG &&
                        ud_obj.operand[1].type == UD_OP_REG &&
                        ud_obj.operand[0].base == UD_R_ST0 &&
                        ud_obj.operand[1].base >= UD_R_ST0 &&
                        ud_obj.operand[1].base <= UD_R_ST7)
                    {
                        sprintf(cResult, "fld st%i", (int)(ud_obj.operand[1].base - UD_R_ST0));
                        text = cResult;
                    }
                }

                output->str = strdup(text);
                break;
        }

        if (inner_labels[0] != 0 && output->str != NULL)
        {
            char *new_str;

            output->has_label = 1;
            new_str = (char *) malloc(strlen(output->str) + strlen(inner_labels) + 1);
            if (new_str != NULL)
            {
                strcpy(new_str, output->str);
                strcat(new_str, inner_labels);
                free(output->str);
                output->str = new_str;
            }
        }

        SR_update_run_state(&state, &mi);

        offset += decoded_length;
    }

    return (finished == 2)?4:0;
}
