;; Steel Panthers runtime: calls from C into recompiled code (System V AMD64 ABI, Linux).
;;
;; While recompiled code runs, [rsp] holds the _stack * (guest esp is kept in r11d).
;; rbx and rbp are used by recompiled code as ebx/ebp, so they are saved here.

%include "x64inc.inc"

global c_call_guest_regs
global c_call_guest_cdecl

%ifidn __OUTPUT_FORMAT__, elf64
section .note.GNU-stack noalloc noexec nowrite progbits
section .text progbits alloc exec nowrite align=16
%else
section .text code align=16
%endif

; uint32_t c_call_guest_regs(_stack *stack, uint32_t func, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
;   rdi = stack, esi = func, edx = eax, ecx = edx, r8d = ebx, r9d = ecx
align 16
c_call_guest_regs:
        push rbp
        push rbx
        push rdi                ; _stack * at [rsp] (stack is now aligned to 16 bytes)
        mov r11d, [rdi]         ; guest stack pointer
        mov r10d, esi           ; function
        mov eax, edx
        mov edx, ecx
        mov ebx, r8d
        mov ecx, r9d
        xor esi, esi
        xor edi, edi
        xor ebp, ebp
        CALL r10
        pop rdi
        mov [rdi], r11d
        pop rbx
        pop rbp
        ret

; uint32_t c_call_guest_cdecl(_stack *stack, uint32_t func, uint32_t nargs, const uint32_t *args)
;   rdi = stack, esi = func, edx = nargs, rcx = args (args[0] is the first argument)
align 16
c_call_guest_cdecl:
        push rbp
        push rbx
        push rdi
        push r12
        push r13
        sub rsp, 8
        push rdi                ; _stack * at [rsp] (16-byte aligned)
        mov r11d, [rdi]
        mov r10d, esi
        mov r12d, edx           ; number of arguments (callee-saved)
    .push_loop:
        test edx, edx
        jz short .pushed
        dec edx
        mov r8d, [rcx+4*rdx]
        sub r11d, byte 4
        mov [r11d], r8d
        jmp short .push_loop
    .pushed:
        xor eax, eax
        xor ebx, ebx
        xor ecx, ecx
        xor edx, edx
        xor esi, esi
        xor edi, edi
        xor ebp, ebp
        CALL r10
        lea r11d, [r11d+4*r12d] ; caller removes the arguments
        pop rdi
        mov [rdi], r11d
        add rsp, 8
        pop r13
        pop r12
        pop rdi
        pop rbx
        pop rbp
        ret
