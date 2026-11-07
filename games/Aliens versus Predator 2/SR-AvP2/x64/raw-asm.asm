;;
;;  Hand-written replacements for MSVCRT helpers with non-standard calling conventions.
;;






;; x86-64 (OUT_X64) version: hand-written import stubs

%include "x64inc.inc"

global _ftol_asm2c
global __CxxFrameHandler_asm2c
global _except_handler3_asm2c
; SRW's helpers for x86 code it can't translate directly: software interrupts and accesses to absolute low
; addresses (dead paths in AvP2's engine - the original would fault there too)
global x86_int
global x86_mov_reg_mem_16
global x86_mov_reg_mem_32
global x86_mov_mem_reg_16
global x86_mov_mem_reg_32

extern Raw_UnexpectedHandler

%ifidn __OUTPUT_FORMAT__, elf64
section .note.GNU-stack noalloc noexec nowrite progbits
section .text progbits alloc exec nowrite align=16
%else
section .text code align=16
%endif

; scratch memory: the red zone below rsp (SysV) / the free parameter slots above rsp (Win64, see asm_unwind.inc)
%ifidn __OUTPUT_FORMAT__, win64
%define SCRATCH(x) (rsp+16+(x))
%else
%define SCRATCH(x) (rsp+(x))
%endif

align 16
_ftol_asm2c:

; st0    = value
; [r11d] = return address (emulated x86 stack)
; result: edx:eax = (int64_t) value (truncated), st0 popped

        fnstcw [SCRATCH(-4)]
        mov ax, [SCRATCH(-4)]
        or ax, 0x0c00               ; rounding control = truncate
        mov [SCRATCH(-2)], ax
        fldcw [SCRATCH(-2)]
        fistp qword [SCRATCH(-16)]
        fldcw [SCRATCH(-4)]
        mov eax, [SCRATCH(-16)]
        mov edx, [SCRATCH(-12)]
        RET

; end procedure _ftol_asm2c


align 16
__CxxFrameHandler_asm2c:

; Only called by the Win32 exception dispatcher, which doesn't exist here.

        and rsp, byte -16
%ifidn __OUTPUT_FORMAT__, win64
        sub rsp, byte 32
        mov ecx, 1
%else
        mov edi, 1
%endif
        call Raw_UnexpectedHandler

; end procedure __CxxFrameHandler_asm2c


align 16
_except_handler3_asm2c:

; Only called by the Win32 exception dispatcher, which doesn't exist here.

        and rsp, byte -16
%ifidn __OUTPUT_FORMAT__, win64
        sub rsp, byte 32
        mov ecx, 2
%else
        mov edi, 2
%endif
        call Raw_UnexpectedHandler

; end procedure _except_handler3_asm2c


%macro UNEXPECTED 2
align 16
%1:
        and rsp, byte -16
%ifidn __OUTPUT_FORMAT__, win64
        sub rsp, byte 32
        mov ecx, %2
%else
        mov edi, %2
%endif
        call Raw_UnexpectedHandler
%endmacro

UNEXPECTED x86_int, 3
UNEXPECTED x86_mov_reg_mem_16, 4
UNEXPECTED x86_mov_reg_mem_32, 4
UNEXPECTED x86_mov_mem_reg_16, 4
UNEXPECTED x86_mov_mem_reg_32, 4
