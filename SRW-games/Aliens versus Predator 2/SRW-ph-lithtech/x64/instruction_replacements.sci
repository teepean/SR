loc_4C1B25,7,mov r10d, 0x0|CALL x86_read_fs_dword|cmp esi, r9d ; CRT SEH unwind (_longjmp): cmp esi, fs:[0] - SRW translates only mov/push/pop with fs:
loc_4A6520,6,CALL avp2_time_init_asm2c ; time_Init: the engine clock at a high resolution (x64/time-asm.asm) - call timeGetTime
loc_4A6533,36,CALL avp2_time_get_asm2c|fld qword [r11d] ; time_GetTime: (timeGetTime() - base) * 0.001 in whole ms -> the time as a double in its stack scratch
