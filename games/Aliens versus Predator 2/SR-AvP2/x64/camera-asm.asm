;;
;;  Wide-screen Hor+ (camera.c): the engine's ILTClient GetCameraFOV / SetCameraFOV, cshell's CreateClientShell and
;;  GetClientShellFunctions
;;  (cdecl, called through function pointers) -> C functions that get a pointer to the arguments on the game's stack
;;  (as time-asm.asm's time_GetTime). eax is the C function's result.
;;

%include "x64inc.inc"
%include "asm-calls.inc"

%ifidn __OUTPUT_FORMAT__, elf64
section .note.GNU-stack noalloc noexec nowrite progbits
section .text progbits alloc exec nowrite align=16
%else
section .text code align=16
%endif

extern avp2_camera_get_c
extern avp2_camera_set_c
extern avp2_create_client_shell_c
extern avp2_get_client_shell_functions_c
global avp2_camera_get_asm2c
global avp2_camera_set_asm2c
global avp2_create_client_shell_asm2c
global avp2_get_client_shell_functions_asm2c

;; cdecl stub: name, C function (one argument: the address of the arguments)
%macro ARGS_STUB 2
align 16
%1:
%ifidn __OUTPUT_FORMAT__, win64
        mov r10, [rsp+FIRST_PARAMETER_OFFSET]
        mov [r10], r11d
        lea ecx, [r11d+4]       ; above the return address
        call %2
        mov r10, [rsp+FIRST_PARAMETER_OFFSET]
        mov r11d, [r10]
        mov r8d, [r11d]
        add r11d, byte 4
        jmp r8
%else
        Call_Asm_Prologue_0
        lea edi, [r11d+12]      ; above the saved esi, edi and the return address
        call %2
        Call_Asm_Epilogue_0 0
%endif
%endmacro

ARGS_STUB avp2_camera_get_asm2c, avp2_camera_get_c
ARGS_STUB avp2_camera_set_asm2c, avp2_camera_set_c
ARGS_STUB avp2_create_client_shell_asm2c, avp2_create_client_shell_c
ARGS_STUB avp2_get_client_shell_functions_asm2c, avp2_get_client_shell_functions_c
