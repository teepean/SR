loc_499997,53,mov word [ebp-0x4], 0 ; CPU speed detection via PIT port I/O (Win9x only) - measure 0 ticks, function returns -1 and caller assumes 200 MHz
loc_4B2E8A,6,cmp ebx, loc_6593E0 + 0x10C00 ; end of the ZIX directory table (256 * 268 bytes) - the address lies beyond the section's VirtualSize, so it can't be a relocation
loc_4B2EFB,6,cmp ebx, loc_6593E0 + 0x10C00 ; end of the ZIX directory table (see above)
loc_49C929,6,call i76_frame_tick ; frame limiter (GetTickCount in the per-frame timer)
loc_467400,8,mov eax, [esp+4]|push esi|test eax, eax|jz loc_467431|mov esi, [eax+0x70]|test esi, esi|jz loc_467431 ; game bug: mission script opcode 0x5A (sub_412CE0) passes a NULL object (crash at the start of mission 13 after getdown)
