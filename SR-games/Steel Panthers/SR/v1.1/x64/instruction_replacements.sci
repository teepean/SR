loc_A432D,5,nop ; CLIB _fpinit: never install the x87 emulator (smsw/sidt/INT 7 hook at loc_A4348) - an FPU is always present
loc_B4FBD,3,mov eax, 0xffffffff ; CLIB heap: lsl eax, ds -> flat 4 GB data segment limit
loc_B51CD,3,mov eax, 0xffffffff ; CLIB heap: lsl eax, ds -> flat 4 GB data segment limit
loc_977D7,5,;ret|RET ; CLIB YI: x87 emulator uninstall (cr0, INT 7 restore) - emulator is never installed

loc_2766E,7,mov dword [ebp-0xc], sp_bios_ica ; mouse state in BIOS ICA 0:4F0 (buttons) -> runtime variable
loc_2767E,7,mov dword [ebp-0x4], sp_bios_ica+2 ; mouse state in BIOS ICA 0:4F2 (x) -> runtime variable
loc_2768E,7,mov dword [ebp-0x8], sp_bios_ica+4 ; mouse state in BIOS ICA 0:4F4 (y) -> runtime variable
loc_27758,7,mov dword [ebp-0x4], sp_bios_ica ; BIOS ICA 0:4F0
loc_2777F,7,mov dword [ebp-0x8], sp_bios_ica ; BIOS ICA 0:4F0
loc_277A6,7,mov dword [ebp-0xc], sp_bios_ica ; BIOS ICA 0:4F0
loc_277CD,7,mov dword [ebp-0x10], sp_bios_ica+2 ; BIOS ICA 0:4F2
loc_277E2,7,mov dword [ebp-0x14], sp_bios_ica+4 ; BIOS ICA 0:4F4
loc_2786D,7,mov dword [ebp-0x18], sp_bios_ica+2 ; BIOS ICA 0:4F2
loc_27882,7,mov dword [ebp-0x1c], sp_bios_ica+4 ; BIOS ICA 0:4F4

loc_B7E3F,5,shr eax, 2|mov r9d, [eax + sp_jt_B7E50]|jmp r9|align 4|sp_jt_B7E50:|dd loc_B7E50, loc_B7E60, loc_B7E70, loc_B7E80, loc_B7E90, loc_B7EA0, loc_B7EB0, loc_B7EC0, loc_B7ED0, loc_B7EE0, loc_B7EF0, loc_B7F00, loc_B7F10, loc_B7F20, loc_B7F30, loc_B7F40 ; computed jump base+16*k -> table of recompiled labels
loc_B7E44,2, ; jmp eax (replaced above)
loc_B7F5B,5,shr eax, 2|mov r9d, [eax + sp_jt_B7F70]|jmp r9|align 4|sp_jt_B7F70:|dd loc_B7F70, loc_B7F80, loc_B7F90, loc_B7FA0, loc_B7FB0, loc_B7FC0, loc_B7FD0, loc_B7FE0, loc_B7FF0, loc_B8000, loc_B8010, loc_B8020, loc_B8030, loc_B8040, loc_B8050, loc_B8060 ; computed jump base+16*k -> table of recompiled labels
loc_B7F60,2, ; jmp eax (replaced above)
loc_B807B,5,shr eax, 2|mov r9d, [eax + sp_jt_B8090]|jmp r9|align 4|sp_jt_B8090:|dd loc_B8090, loc_B80A0, loc_B80B0, loc_B80C0, loc_B80D0, loc_B80E0, loc_B80F0, loc_B8100, loc_B8110, loc_B8120, loc_B8130, loc_B8140, loc_B8150, loc_B8160, loc_B8170, loc_B8180 ; computed jump base+16*k -> table of recompiled labels
loc_B8080,2, ; jmp eax (replaced above)
loc_B819C,5,shr eax, 2|mov r9d, [eax + sp_jt_B81B0]|jmp r9|align 4|sp_jt_B81B0:|dd loc_B81B0, loc_B81C0, loc_B81D0, loc_B81E0, loc_B81F0, loc_B8200, loc_B8210, loc_B8220, loc_B8230, loc_B8240, loc_B8250, loc_B8260, loc_B8270, loc_B8280, loc_B8290, loc_B82A0 ; computed jump base+16*k -> table of recompiled labels
loc_B81A1,2, ; jmp eax (replaced above)
loc_B8FFF,5,shr eax, 2|mov r9d, [eax + sp_jt_B9010]|jmp r9|align 4|sp_jt_B9010:|dd loc_B9010, loc_B9020, loc_B9030, loc_B9040, loc_B9050, loc_B9060, loc_B9070, loc_B9080, loc_B9090, loc_B90A0, loc_B90B0, loc_B90C0, loc_B90D0, loc_B90E0, loc_B90F0, loc_B9100 ; computed jump base+16*k -> table of recompiled labels
loc_B9004,2, ; jmp eax (replaced above)
loc_B911B,5,shr eax, 2|mov r9d, [eax + sp_jt_B9130]|jmp r9|align 4|sp_jt_B9130:|dd loc_B9130, loc_B9140, loc_B9150, loc_B9160, loc_B9170, loc_B9180, loc_B9190, loc_B91A0, loc_B91B0, loc_B91C0, loc_B91D0, loc_B91E0, loc_B91F0, loc_B9200, loc_B9210, loc_B9220 ; computed jump base+16*k -> table of recompiled labels
loc_B9120,2, ; jmp eax (replaced above)
loc_B923B,5,shr eax, 2|mov r9d, [eax + sp_jt_B9250]|jmp r9|align 4|sp_jt_B9250:|dd loc_B9250, loc_B9260, loc_B9270, loc_B9280, loc_B9290, loc_B92A0, loc_B92B0, loc_B92C0, loc_B92D0, loc_B92E0, loc_B92F0, loc_B9300, loc_B9310, loc_B9320, loc_B9330, loc_B9340 ; computed jump base+16*k -> table of recompiled labels
loc_B9240,2, ; jmp eax (replaced above)
loc_B935C,5,shr eax, 2|mov r9d, [eax + sp_jt_B9370]|jmp r9|align 4|sp_jt_B9370:|dd loc_B9370, loc_B9380, loc_B9390, loc_B93A0, loc_B93B0, loc_B93C0, loc_B93D0, loc_B93E0, loc_B93F0, loc_B9400, loc_B9410, loc_B9420, loc_B9430, loc_B9440, loc_B9450, loc_B9460 ; computed jump base+16*k -> table of recompiled labels
loc_B9361,2, ; jmp eax (replaced above)
