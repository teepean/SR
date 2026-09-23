;;
;;  Hand-written replacements for MSVCRT helpers with non-standard calling conventions.
;;






;; x86-64 (OUT_X64) version: hand-written import stubs

%include "x64inc.inc"

global _ftol_asm2c
global __CxxFrameHandler_asm2c
global _except_handler3_asm2c

extern Raw_UnexpectedHandler

%ifidn __OUTPUT_FORMAT__, elf64
section .note.GNU-stack noalloc noexec nowrite progbits
section .text progbits alloc exec nowrite align=16
%else
%error "only elf64 (SysV) is supported"
%endif

align 16
_ftol_asm2c:

; st0    = value
; [r11d] = return address (emulated x86 stack)
; result: edx:eax = (int64_t) value (truncated), st0 popped
; uses the red zone below rsp as scratch memory

        fnstcw [rsp-4]
        mov ax, [rsp-4]
        or ax, 0x0c00               ; rounding control = truncate
        mov [rsp-2], ax
        fldcw [rsp-2]
        fistp qword [rsp-16]
        fldcw [rsp-4]
        mov eax, [rsp-16]
        mov edx, [rsp-12]
        RET

; end procedure _ftol_asm2c


align 16
__CxxFrameHandler_asm2c:

; Only called by the Win32 exception dispatcher, which doesn't exist here.

        and rsp, byte -16
        mov edi, 1
        call Raw_UnexpectedHandler

; end procedure __CxxFrameHandler_asm2c


align 16
_except_handler3_asm2c:

; Only called by the Win32 exception dispatcher, which doesn't exist here.

        and rsp, byte -16
        mov edi, 2
        call Raw_UnexpectedHandler

; end procedure _except_handler3_asm2c
