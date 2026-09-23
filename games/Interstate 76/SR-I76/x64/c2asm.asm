;;
;;  Hand-written replacements for MSVCRT helpers with non-standard calling conventions.
;;






;; x86-64 (OUT_X64): calls from C into recompiled game code
;;
;; uint32_t c_call_asm_n(_stack *stack, uint32_t func, uint32_t nargs, const uint32_t *args)
;;   pushes args[nargs-1] .. args[0] on the emulated x86 stack and calls func (cdecl or stdcall: the
;;   stack pointer is restored from r12 afterwards, which recompiled code doesn't use); returns eax.
;;   [rsp] = stack while game code runs: the asm2c stubs (Call_Asm_Prologue_0) read it there.

%include "x64inc.inc"

global c_call_asm_n

%ifidn __OUTPUT_FORMAT__, elf64
section .note.GNU-stack noalloc noexec nowrite progbits
section .text progbits alloc exec nowrite align=16
%else
section .text code align=16
%endif

%ifidn __OUTPUT_FORMAT__, win64

; Win64: rcx = stack, edx = func, r8d = nargs, r9 = args
; The frame is the one the asm2c stubs expect (asm_unwind.inc): [rsp] = 4 free parameter slots,
; [rsp+FIRST_PARAMETER_OFFSET] = _stack *, the other parameter slots are scratch memory of asm_fs_mem.asm.
; call_game restores stack->esp afterwards (cdecl functions leave their arguments on the stack).

align 16
c_call_asm_n:
        SECTION_PROLOG
        mov [rsp+FIRST_PARAMETER_OFFSET], rcx
        mov r11d, [rcx]         ; esp = stack->esp
        mov r10d, edx           ; function
        mov ecx, r8d
        test ecx, ecx
        jz .call
.push:
        mov eax, [r9+rcx*4-4]
        sub r11d, byte 4
        mov [r11d], eax
        dec ecx
        jnz .push
.call:
        CALL r10
        SECTION_EPILOG

%else

align 16
c_call_asm_n:
        push rbp                ; game code clobbers ebx/ebp (the upper halves are lost)
        push rbx
        push r12
        push r13                ; padding: rsp is 16-byte aligned after the next push
        push rdi                ; [rsp] = _stack *
        mov r11d, [rdi]         ; esp = stack->esp
        mov r10d, esi           ; function
        mov r13, rcx            ; args
        mov r12d, r11d          ; saved esp
        mov ecx, edx
        test ecx, ecx
        jz .call
.push:
        mov eax, [r13+rcx*4-4]
        sub r11d, byte 4
        mov [r11d], eax
        dec ecx
        jnz .push
.call:
        CALL r10
        mov r11d, r12d          ; restore esp (cdecl: the caller pops, stdcall: the callee already did)
        pop rdi
        mov [rdi], r11d         ; stack->esp = esp
        pop r13
        pop r12
        pop rbx
        pop rbp
        ret

%endif

; end procedure c_call_asm_n
