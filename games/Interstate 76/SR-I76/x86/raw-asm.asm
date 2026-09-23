;;
;;  Hand-written replacements for MSVCRT helpers with non-standard calling conventions.
;;

%ifidn __OUTPUT_FORMAT__, win32
    %define Raw_UnexpectedHandler _Raw_UnexpectedHandler
%endif

extern Raw_UnexpectedHandler

global _ftol_asm2c
global __CxxFrameHandler_asm2c
global _except_handler3_asm2c

%ifidn __OUTPUT_FORMAT__, elf32
section .note.GNU-stack noalloc noexec nowrite progbits
section .text progbits alloc exec nowrite align=16
%else
section .text code align=16
%endif

align 16
_ftol_asm2c:

; st0   = value
; [esp] = return address
; result: edx:eax = (int64_t) value (truncated), st0 popped

        sub esp, byte 12
        fnstcw [esp]
        mov ax, [esp]
        or ax, 0x0c00               ; rounding control = truncate
        mov [esp + 2], ax
        fldcw [esp + 2]
        fistp qword [esp + 4]
        fldcw [esp]
        mov eax, [esp + 4]
        mov edx, [esp + 8]
        add esp, byte 12
        retn

; end procedure _ftol_asm2c


align 16
__CxxFrameHandler_asm2c:

; Only called by the Win32 exception dispatcher, which doesn't exist here.

        and esp, 0FFFFFFF0h
        sub esp, byte 12
        push dword 1
        call Raw_UnexpectedHandler

; end procedure __CxxFrameHandler_asm2c


align 16
_except_handler3_asm2c:

; Only called by the Win32 exception dispatcher, which doesn't exist here.

        and esp, 0FFFFFFF0h
        sub esp, byte 12
        push dword 2
        call Raw_UnexpectedHandler

; end procedure _except_handler3_asm2c
