;; module-specific definitions for the recompiled cshell module (AvP2 1.0.9.6 client shell, CShell.DLL - stand-in for the native build)

%include "imports.inc"

; raw-asm.asm: SRW helpers for code that would fault on Windows too (software interrupts, absolute low addresses)
extern x86_int
extern x86_mov_reg_mem_16
extern x86_mov_reg_mem_32
extern x86_mov_mem_reg_16
extern x86_mov_mem_reg_32

; camera-asm.asm: the gamma slider on the Display page (gamma.c; instruction_replacements.sci: CFolderDisplay::Build)
extern avp2_display_build_asm2c
