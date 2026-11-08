loc_4B91BD,7,mov r10d, 0x0|CALL x86_read_fs_dword|cmp esi, r9d ; CRT SEH unwind (_local_unwind2 area): cmp esi, fs:[0] - SRW translates only mov/push/pop with fs:
loc_49EF00,6,CALL avp2_time_init_asm2c ; time_Init: the engine clock at a high resolution (x64/time-asm.asm) - call timeGetTime
loc_49EF13,36,CALL avp2_time_get_asm2c|fld qword [r11d] ; time_GetTime: (timeGetTime() - base) * 0.001 in whole ms -> the time as a double in its stack scratch
