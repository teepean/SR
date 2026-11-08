;;
;;  The engine's clock at a high resolution: LithTech's time_Init / time_GetTime (seconds since time_Init, from
;;  timeGetTime's whole milliseconds) call these instead of timeGetTime (SRW-lithtech / SRW-ph-lithtech
;;  instruction_replacements.sci). time_GetTime keeps its 8-byte stack scratch: avp2_time_get_c writes the time
;;  there as a double and the replacement loads it (fld qword [r11d]) - nothing shared between threads.
;;

%include "x64inc.inc"
%include "asm-calls.inc"

%ifidn __OUTPUT_FORMAT__, elf64
section .note.GNU-stack noalloc noexec nowrite progbits
section .text progbits alloc exec nowrite align=16
%else
section .text code align=16
%endif

extern avp2_time_init_c
extern avp2_time_get_c
global avp2_time_init_asm2c
global avp2_time_get_asm2c

; uint32_t avp2_time_init_c(void): returns timeGetTime's value (the engine still stores it as its base)
align 16
avp2_time_init_asm2c:
%ifidn __OUTPUT_FORMAT__, win64
        mov r10, [rsp+FIRST_PARAMETER_OFFSET]
        mov [r10], r11d
        call avp2_time_init_c
        mov r10, [rsp+FIRST_PARAMETER_OFFSET]
        mov r11d, [r10]
        mov r8d, [r11d]
        add r11d, byte 4
        jmp r8
%else
        Call_Asm_Prologue_0
        call avp2_time_init_c
        Call_Asm_Epilogue_0 0
%endif

; void avp2_time_get_c(uint32_t out): the caller's [esp] (8 bytes) receives the time as a double
align 16
avp2_time_get_asm2c:
%ifidn __OUTPUT_FORMAT__, win64
        mov r10, [rsp+FIRST_PARAMETER_OFFSET]
        mov [r10], r11d
        lea ecx, [r11d+4]       ; above the return address
        call avp2_time_get_c
        mov r10, [rsp+FIRST_PARAMETER_OFFSET]
        mov r11d, [r10]
        mov r8d, [r11d]
        add r11d, byte 4
        jmp r8
%else
        Call_Asm_Prologue_0
        lea edi, [r11d+12]      ; above the saved esi, edi and the return address
        call avp2_time_get_c
        Call_Asm_Epilogue_0 0
%endif
