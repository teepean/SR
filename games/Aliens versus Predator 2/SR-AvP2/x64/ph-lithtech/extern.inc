;; module-specific definitions for the recompiled lithtech module (Primal Hunt 1.0.2.2 engine)

%include "imports.inc"

; raw-asm.asm: SRW helpers for code that would fault on Windows too (software interrupts, absolute low addresses)
extern x86_int
extern x86_mov_reg_mem_16
extern x86_mov_reg_mem_32
extern x86_mov_mem_reg_16
extern x86_mov_mem_reg_32

; x64/time-asm.asm: the engine clock at a high resolution (instruction_replacements.sci)
extern avp2_time_init_asm2c
extern avp2_time_get_asm2c
