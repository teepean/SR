;;
;;  The renderer's ILTCustomDraw for canvases (game code calls its virtual methods: thiscall, `this` in ecx, the
;;  callee pops the arguments): each method's stub passes ecx and the stack arguments to a C function
;;  (canvas_<method>_c in render3d.cpp).
;;

%include "x64inc.inc"
%include "asm-calls.inc"

extern canvas_DrawPrimitive_c
extern canvas_SetState_c
extern canvas_GetState_c
extern canvas_SetTexture_c
extern canvas_GetTexelSize_c

global canvas_DrawPrimitive_asm2c
global canvas_SetState_asm2c
global canvas_GetState_asm2c
global canvas_SetTexture_asm2c
global canvas_GetTexelSize_asm2c

%ifidn __OUTPUT_FORMAT__, elf64
section .note.GNU-stack noalloc noexec nowrite progbits
section .text progbits alloc exec nowrite align=16
%else
section .text code align=16
%endif

;; thiscall stub: name, C function, number of stack arguments (1-3)
%macro THISCALL_STUB 3
align 16
%1:
%ifidn __OUTPUT_FORMAT__, win64
        mov r10, [rsp+FIRST_PARAMETER_OFFSET]
        mov [r10], r11d
        ; ecx = this
        mov edx, [r11d+4]
%if %3 >= 2
        mov r8d, [r11d+8]
%endif
%if %3 >= 3
        mov r9d, [r11d+12]
%endif
        call %2
        mov r10, [rsp+FIRST_PARAMETER_OFFSET]
        mov r11d, [r10]
        mov r8d, [r11d]
        add r11d, byte 4+(%3*4)
        jmp r8
%else
        Call_Asm_Prologue_0
        mov edi, ecx            ; this
        mov esi, [r11d+12]
%if %3 >= 2
        mov edx, [r11d+16]
%endif
%if %3 >= 3
        mov ecx, [r11d+20]
%endif
        call %2
        Call_Asm_Epilogue_0 %3
%endif
%endmacro

THISCALL_STUB canvas_DrawPrimitive_asm2c, canvas_DrawPrimitive_c, 3
THISCALL_STUB canvas_SetState_asm2c, canvas_SetState_c, 2
THISCALL_STUB canvas_GetState_asm2c, canvas_GetState_c, 2
THISCALL_STUB canvas_SetTexture_asm2c, canvas_SetTexture_c, 1
THISCALL_STUB canvas_GetTexelSize_asm2c, canvas_GetTexelSize_c, 2
