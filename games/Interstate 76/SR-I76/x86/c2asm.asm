;; x86 (32-bit): calls from C into recompiled game code, same interface as x64/c2asm.asm
;;
;; uint32_t c_call_asm_n(void *stack (unused), uint32_t func, uint32_t nargs, const uint32_t *args)
;;   pushes args[nargs-1] .. args[0] and calls func (cdecl or stdcall: esp is restored afterwards); returns eax.

%ifidn __OUTPUT_FORMAT__, win32
    %define c_call_asm_n _c_call_asm_n
%endif

global c_call_asm_n

%ifidn __OUTPUT_FORMAT__, elf32
section .note.GNU-stack noalloc noexec nowrite progbits
section .text progbits alloc exec nowrite align=16
%else
section .text code align=16
%endif

align 16
c_call_asm_n:
        push ebp
        push ebx
        push esi
        push edi
        mov ebp, esp
        mov ecx, [ebp+28]       ; nargs
        mov edx, [ebp+32]       ; args
        test ecx, ecx
        jz .call
.push:
        push dword [edx+ecx*4-4]
        dec ecx
        jnz .push
.call:
        call [ebp+24]           ; func
        mov esp, ebp            ; restore esp (cdecl or stdcall)
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; end procedure c_call_asm_n
