/**
 *
 *  The llasm version of the recompiled code (SRW OUT_LLASM): the emulated CPU of each thread (llasm_cpu.h's _cpu:
 *  registers, x87 stack, the emulated x86 stack - from Septerra's llasm/asm-cpu.c) and calls from C into recompiled
 *  code (c2asm.c's call_game; the x64 build's x64/c2asm.asm and x64/asm-cpu.c).
 *
 *  llasm_cpu.h defines the register names as macros (eax = cpu->_eax, ...): this file includes little else of the
 *  runtime.
 *
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "../Game-Memory.h"
#include "llasm_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

void CCALL Raw_UnexpectedHandler(int which);

// runtime-asm.llasm: tcall eax (llasm's global procedure pushes its return procedure as the return address)
void CCALL c_call_asm_eax(CPU);

uint32_t X86_InterruptFlag;

#ifdef PTROFS_64BIT
extern uint64_t pointer_offset;
#endif

#ifdef __cplusplus
}
#endif

static thread_local _cpu *thread_cpu = NULL;

EXTERNC _cpu *x86_initialize_cpu(void)
{
    _cpu *cpu;
    void *stack_bottom;

    cpu = thread_cpu;
    if (cpu != NULL) return cpu;

    // 1 MB: the emulated x86 stack (game-visible memory) with the _cpu structure at its top
    stack_bottom = x86_malloc(1024 * 1024 - 8);
    if (stack_bottom == NULL) exit(2);

    cpu = (_cpu *)((1024 * 1022 + (uintptr_t)stack_bottom) & ~(uintptr_t)127);
    cpu->stack_bottom = stack_bottom;
    cpu->stack_top = (void *)(1024 * 1020 + (uintptr_t)stack_bottom);

    cpu->_st_top = 0;
    cpu->_st_sw_cond = 0;
    cpu->_st_cw = 0x037f;

#ifdef PTROFS_64BIT
    cpu->_pointer_offset = pointer_offset;
#else
    cpu->_reserved1 = 0;
#endif

    esp = PTR2REG(cpu->stack_top);
    eflags = 0x3202;
    X86_InterruptFlag = 1;

    thread_cpu = cpu;
    return cpu;
}

EXTERNC void x86_deinitialize_cpu(void)
{
    _cpu *cpu;
    void *stack_bottom;

    cpu = thread_cpu;
    if (cpu == NULL) return;

    if (cpu->stack_bottom != NULL)
    {
        stack_bottom = cpu->stack_bottom;
        cpu->stack_bottom = NULL;
        x86_free(stack_bottom);
    }

    thread_cpu = NULL;
}

// func = the recompiled function's address; the arguments are pushed right to left; thiscall: args[0] = this (ecx).
// The emulated stack pointer is restored afterwards (cdecl leaves the arguments on the stack).
EXTERNC uint32_t llasm_call_game(uint32_t func, uint32_t nargs, const uint32_t *args, int thiscall)
{
    _cpu *cpu;
    uint32_t saved_esp, saved_eax, saved_ecx, result, i;

    cpu = x86_initialize_cpu();
    saved_esp = esp;
    // the caller's eax/ecx (the recompiled code that called into C expects only eax/ecx/edx to change, and those
    // are the result of the C function anyway; kept for nested callbacks)
    saved_eax = eax;
    saved_ecx = ecx;

    for (i = nargs; i > (thiscall ? 1u : 0u); i--)
    {
        esp -= 4;
        *((uint32_t *)REG2PTR(esp)) = args[i - 1];
    }
    if (thiscall) ecx = args[0];
    eax = func;

    c_call_asm_eax(cpu);

    result = eax;
    esp = saved_esp;
    eax = saved_eax;
    ecx = saved_ecx;
    return result;
}

// the emulated stack pointer of this thread (crash reports)
EXTERNC uint32_t llasm_current_esp(void)
{
    return (thread_cpu != NULL) ? thread_cpu->_esp : 0;
}

// the return address on the emulated stack: the recompiled code that called the import being run (diagnostics)
EXTERNC uint32_t llasm_caller(void)
{
    _cpu *cpu = thread_cpu;
    if (cpu == NULL) return 0;
    return *((uint32_t *)REG2PTR(esp));
}

// SR/llasm-support: software interrupts and accesses to absolute low addresses (dead paths in AvP2's engine - the
// original would fault there too; the x64 build's x64/raw-asm.asm)
EXTERNC void CCALL X86_InterruptProcedure(const uint8_t IntNum, void *regs)
{
    Raw_UnexpectedHandler(3);
}

EXTERNC uint32_t CCALL X86_ReadMemProcedure(const uint32_t Address, const uint32_t MemSize)
{
    Raw_UnexpectedHandler(4);
    return 0;
}

EXTERNC void CCALL X86_WriteMemProcedure(const uint32_t Address, const uint32_t MemSize, const uint32_t _eax)
{
    Raw_UnexpectedHandler(4);
}
