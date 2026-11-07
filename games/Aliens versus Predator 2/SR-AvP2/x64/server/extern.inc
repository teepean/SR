;; module-specific definitions for the recompiled server module (AvP2 1.0.9.6 LithTech server, server.dll)

%include "imports.inc"

; raw-asm.asm: SRW helpers for code that would fault on Windows too (software interrupts, absolute low addresses)
extern x86_int
extern x86_mov_reg_mem_16
extern x86_mov_reg_mem_32
extern x86_mov_mem_reg_16
extern x86_mov_mem_reg_32
