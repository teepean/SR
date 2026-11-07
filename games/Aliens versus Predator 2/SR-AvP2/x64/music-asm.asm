;;
;;  The engine's DirectMusic manager replaced (music.c): the game calls CLTDirectMusicMgr's virtual methods
;;  (thiscall: `this` in ecx, the callee pops the arguments); music.c points the engine's vtable at these stubs.
;;  Each passes `this` and a pointer to the stack arguments to its C function (music_<method>_c).
;;

%include "x64inc.inc"
%include "asm-calls.inc"

%ifidn __OUTPUT_FORMAT__, elf64
section .note.GNU-stack noalloc noexec nowrite progbits
section .text progbits alloc exec nowrite align=16
%else
section .text code align=16
%endif

;; name, number of stack arguments: music_<name>_asm2c calls uint32_t music_<name>_c(uint32_t self, const uint32_t *args)
%macro MUSIC_STUB 2
extern music_%1_c
global music_%1_asm2c
align 16
music_%1_asm2c:
%ifidn __OUTPUT_FORMAT__, win64
        mov r10, [rsp+FIRST_PARAMETER_OFFSET]
        mov [r10], r11d
        mov ecx, ecx            ; this
        lea edx, [r11d+4]       ; the arguments
        call music_%1_c
        mov r10, [rsp+FIRST_PARAMETER_OFFSET]
        mov r11d, [r10]
        mov r8d, [r11d]
        add r11d, byte 4+(%2*4)
        jmp r8
%else
        Call_Asm_Prologue_0
        mov edi, ecx            ; this
        lea esi, [r11d+12]      ; the arguments (above the saved esi, edi and the return address)
        call music_%1_c
        Call_Asm_Epilogue_0 %2
%endif
%endmacro

MUSIC_STUB Init, 0
MUSIC_STUB Term, 0
MUSIC_STUB InitLevel, 5
MUSIC_STUB TermLevel, 0
MUSIC_STUB Play, 0
MUSIC_STUB Stop, 1
MUSIC_STUB Pause, 1
MUSIC_STUB UnPause, 0
MUSIC_STUB SetVolume, 1
MUSIC_STUB ChangeIntensity, 2
MUSIC_STUB PlaySecondary, 2
MUSIC_STUB StopSecondary, 2
MUSIC_STUB PlayMotif3, 3
MUSIC_STUB StopMotif3, 3
MUSIC_STUB GetCurIntensity, 0
MUSIC_STUB StringToEnactType, 1
MUSIC_STUB EnactTypeToString, 2
MUSIC_STUB GetNumIntensities, 0
MUSIC_STUB GetInitialIntensity, 0
MUSIC_STUB GetInitialVolume, 0
MUSIC_STUB GetVolumeOffset, 0
MUSIC_STUB PlayMotif2, 2
MUSIC_STUB StopMotif2, 2
