;; module-specific definitions for the recompiled cshell module (Primal Hunt 1.0.2.2 client shell, CShell.DLL - stand-in for the native build)

%include "imports.inc"

; raw-asm.asm: SRW helpers for code that would fault on Windows too (software interrupts, absolute low addresses)
extern x86_int
extern x86_mov_reg_mem_16
extern x86_mov_reg_mem_32
extern x86_mov_mem_reg_16
extern x86_mov_mem_reg_32
