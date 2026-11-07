loc_4B91BD,7,mov r10d, 0x0|CALL x86_read_fs_dword|cmp esi, r9d ; CRT SEH unwind (_local_unwind2 area): cmp esi, fs:[0] - SRW translates only mov/push/pop with fs:
