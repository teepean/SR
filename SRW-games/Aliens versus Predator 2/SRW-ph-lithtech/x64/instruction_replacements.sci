loc_4C1B25,7,mov r10d, 0x0|CALL x86_read_fs_dword|cmp esi, r9d ; CRT SEH unwind (_longjmp): cmp esi, fs:[0] - SRW translates only mov/push/pop with fs:
