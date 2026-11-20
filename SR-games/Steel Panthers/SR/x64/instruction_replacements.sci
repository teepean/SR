loc_AD997,5,nop ; CLIB _fpinit: never install the x87 emulator (smsw/sidt/INT 7 hook at loc_AD9B2) - an FPU is always present
loc_BEC58,3,mov eax, 0xffffffff ; CLIB heap: lsl eax, ds -> flat 4 GB data segment limit
loc_BEE68,3,mov eax, 0xffffffff ; CLIB heap: lsl eax, ds -> flat 4 GB data segment limit
loc_A08A7,5,;ret|RET ; CLIB YI: x87 emulator uninstall (cr0, INT 7 restore) - emulator is never installed

loc_28AC6,7,mov dword [ebp-0xc], sp_bios_ica ; mouse state in BIOS ICA 0:4F0 (buttons) -> runtime variable
loc_28AD6,7,mov dword [ebp-0x4], sp_bios_ica+2 ; mouse state in BIOS ICA 0:4F2 (x) -> runtime variable
loc_28AE6,7,mov dword [ebp-0x8], sp_bios_ica+4 ; mouse state in BIOS ICA 0:4F4 (y) -> runtime variable
loc_28BB0,7,mov dword [ebp-0x4], sp_bios_ica ; BIOS ICA 0:4F0
loc_28BD7,7,mov dword [ebp-0x8], sp_bios_ica ; BIOS ICA 0:4F0
loc_28BFE,7,mov dword [ebp-0xc], sp_bios_ica ; BIOS ICA 0:4F0
loc_28C25,7,mov dword [ebp-0x10], sp_bios_ica+2 ; BIOS ICA 0:4F2
loc_28C3A,7,mov dword [ebp-0x14], sp_bios_ica+4 ; BIOS ICA 0:4F4
loc_28CC5,7,mov dword [ebp-0x18], sp_bios_ica+2 ; BIOS ICA 0:4F2
loc_28CDA,7,mov dword [ebp-0x1c], sp_bios_ica+4 ; BIOS ICA 0:4F4

loc_C1ADF,5,shr eax, 2|mov r9d, [eax + sp_jt_C1AF0]|jmp r9|align 4|sp_jt_C1AF0:|dd loc_C1AF0, loc_C1B00, loc_C1B10, loc_C1B20, loc_C1B30, loc_C1B40, loc_C1B50, loc_C1B60, loc_C1B70, loc_C1B80, loc_C1B90, loc_C1BA0, loc_C1BB0, loc_C1BC0, loc_C1BD0, loc_C1BE0 ; computed jump base+16*k -> table of recompiled labels
loc_C1AE4,2, ; jmp eax (replaced above)
loc_C1BFB,5,shr eax, 2|mov r9d, [eax + sp_jt_C1C10]|jmp r9|align 4|sp_jt_C1C10:|dd loc_C1C10, loc_C1C20, loc_C1C30, loc_C1C40, loc_C1C50, loc_C1C60, loc_C1C70, loc_C1C80, loc_C1C90, loc_C1CA0, loc_C1CB0, loc_C1CC0, loc_C1CD0, loc_C1CE0, loc_C1CF0, loc_C1D00 ; computed jump base+16*k -> table of recompiled labels
loc_C1C00,2, ; jmp eax (replaced above)
loc_C1D1B,5,shr eax, 2|mov r9d, [eax + sp_jt_C1D30]|jmp r9|align 4|sp_jt_C1D30:|dd loc_C1D30, loc_C1D40, loc_C1D50, loc_C1D60, loc_C1D70, loc_C1D80, loc_C1D90, loc_C1DA0, loc_C1DB0, loc_C1DC0, loc_C1DD0, loc_C1DE0, loc_C1DF0, loc_C1E00, loc_C1E10, loc_C1E20 ; computed jump base+16*k -> table of recompiled labels
loc_C1D20,2, ; jmp eax (replaced above)
loc_C1E3C,5,shr eax, 2|mov r9d, [eax + sp_jt_C1E50]|jmp r9|align 4|sp_jt_C1E50:|dd loc_C1E50, loc_C1E60, loc_C1E70, loc_C1E80, loc_C1E90, loc_C1EA0, loc_C1EB0, loc_C1EC0, loc_C1ED0, loc_C1EE0, loc_C1EF0, loc_C1F00, loc_C1F10, loc_C1F20, loc_C1F30, loc_C1F40 ; computed jump base+16*k -> table of recompiled labels
loc_C1E41,2, ; jmp eax (replaced above)
loc_C2C9F,5,shr eax, 2|mov r9d, [eax + sp_jt_C2CB0]|jmp r9|align 4|sp_jt_C2CB0:|dd loc_C2CB0, loc_C2CC0, loc_C2CD0, loc_C2CE0, loc_C2CF0, loc_C2D00, loc_C2D10, loc_C2D20, loc_C2D30, loc_C2D40, loc_C2D50, loc_C2D60, loc_C2D70, loc_C2D80, loc_C2D90, loc_C2DA0 ; computed jump base+16*k -> table of recompiled labels
loc_C2CA4,2, ; jmp eax (replaced above)
loc_C2DBB,5,shr eax, 2|mov r9d, [eax + sp_jt_C2DD0]|jmp r9|align 4|sp_jt_C2DD0:|dd loc_C2DD0, loc_C2DE0, loc_C2DF0, loc_C2E00, loc_C2E10, loc_C2E20, loc_C2E30, loc_C2E40, loc_C2E50, loc_C2E60, loc_C2E70, loc_C2E80, loc_C2E90, loc_C2EA0, loc_C2EB0, loc_C2EC0 ; computed jump base+16*k -> table of recompiled labels
loc_C2DC0,2, ; jmp eax (replaced above)
loc_C2EDB,5,shr eax, 2|mov r9d, [eax + sp_jt_C2EF0]|jmp r9|align 4|sp_jt_C2EF0:|dd loc_C2EF0, loc_C2F00, loc_C2F10, loc_C2F20, loc_C2F30, loc_C2F40, loc_C2F50, loc_C2F60, loc_C2F70, loc_C2F80, loc_C2F90, loc_C2FA0, loc_C2FB0, loc_C2FC0, loc_C2FD0, loc_C2FE0 ; computed jump base+16*k -> table of recompiled labels
loc_C2EE0,2, ; jmp eax (replaced above)
loc_C2FFC,5,shr eax, 2|mov r9d, [eax + sp_jt_C3010]|jmp r9|align 4|sp_jt_C3010:|dd loc_C3010, loc_C3020, loc_C3030, loc_C3040, loc_C3050, loc_C3060, loc_C3070, loc_C3080, loc_C3090, loc_C30A0, loc_C30B0, loc_C30C0, loc_C30D0, loc_C30E0, loc_C30F0, loc_C3100 ; computed jump base+16*k -> table of recompiled labels
loc_C3001,2, ; jmp eax (replaced above)
