/**
 *
 *  Calls from C into recompiled game code (x86: x86/c2asm.asm, x86-64: x64/c2asm.asm, which switches to the
 *  emulated x86 stack). Game function pointers are 32-bit values.
 *
 */

#include <stdint.h>
#include "platform.h"
#include "winapi.h"
#if defined(__x86_64__)
#include "x64/x64_stack.h"
EXTERNC _stack *x86_initialize_stack(void);
#endif

EXTERN_C_BEGIN

uint32_t c_call_asm_n(void *stack, uint32_t func, uint32_t nargs, const uint32_t *args);

uint32_t call_game(uint32_t func, uint32_t nargs, const uint32_t *args)
{
#if defined(__x86_64__)
    // the emulated stack pointer is restored here (cdecl: the arguments are still on the stack);
    // nothing in the asm frame is safe for keeping it (asm_fs_mem.asm uses the parameter slots on Win64)
    _stack *stack = x86_initialize_stack();
    void *saved_esp = stack->esp;
    uint32_t result = c_call_asm_n(stack, func, nargs, args);
    stack->esp = saved_esp;
    return result;
#else
    return c_call_asm_n(0, func, nargs, args);
#endif
}

EXTERN_C_END
