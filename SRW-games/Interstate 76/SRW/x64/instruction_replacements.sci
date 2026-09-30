loc_499997,53,mov word [ebp-0x4], 0 ; CPU speed detection via PIT port I/O (Win9x only) - measure 0 ticks, function returns -1 and caller assumes 200 MHz
loc_4B2E8A,6,cmp ebx, loc_6593E0 + 0x10C00 ; end of the ZIX directory table (256 * 268 bytes) - the address lies beyond the section's VirtualSize, so it can't be a relocation
loc_4B2EFB,6,cmp ebx, loc_6593E0 + 0x10C00 ; end of the ZIX directory table (see above)
loc_49C929,6,CALL i76_frame_tick ; frame limiter (GetTickCount in the per-frame timer)
loc_467400,8,mov eax, [r11d+4]|PUSH32 esi|test eax, eax|jz loc_467431|mov esi, [eax+0x70]|test esi, esi|jz loc_467431 ; game bug: mission script opcode 0x5A (sub_412CE0) passes a NULL object (crash at the start of mission 13 after getdown)
loc_43411E,5,mov esi, [i76_screen_width] ; widescreen: sub_434100 (Glide device setup) - screen width i76_screen_width instead of 640 (ZGLIDE clips to it, dword_608400)
loc_434178,8,mov r8d, [i76_screen_width]|add r8d, r8d|mov [r11d+0x68], r8d ; widescreen: device line length 2 * width instead of 1280
loc_434188,8,mov r8d, [i76_screen_width]|dec r8d|mov [r11d+0x7c], r8d ; widescreen: device rectangle right edge width - 1 instead of 639
loc_47244A,1,inc edx|cmp edx, [i76_screen_width]|cmove edx, [i76_width_4x3] ; widescreen (Hor+): sub_472400 computes the focal length from the viewport width - full-width views use the 4:3 width (same vertical view, wider horizontal view)
loc_433E16,5,mov eax, [i76_screen_width] ; widescreen: sub_433DF0 (Glide renderer setup) - device width (+0, +108; the 3D viewport follows) i76_screen_width instead of 640
loc_4343A4,5,mov r8d, [i76_screen_width]|dec r8d|PUSH32 r8d ; widescreen: sub_434340 (display surface of a frame, also the 3D viewport) - right edge width - 1 instead of 639; 2D drawing stays centered (glide.c offsets the LFB pointer)
loc_47245A,3,fld dword [esi+0x30]|jne ws_fy_4x3|fmul dword [i76_aspect_scale]|ws_fy_4x3: ; widescreen (Hor+): sub_472400 - fy uses the aspect factor (height * 4 / (width * 3) from the callers) * width / 640 for full-width views, i.e. square pixels with the 4:3 focal length (ZF from the width compare at loc_47244A; only FPU instructions in between)
loc_42CDFB,8,mov r8d, [i76_screen_width_f]|mov [r11d+0x48], r8d ; widescreen: sub_42CD90 (1-pixel border strips after the rear mirror rendering: bottom row, right column) - 640.0 -> screen width
loc_42CE44,8,mov r8d, [i76_screen_width_f]|mov [r11d+0x6c], r8d ; widescreen: sub_42CD90 640.0 -> screen width
loc_42CF18,8,mov r8d, [i76_screen_right_f]|mov [r11d+0x10], r8d ; widescreen: sub_42CD90 639.0 -> screen width - 1 (otherwise a line in the middle of the view)
loc_42CF61,8,mov r8d, [i76_screen_width_f]|mov [r11d+0x34], r8d ; widescreen: sub_42CD90 640.0 -> screen width
loc_42CFBA,8,mov r8d, [i76_screen_width_f]|mov [r11d+0x6c], r8d ; widescreen: sub_42CD90 640.0 -> screen width
loc_42D018,11,mov r8d, [i76_screen_right_f]|mov [r11d+0x90], r8d ; widescreen: sub_42CD90 639.0 -> screen width - 1
