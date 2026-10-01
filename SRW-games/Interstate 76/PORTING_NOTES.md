# Interstate '76 — SRW porting notes

Running log of the static-recompilation port of Interstate '76 (GOG release).
Newest entries at the bottom of the log section.

## Source files

Game directory: `/home/teemu/.wine/drive_c/i76/`

| file | size | md5 | notes |
|---|---|---|---|
| i76.exe | 1050624 | 9a232dcc2c164648cff20c414c1f9698 | main exe, linked 1998-02-20 (MSVC 5.10), relocations stripped |

PE layout of i76.exe (image base 0x400000):

| section | VA | raw size | notes |
|---|---|---|---|
| .text | 0x401000 | 0xbae55 | entry 0x4ba0e0 |
| .rdata | 0x4bc000 | 0x5788 | IAT at 0x4bc000 (0x404) |
| .data | 0x4c2000 | 0x3f800 in file | virtual size extends to 0x66a000 (bss) |
| .rsrc | 0x66a000 | 0x398 | |

Imports: Strlkup.dll, KERNEL32, USER32, GDI32, MSVCRT, anetdll.dll (network),
ADVAPI32 (registry), WIN32.dll (GOG-renamed WINMM: timeGetTime, mci*, joy*, aux*),
DSOUND, smackw32.DLL (by ordinal), DDRAW, IMM32, ole32.

Dynamically loaded (strings): `I76SHELL.DLL`, renderer DLLs found via `*.dll`
(ZDX5DRAW.DLL, ZGLIDE.DLL, ZREDLINE.DLL, zpowervr.dll).

## Tooling

- SRW built on Linux: `cd SRW && scons` (scons in `~/.local/share/sr-venv`).
- nasm 2.16.03 in `~/.local/bin` (built from source; 2.15.03–2.15.05 are broken for SR).
- IDA Pro 9.3 (`/home/teemu/ida-pro-9.3`), Ghidra.

## Research summary

- GitHub issue #70 (M-HT): process = OUT_ORIG test → SCI fixups → recompiled build → I/O
  reimplementation. SRW only disassembles; all Win32/DirectX/etc. must be reimplemented
  (Septerra does this in `games/Septerra Core/SR-Septerra/WinApi-*.c` on SDL2).
- Issue #25: M-HT replaces libraries (C runtime, sound libs) with C, fixes NULL derefs via
  instruction_replacements, patches crazy code (jumps into instructions, SMC).
- Issue #67: rewritten parts can differ entirely from original code.
- Issue #28: x64/arm64 builds exist via llasm / x64 output for Albion and Septerra.
- SRW pipeline for PE files (see `SRW-games/Septerra Core/SRW`):
  - `relocations.csv` required when relocations are stripped: `fixup_va,target_va[,i]`.
    SRW verifies the dword at fixup_va equals target_va.
  - `bssborder.csv`: `addr,align_minus` splits uninitialized tail of .data into .bss.
  - `SR.cfg`: `esp_dword_aligned=yes`, `ebp_dword_aligned=no`.
  - Imports become `extern Name`; the game runtime's `x86/extern.inc` maps
    `%define Name Name_asm2c`; `Name_asm2c` stubs (`Call_Asm_StackN`) call C `Name_c`.
- Baseline check: GOG `septerra.exe` (md5 3e51892504ba28125311ebd77ac7cd0c) runs through
  SRW with the repo's SCI/CSV files without errors.

## Log

### 2026-09-22
- Researched repo + GitHub issues; installed nasm/scons; built SRW; verified Septerra baseline.
- Analysed i76.exe PE headers and imports (above).

### 2026-09-23 — relocation reconstruction, first successful SRW run
- IDAPython needed `idapyswitch --force-path /usr/lib/libpython3.14.so.1.0`. `-S` script paths must not contain spaces.
- Tools in `tools/`: `ida_export.py` (IDA headless export), `classify.py` + `blockstart.py`
  (data-pointer heuristics), `gen_relocs.py` (writes relocations.csv + `.rejected.txt`), `reloc_exclude.txt`.
  Work dir outside the repo: `/home/teemu/sorsa/i76work` (IDA dbs, run-NNN dirs, one per SRW run).
- Validation against Septerra's hand-made relocations.csv (21077 entries):
  code refs 17929/17929 exact (only verify operand dword == decoded addr/value, otherwise
  disp8+imm bytes create bogus refs); data refs → final total FP 21, FN 10.
- Data-pointer rejection rules (classify.py): low 16 bits zero (float halves), inside IDA strings,
  inside float/double items, misaligned in dword/word items, 3 printable bytes pointing into .bss w/o head
  (short strings like "One\0" look like bss pointers because .bss reaches 0x66a000),
  target in .text not an instruction head, target mid-block with no other xrefs, overlaps.
  IDA's own offset flag is NOT trusted (it marked the string "NEC\0" as an offset → 0x43454e).
- SRW change: added smackw32.dll ordinal→name table in `SRW/SRW_loader.c` (15 Smack* imports by ordinal).
- Manual excludes: GUID bytes at 0x4bcd5f; `cmp ebx, 0x669fe0` integer constant (0x4b2e8c, 0x4b2efd).
- bssborder.csv: `0x501800,0` (raw size of .data = 0x3f800).
- Result: SRW run-007 finished (rc=0), 29497 relocations; output i76.asm + seg01/02/03/05.inc.

### 2026-09-23 — first complete assembly (run-015)
- In x86 output mode SRW does NOT use the PE entry point as a code root; the CRT startup is replaced
  and C `main()` calls `WinMain_asm`. Root = `global_aliases.sci`: `loc_402B30,WinMain_` (IDA: `_WinMain@16`).
- Callbacks (`push offset func`, WndProc, qsort comparators, EH funclets) come from
  `SRW --list_invalid_code_fixups=...`; `tools/gen_code_fixups.py` keeps only candidates that IDA
  sees as code heads (skips jump tables in .text). Converged after 1 iteration: 149 entries in
  `x86/fixup_interpret_as_code.sci`. All 2002 IDA functions except the CRT startup are now emitted as code.
- SRW change (`SRW/SR_basic.c`, SR_initial_disassembly): don't define the section-end sentinel label
  when another section starts at that address. Otherwise a bss split exactly at the end of real data
  (no unlabeled padding gap, unlike Septerra's `-24`) gives "label inconsistently redefined".
  bssborder.csv back to `0x501800,0`.
- `instruction_replacements.sci`: 0x499997 (53 bytes, cli..sti) CPU MHz measurement via PIT ports
  0x42/0x43/0x61 → `mov word [ebp-0x4], 0`; function then returns -1 and caller 0x499B00 uses 200 MHz.
  (Replacing only the function entry did not stop SRW from emitting the rest of the body.)
- `ignored_areas.sci`: CRT startup `start` 0x4BA0E0 (430), thunks `_XcptFilter` 0x4BA30A, `_initterm` 0x4BA310,
  `__setdefaultprecision` 0x4BA320, `_controlfp` 0x4BA340, and start's SEH scope table in .rdata 0x4BECA8 (12).
- TODO: CRT startup called `_initterm` (C++ static constructors, `__xc_a..__xc_z`) → replacement main() must do that.
- nasm: 0 errors, no short-jump repairs needed. `i76.o` 1.9 MB, 233 undefined externals
  (Win32 API, MSVCRT, DirectDraw/DirectSound, Smack*, dp* (anetdll), StrLookup*, joy*/aux*/mci*, x86_read/write_fs_dword).
- `build-x86.sh` added (no deletes).

### 2026-09-23 — DLLs and runtime scoping
- Dynamically loaded modules: `I76SHELL.DLL` → use `i76shell.dll` (PE timestamp Feb 1998, matches exe;
  `I76SHELL_1083.DLL` is an older Jul 1997 build). Exports `ShellMain`, `ShellWindowProc`.
  Renderers `ZGLIDE.DLL`/`ZDX5DRAW.DLL`/`ZREDLINE.DLL`/`zpowervr.dll` share one export interface
  (CheckFunc, FirstDevice, GetFuncDesc, GetNumDevice, GetSocketCaps, LastDevice, LockDisplay, LostDeviceDisplay,
  PreloadTexture, RefreshDisplay, Render, RenderNoClip, RenderRefresh, RestoreDevice, SetLumaTable, SetState,
  SetTexturePalette, UnlockDisplay, UpdateTexture).
- GOG setup is the Glide build (product.ini ProductID `I76GLDW95`, OpenGLide config) → DECISION: renderer =
  recompiled ZGLIDE.DLL + own Glide 2.x implementation (39 functions) on OpenGL/SDL2.
- All DLLs keep their relocation tables → SRW runs without relocations.csv. All share image base 0x10000000;
  that's fine because `loc_` labels are local per nasm object (BI3 does the same).
- SRW-i76shell: 88 fixup_interpret_as_code entries; the 131 unreferenced IDA functions are dead code.
- SRW-zglide: 29 entries; ZGLIDE links the MSVC CRT statically (fopen/fseek/ftell/realloc/strncmp…);
  its CRT init (DllMain→__cinit/__heap_init/__ioinit) is not run → map the CRT functions it uses to runtime
  implementations via external_procedures.sci (as Septerra's CLIB does).
- Tools: `tools/check_coverage.py` (IDA funcs not emitted as code), `tools/ida_xrefs.py` (callers per function).
- Runtime scope: 233 distinct imports across exe+shell+zglide; Septerra's runtime already provides 55.
  Missing: MSVCRT (forward most to host libc: same i386 cdecl ABI; emulate FILE internals, _stat, _open
  flags, _pctype/_mbctype, errno), Heap*/Virtual*/Reg*/GDI extras, Smack* (Smacker video), dp*
  (anetdll networking → stub, single player first), StrLookup* (STRLKUP.DLL, small; recompile or rewrite),
  joy*/aux*/mci* (GOG WIN32.dll = winmm + CD-audio emulation via audiere from music/ dir), C++ EH
  (`__CxxFrameHandler`, `_except_handler3`, fs:[0] via x86_read/write_fs_dword like Septerra).

## Plan
1. [done] SRW + nasm for i76.exe; SRW for i76shell.dll and ZGLIDE.DLL.
2. Runtime skeleton `games/Interstate 76/SR-I76` (copied from Septerra): SConstruct, main (+ C++ static ctors),
   extern.inc/asm2c stubs for every import (unimplemented ones log + abort) → first link.
3. Get WinMain running: file I/O (ZFS archive via CreateFileMapping/MapViewOfFile), registry, window/input (SDL2).
4. LoadLibrary/GetProcAddress emulation: table of recompiled modules (i76shell, zglide) and their exports.
5. Glide 2.x on OpenGL; ZGLIDE external CRT mapping.
6. Shell UI (GDI/DIB → SDL surface), Smacker videos, sound (DirectSound → SDL audio), CD music.
7. Networking stubs, force feedback stubs, polish.

### 2026-09-23 — runtime skeleton, first link and first run
- `SRW-games/Interstate 76/gen_all.sh` regenerates all 4 modules (i76, i76shell, zglide, strlkup) into a new
  `/home/teemu/sorsa/i76work/gen-NNN` dir, runs compact_source, `fix_import_names.py`, nasm + repair_short_jumps
  loop, and copies the `.asm`/`seg*.inc` into `games/Interstate 76/SR-I76/x86/<module>/`.
- `fix_import_names.py`: MSVCRT import `div` collides with the x86 `div` mnemonic (a `%define div ...` rewrites
  instructions) → import references renamed to `msvcrt_div`.
- DLL CRT: `external_procedures.sci` redirects CRT entry points used by game code (ZGLIDE 23, STRLKUP 21) to the
  shared runtime CRT; `tools/gen_ignored.py` writes `ignored_areas.sci` for dead functions (fixpoint over IDA xrefs;
  a missing loc_ label does NOT mean dead - fall-through code has none; areas extend to the next function start
  so trailing jump tables go too). `tools/crt_boundary.py` lists CRT functions called from game code.
- Runtime `games/Interstate 76/SR-I76`: `imports.spec` (359 imports: conv/args from IDA prototypes, dp* from call
  sites, Smack* from decorated DLL exports) → `gen_imports.py` → `x86/imports.inc` (%define name → name_asm2c),
  `x86/imports-asm.asm` (Call_Asm_StackN stubs, 16-byte stack alignment), `imports-stubs.c` (weak "unimplemented").
  Added macros Call_Asm_Stack10/11 and Call_Asm_VariableStack1 (reserve 12 bytes, otherwise the saved-esp slot can
  overwrite the return address).
- `x86/raw-asm.asm`: `_ftol` (truncating fistp → edx:eax), traps for `__CxxFrameHandler`/`_except_handler3`.
- `msvcrt.c`: MS-layout FILE (host FILE* in _tmpfname, _cnt kept 0 so inline getc → _filbuf, _IOEOF/_IOERR flags
  maintained for inline feof/ferror), text-mode CRLF, MS rand (RAND_MAX 0x7fff), exact _msize (16-byte header),
  _stat/_finddata_t MS layouts, _open flag translation, ctype tables (_pctype/_mbctype/__mb_cur_max), printf via
  printf_x86 (from Septerra), scanf via host v*scanf with the x86 arg area as va_list (i386 only).
  `div` returns div_t in edx:eax → implemented as uint64_t return.
- `vfs.c`: Windows path → host path, case-insensitive per component, `..`, drive letters, fake install dir `C:\I76`.
- `main.c`: runs i76.exe's C++ static constructors (`__xc_a..__xc_z` = 0x4C2000..0x4C2010, exported via
  global_aliases `i76_xc_a/i76_xc_z`) then `WinMain_`.
- Build: `cd "games/Interstate 76/SR-I76" && ~/.local/share/sr-venv/bin/scons` → `SR-I76` (4.9 MB, -m32).
- Test data copy: `/home/teemu/sorsa/i76work/gamedata` (don't write into the Wine install).
- First run: WinMain executes: FindWindowA, Reg*, FindFirstFileA, GetSystemInfo, GetProcessHeap, then LoadLibraryA
  (stub) → exits 1. Next: kernel32/advapi32/user32 implementations and LoadLibrary emulation.

### 2026-09-23 — main menu visible
- Renderer path: no renderer flag → built-in software renderer; runtime passes `/gdi` so the exe presents its
  8-bit framebuffer with SetDIBitsToDevice (DIB_PAL_COLORS through the realized logical palette). The shell
  (i76shell) is GDI-only (DIB sections, BitBlt/StretchBlt). DirectDraw/DirectSound creation fail for now.
- Win95 quirk: i76shell treats `BitBlt(...) == height` as success (Win95 returned the scan-line count) →
  BitBlt/StretchBlt return the line count.
- New runtime files: display.c (SDL window/texture, `I76_SCALE`, `I76_DUMP_FRAMES`), winapi-user32.c
  (windows, message queue, SDL key → VK/scan code, WM_CHAR via TranslateMessage), winapi-gdi32.c (DCs, DIB
  sections, palettes, blits, FreeType text: Lee from game dir, Arial → Liberation Sans, courier → Liberation
  Mono), winapi-kernel32.c (files, finds, mmap file mappings, per-heap tracked HeapAlloc, VirtualAlloc, module
  table for LoadLibrary/GetProcAddress + DLL static ctors), winapi-advapi32.c (registry in `SR-I76.reg`),
  winapi-misc.c (winmm/ole32/DirectX stubs). Env: `I76_DEBUG=1|2`, `I76_NO_MESSAGEBOX`.
- Hex-Rays decompilation of each module (tools/ida_decompile.py → i76work/*/out/decompiled.c) is the main
  reference; tools/getfunc.py extracts functions.
- Result: the user saw the game's starting menu (shell) rendered in the SDL window.

### 2026-09-23 — decisions (user)
- Targets: Linux 32-bit x86 and Windows 32-bit.
- 3D: own Glide 2.x implementation (replaces nGlide-style wrappers) with a Direct3D 11 backend; Linux will need
  a second backend (OpenGL) - decide when starting 3D.
- Order: Smacker video → play a mission in software mode → sound → Glide/D3D11.
- Work committed on local branch `i76` (not pushed). User's fork: https://github.com/teepean/SR

### 2026-09-23 — toward a mission
- Smacker video works (smackw32.c on Albion's decoder; blit rect args are 16-bit with garbage high words).
- `I76_INPUT_SCRIPT=<file>`: timed input (`<ms> click|down|up|move x y`, `key|keydown|keyup VK`, `quit`).
  Menus poll the button state, so use separate down/up ~300 ms apart.
- Relocation bugs found at runtime:
  - `cmp ebx, 0x669fe0` (0x4B2E8A/0x4B2EFB) is the end pointer of the ZIX directory table (beyond .data
    VirtualSize) → instruction_replacements.sci `cmp ebx, loc_6593E0 + 0x10C00`. Lesson: past-the-end pointers.
  - unaligned candidates in data sections were all false positives in i76 (floats, chunk tags "SOBJ", strings
    "rb") → `gen_relocs.py --aligned-data`; plus two UTF-16 table entries excluded.
- The game's qsort/bsearch comparators return dword differences (overflow) → msvcrt.c reimplements MSVC's
  exact qsort (median-of-middle quicksort, 8-element shortsort) and bsearch.
- CD emulation (`I76_CD=1`) is off by default: GOG runs without CD; with a CD the shell behaves differently.
- fopen/SmackOpen reject directories (Linux can fopen a directory, Windows can't).
- Current blocker: TRIP→TRAINING loads a01.msn, then WinMain calls the state function pointer at 0x4C2720,
  which is still NULL. Next: watch 0x4C2720 in the original exe under Wine (separate prefix
  /home/teemu/sorsa/i76work/wineprefix, input via xdotool) to find the divergence.

### 2026-09-23 — training mission runs (software renderer)
- The NULL state pointer came from missing vehicle objects: the mission chunk descriptor tables (OREV 0x500AE8,
  LREV 0x500D80) contain the tag "OBJ\0" = 0x004A424F, which is a valid image address → wrongly relocated,
  so the OBJ chunk was never matched and `sub_463120` (vehicle class init → `sub_405970(1)`) never ran.
  Excluded in reloc_exclude.txt (also the `mov [esp+0x1c],"REV\0"` immediate); classify.py got a `text-tag`
  rule (data-section value with 3 uppercase low bytes pointing into .text but not at a function).
  Found by instrumenting the original exe under Wine (code caves → OutputDebugStringA, `+debugstr`).
- `text-mid-block-noxref` rejected real handlers: blockstart.py now accepts any 16-byte aligned target.
- Key-binding action table (0x4F2Cxx, `{name, flag ptr}`) pointers to 0x53677x were rejected by `bss-ascii`
  (bytes printable). classify.py now "rescues" aligned ascii/bss-ascii/low16zero rejects that sit in a regular
  table (accepted neighbours at the same stride, targets within 0x100). Septerra validation unchanged (FP 23/FN 10).
- Hang in texture-animation update `sub_44B2D0`: descriptors allocated with HeapAlloc (sub_449xxx) leave fps
  (+20) and flags (+24) uninitialised; on Windows the fresh heap is zero. HeapAlloc/HeapReAlloc now always zero.
- Exit crash: `atexit(SDL_Quit)` crashed inside the video driver. `app_exit()` (display.c) destroys the window,
  calls SDL_Quit and `_exit`; used by ExitProcess, WinMain return and the input script `quit`.
- Result: TRIP → TRAINING starts a01.msn; intro camera and the cockpit (radar, weapons, damage panel, mirror)
  render in software mode. Offscreen test: `SDL_VIDEODRIVER=offscreen I76_DUMP_FRAMES=<existing dir>
  I76_INPUT_SCRIPT=script4.txt` (the dump dir must exist).
- Next: driving input check, sound (DirectSound → SDL), then Glide/D3D11.
- Exit abort "double free or corruption": the game frees dword_504C0C twice in WinMain's cleanup (the original
  crashes on exit under Wine's debug heap too). Heap blocks now carry a magic; HeapFree/HeapReAlloc/HeapSize
  fail on invalid blocks like Windows instead of aborting.
- User test: driving in the training mission works; only audio is missing.

### 2026-09-23 — sound
- mixer.c: SDL2 audio (44.1 kHz stereo S16), sources mix floats in the audio thread. `I76_NOSOUND` disables it.
- COM glue generated from com.spec by gen_imports.py: `<Iface>_<Method>_asm2c` stdcall stubs, `<Iface>Vtbl_asm2c`
  tables (x86/com-asm.asm) and weak E_NOTIMPL C stubs (com-stubs.c) that log unimplemented methods.
- dsound.c: IDirectSound/Buffer/3DListener/3DBuffer. The game creates the primary buffer with DSBCAPS_CTRL3D,
  QIs it for the listener, and uses 11025 Hz mono 8-bit secondary buffers: 0xb2 (3D) and 0xe2 (pan) - volume,
  frequency (engine pitch), looping. 3D model: min/(min + rolloff*(d-min)) attenuation + left/right attenuation
  from the listener's right axis (top x front, left-handed). `I76_DEBUG=3` lists playing buffers every second,
  `I76_DUMP_SOUND=<dir>` saves buffer contents on Play.
- cdaudio.c: MCI "cdaudio" device + aux volume playing music/<track>.mp3 (minimp3, CC0) like GOG's win32.dll.
  The game only starts CD music when a CD-ROM drive exists (sub_470ED0 counts them), so D: is now a CD-ROM
  by default with the label "AUDIO_CD" (not "I76_CD2", which changes the shell's behaviour). `I76_CD=1` = game CD,
  `I76_CD=0` = no CD drive.
- smackw32.c: Smacker audio track 0 decoded (Albion's decoder) into a ring buffer played by a mixer source.
- Test without speakers: `SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=out.raw` (sdl2-compat also needs
  `SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=out.raw`).

### 2026-09-23 — frame limiter
- The game logic is frame-rate dependent (physics/jumps, flamethrower, AI steering, sound cut-offs; see
  https://github.com/CahootsMalone/interstate-76-stuff, the community recommends 20 FPS). Uncapped the port ran
  the training mission at ~900 FPS.
- instruction_replacements.sci: `loc_49C929` (the GetTickCount call in the per-frame timer sub_49C920, called
  once per frame from the main loop) → `call i76_frame_tick`, a runtime function (imports.spec entry with dll
  "runtime") that waits for the next frame slot and returns GetTickCount. `I76_FPS=<n>` (default 20, 0 = off).
  `I76_DEBUG=1` prints the measured frame rate every 5 s.

### 2026-09-23 — Glide (hardware renderer), part 1
- Architecture: render.h = backend interface (render_gl.c: OpenGL 3.3 core, testable on Linux incl. the offscreen
  SDL driver; a Direct3D 11 backend for Windows will implement the same interface). display.c presents the GDI
  framebuffer through the backend (SDL_Renderer is gone: Glide needs the GL context of the same window).
  glide.c = Glide 2.x semantics. I76_GLIDE_SCALE (default 2) = internal resolution multiplier, I76_VSYNC.
- `/glide` selects ZGLIDE.DLL. /gdi must NOT be added then: with /gdi (dword_4F9F20 = 0) video playback takes a
  DirectDraw path (sub_42E1D0) even in Glide mode.
- ZGLIDE's Glide usage (survey of the DLL):
  - grSstWinOpen(0, 640x480, 60Hz, ARGB, upper-left, 2, 1); GrHwConfiguration is 148 bytes (GLIDE_NUM_TMU = 2),
    only nTexelfx (+0x10) is read -> report 1 TMU.
  - vertices: x/y carry +786432.0 (3<<18) snap bias that is never removed (Voodoo fixed point drops it);
    ooz and tmuvtx[0].oow are never written -> texture perspective and W-buffer depth from the vertex oow.
    s/t are 0..255 along the long side.
  - textures: P_8 (palette downloaded on every bind), RGB_565 or ARGB_1555; one TMU; bump allocator from
    grTexMinAddress with grTexCalcMemRequired; buggy past 2 MB -> emulate a 2 MB TMU.
  - 4 color combines (tex*iter, decal, flat constant, gouraud), alpha: tex / const / tex*iter / iter,
    blends: alpha, opaque, (DST_COLOR, ONE) "brighten". Chroma key compares the combiner's "other" input.
  - fog: GR_FOG_WITH_ITERATED_ALPHA only. No per-frame clear. grBufferSwap(1).
  - LFB: write-only 565 lock of the back buffer every frame (cockpit, HUD, menus, text). Emulated by reading the
    back buffer into the lock buffer and compositing only changed pixels on unlock (keeps the scaled 3D).
  - GetState/SetState only in LostDevice/RestoreDevice (GrState = 312 bytes).
- Glide mode switches between 2D screens (DirectDraw 8-bit, sub_434100(a1, 0) -> sub_431980(3, ...)) and Glide
  (sub_434100(a1, 1) -> FirstDevice): DirectDraw emulation is needed next.
- ddraw.c: DirectDraw emulation (IDirectDraw v1 + IDirectDraw2 view, surfaces, palette, clipper). The game's
  2D mode in Glide mode: EnumDisplayModes must offer 640x480x8 (the game's callback is called directly as
  stdcall), SetCooperativeLevel(EXCLUSIVE|FULLSCREEN), SetDisplayMode(640,480,8), primary with one back buffer
  (caps 0x2A18), palette on both; drawing = Lock back buffer (lPitch must be 640, lpSurface must stay stable -
  Flip copies instead of swapping), present = primary->Blt(back); palette SetEntries must re-present.
  i76shell.dll uses the same DD objects (passed via ShellMain) with the same pattern.
- Result (offscreen test): /glide shows the shell via DirectDraw, videos, and the training mission rendered
  by Glide at 1280x960 (I76_GLIDE_SCALE=2) with the LFB cockpit composited on top, 20 FPS.
- Mouse fix: GetCursorPos still converted through SDL_Renderer (gone since the GL backend) -> raw window pixels,
  clamped at 639/479 in a 1280x960 window; the shell polls GetCursorPos + GetAsyncKeyState, so clicks missed.
  Now uses display_window_to_client. Input scripts: `wmove X Y` (real SDL mouse warp, window pixels), `wdown`,
  `wup` test the real mapping path.
- In-game menu (ESC during a mission; in training the first ESC skips the intro): drawn into a 296x425 DIB,
  composited through the LFB, then the game swaps only when the menu changes. Some compositors only show a
  frame after the next one arrives, so the window kept showing the pre-menu frame ("freeze"). The GL backend
  now re-presents the Glide front buffer every ~33 ms when the game isn't swapping (render_glide_refresh).
  I76_DUMP_FRAMES now dumps exactly what the window shows in Glide mode.
- Keyboard dead / in-game menu "freezing": during a mission the game only removes keyboard messages
  (PeekMessage 0x100-0x108), so every SDL mouse motion added a WM_MOUSEMOVE until the 512-entry queue was full;
  new key messages were dropped until something drained the backlog (the menu then crawled through it).
  post_message now keeps at most one pending WM_MOUSEMOVE per window (latest position, not across clicks),
  like Windows. Script command `mflood N` posts N mouse moves (queue stays at 1).
- Exit abort again ("double free or corruption (out)") in Glide mode: the magic value in the block header wasn't
  reliable (HeapDestroy and a moving HeapReAlloc left it in freed memory, which can be reused without being
  overwritten). Live heap blocks are now tracked in a hash set; HeapFree/HeapReAlloc/HeapSize of anything else
  fails like on Windows. Verified: ESC -> Abort Mission -> Exit Game -> Yes exits cleanly in Glide mode.

### 2026-09-23 — settings
- config.c: SR-I76.cfg in the game directory (written with commented defaults if missing), keys: renderer
  (glide|software, default glide), glide_scale, window_scale, fullscreen, fps, vsync, sound, cd, joystick.
  Environment variables I76_<KEY> override the file. Command line /gdi or /glide overrides renderer.
- Community notes (GOG forum, user-provided): dgVoodoo users fixed texture corruption (road-sign textures on
  roads, explosions on the sky) by limiting texture memory to 2 MB = ZGLIDE's allocator bug (we emulate 2 MB);
  25 FPS exes for the frame-rate bugs; 16-bit depth buffers avoided; crashes reported between story missions
  (to check in playthrough tests). Widescreen isn't possible from the Glide side (the game renders into 640x480
  screen coordinates; wrappers only stretch) - would need a camera/FOV change in the game.
- joystick.c: winmm joyGetNumDevs/joyGetDevCapsA/joyGetPosEx/joyGetPos on SDL. Game controllers: X/Y left
  stick, Z = triggers as one throttle axis (RT forward), R/U right stick, 11 buttons, d-pad = POV; other devices:
  axes 0-5, 32 buttons, hat 0. The game enumerates joysticks through its input driver table (0x4F53F8) when the
  Control Configuration screen opens; no default joystick bindings (the player binds in that screen).
  SDL3/sdl2-compat enumerates devices asynchronously: the subsystem is started at program start
  (joystick_startup) and devices are opened on first use. I76_VIRTUAL_JOYSTICK=1 attaches a virtual controller.
  Config: joystick = 0/1.
- Joystick test tooling: I76_VIRTUAL_JOYSTICK=1 uses only an SDL virtual controller; script commands
  `jaxis N value`, `jbutton N 0|1` drive it. Verified with gdb on sub_450870 (the game's joystick read): stick up
  -> axis0 = -65532, stick right -> axis1 = +65532, buttons pass through. With a joystick present the game
  binds "joystick1" to Accelerate/Brake/Steer Left/Steer Right by default (Control Configuration).
- Real Xbox controller didn't work: values frozen at their first reading and the device later "unplugged".
  Likely SDL replacing the device (evdev -> HIDAPI driver switch). joystick.c now reopens the devices when the
  eligible device count changes or a handle is detached (checked at most every 500 ms from joyGetNumDevs/
  joyGetDevCapsA/joyGetPosEx). Script command `jreattach` simulates it with the virtual controller.
- Real controller still frozen: SDL (32- and 64-bit test programs too) only saw the initial state. Cause: another
  program (Wine's winedevice.exe of an unrelated Wine app) had the evdev device open - Wine's winebus grabs
  controllers. Diagnose with: for each /proc/*/fd, readlink to /dev/input/eventN. joystick_evdev.c (direct evdev
  reading) was added as an optional backend (joystick_backend = evdev); SDL stays the default.
- Joystick works (user-verified with an Xbox One pad). Remaining gotcha was game-side: input.map had
  `throttle { - joystick1 ----- }` / `steer { ... ----- }` (no axis) - saved while the controller was frozen.
  Control Configuration -> RESTORE (defaults from JOYSTICK.MAP: throttle = joystick1 Down/Up, steer =
  joystick1 Left/Right) or picking the axes in SELECT INPUT fixes it.

### 2026-09-23 — playtesting
- Crash after the first mission (user, Wayland/KDE desktop): SIGABRT in libwayland-client (wl_proxy_marshal) from
  SDL_GL_SwapWindow via libEGL_mesa: in a Wayland session the 32-bit NVIDIA EGL isn't used ("MESA-EGL: failed to
  create dri2 screen"), rendering fell back to Mesa. No fd leak found (41 fds stable through a mission). The port
  now prefers X11/XWayland on Linux (SDL video driver hint "x11,wayland"; SR-I76.cfg video_driver = x11 |
  wayland | auto). I76_DEBUG=1 logs the GL renderer and SDL video driver.
- Crash a few seconds into mission 2: glibc "double free or corruption (!prev)" detected inside the NVIDIA driver
  (texture upload) = earlier host-heap corruption. The game frees invalid pointers (HeapFree log shows repeated
  invalid blocks, e.g. a static address) and the CRT free()/delete went straight to glibc. Now the CRT heap
  (malloc/free/realloc/_msize/new/delete) uses the emulated Win32 heap (live-block set rejects invalid frees),
  and every heap block has 32 bytes of zeroed slack so small overruns don't hit glibc's chunk headers.
  Septerra's runtime has no such protection (its 32-bit build uses plain malloc; Game-Memory.c is its low-4GB
  allocator for 64-bit builds - to reuse for the I76 64-bit port).
- Playtest status (user): story missions 1-3 played through without problems (Glide, X11, sound, joystick,
  save game created and loaded).

### 2026-09-23 — 64-bit port, step 1 (branch I76_64bit)
- SRW x64 translator (SRW/SR_full_x64_instr.c): added fpatan, fprem1, fscale, f2xm1, fsincos, fclex (FPU
  pass-through), setae (setcc group), jecxz (jump group; nasm encodes it with the a32 prefix in 64-bit mode),
  cmpsw/cmpsd/scasw/scasd (like cmpsb, `a32` string ops). SRW64 is built out of tree with OUTPUT_TYPE OUT_X64
  (/home/teemu/sorsa/i76work/srw64-build/SRW64.exe).
- SCI: <module>/x64/ = copies of x86/, except instruction_replacements `CALL i76_frame_tick` (x64 macro).
- gen_all.sh: ARCH=x64 uses SRW64 ($SRW64), x64 SCI files, nasm -felf64, DEST SR-I76/x64.
- SR-I76/x64: Septerra's x64 includes (x64inc.inc, asm_call.inc, asm_pushx.inc, asm_unwind.inc, asm_fs_mem.*,
  asm-calls.inc, misc.inc, asm-cpu.c, x64_stack.h).
- Result: all four modules generate and assemble as ELF64 objects. Next: runtime (C++/PTR32, low memory,
  trampolines, x64 asm2c glue) per X64_PORT_PLAN.md.
- x64 step 2: the runtime builds and links as x86-64 (`scons device=pc64-linux` -> SR-I76-x64; objects .o64,
  C compiled as C++ like Septerra, EXTERN_C_BEGIN/END around every runtime file, extern "C" guards in all
  headers; Septerra's Game-Memory.c copied; gen_imports.py writes x64/imports-asm.asm and x64/com-asm.asm with
  explicit SysV stubs; x64/raw-asm.asm, x64/c2asm.asm (generic C -> game trampoline c_call_asm_n); scanf now
  translates the format (MS %ld = 32 bits) and passes explicit host arguments). The 32-bit build is unchanged
  in behaviour (mission test passes). Not yet runnable: trampolines not wired, memory not low, PTR32 fields.
- x64 step 3: calls into game code go through call_game() (c2asm.c; x86/c2asm.asm and x64/c2asm.asm):
  WinMain, static and DLL constructors (4-byte xc tables), WndProc, EnumDisplayModes callback, qsort/bsearch
  comparators. Game-visible memory from x86_malloc/map_memory_32bit (Septerra Game-Memory.c): heaps, handles
  (windows, files, finds, mappings, registry keys, GDI objects), DirectDraw/DirectSound objects and buffers,
  DIB sections, LFB, Smacker objects, file views, VirtualAlloc, CREATESTRUCT, EnumDisplayModes desc, errno copy.
  PTR32 fields: COM lpVtbl, out-params (PTR32(T) *), GrTexInfo.data, GrLfbInfo_t.lfbPtr, DSBUFFERDESC format,
  ms_FILE (host FILE* moved behind the 32-byte struct / iob_host), _pctype, MSG.hwnd, WNDCLASSA, PAINTSTRUCT.hdc,
  SmackBuf.Buffer. First x64 run: starts, loads the DLLs, opens Glide and DirectDraw (addresses 0x41xxxxxx).
- GetKeyState: Windows returns 0xFF80|toggle for a pressed key; the game tests & 0x1000 for Ctrl/Shift/Alt, so
  modifiers (and the Ctrl+Shift cheat codes like "getdown") never worked with 0x8000.
- Crash at the start of a later mission (again glibc "double free or corruption (!prev)" noticed inside the
  NVIDIA driver's texture upload) even with the tracked CRT heap. Heap hardening: the 32-byte slack after
  every block holds a check pattern (overruns reported on free/realloc, and for all blocks ~10x/s with
  I76_HEAPCHECK=1); freed blocks (HeapFree, HeapReAlloc's old block, HeapDestroy) are quarantined (up to
  4096 blocks / 32 MB) before the memory is reused, so use-after-free writes can't hit live data or glibc's
  metadata; HeapReAlloc always allocates + copies. Training mission: no overruns detected.
- ROOT CAUSE of the mission-start crashes (found with I76_HEAPGUARD=1: per-block pages + guard page, freed
  pages made inaccessible): sub_469B00 (parses a loaded text file line by line) computes the remaining length
  as `strpbrk(...) - Str`; at the end strpbrk returns NULL and NULL - Str is negative on Windows (addresses
  < 2 GB) but a huge positive length for buffers above 0x80000000 -> strncpy into a 200-byte stack buffer.
  In the 32-bit Linux build glibc mmaps large allocations high (0xE...). Fix: 32-bit build keeps game memory
  below 2 GB like Win32 (mallopt(M_MMAP_MAX, 0) -> brk heap; low_mmap searches 0x10000000-0x7FFF0000 with
  MAP_FIXED_NOREPLACE for file views/VirtualAlloc/guard pages). The x64 build already uses x86_malloc (< 2 GB).
  Note: the native stack is still high in the 32-bit build.
- The heap check pattern must be zero (a non-zero pattern broke the same parser's terminator); guard mode keeps
  16 zero bytes before the guard page. I76_HEAPGUARD=1 training run: no overruns or use-after-free.
- x64 milestone: SR-I76-x64 (`scons device=pc64-linux`) runs the full offscreen test: intro videos, menus,
  training mission in Glide mode (20 FPS) and GDI mode, in-game menu, Exit Game with a clean exit; sound
  (DirectSound buffers, CD music) works. No 32-bit libraries are needed.
- Another 32-bit mission-start crash (t06, same glibc corruption seen in the GL driver) while the x64 build
  played the same mission fine (user-verified): in the x64 build x86_malloc is Game-Memory.c's separate heap, in
  the 32-bit build it was glibc's malloc - stray game writes into freed runtime memory (surfaces, sound buffers,
  Smacker buffers...) corrupted glibc. Now all memory the runtime hands to the game comes from game_malloc/
  game_calloc/game_free = a private emulated Win32 heap (quarantine, overrun check, I76_HEAPGUARD) in both builds.
  User playtest: x64 build played missions 5-6 without problems.
- x64 playtest: several missions OK, then a crash at the start of mission 13 (t13.ter) - a NULL dereference in
  game logic, not memory corruption: the mission script interpreter sub_412CE0, opcode 0x5A, resolves an object
  with sub_45F0F0 (may return NULL) and calls sub_467400(obj) without a check ([obj+0x70]). The user had skipped
  missions with the getdown cheat (which disables mission triggers); forum users report crashes between missions
  in the original too. Defensive game patch in instruction_replacements (x86 + x64): sub_467400 returns when
  obj or obj->+0x70 is NULL. Reproduction attempt (bookmark "Scene 12. 14" -> mission 12 + getdown) didn't reach
  the mission end within 8 minutes (car stuck); the bookmark load path itself works in x64.
- Mission 13 crash, real cause (the sub_467400 check above only moved the crash to sub_467470): a bug of the
  GOG i76shell.dll (known: "mission 13 crash", fixed by the 1.06 patch's i76shell.dll). Between missions 12 and
  13 the shell rebuilds the car's installed parts from the part *display names* in the car record
  (sub_10002130: strncmp(name, catalogue[i], 15), first match). Wheels of all vehicle classes share names
  ("14in Rally" = wauto_1b.wdf, wbtck_1b.wdf, ...), and the file table (sorted by sub_470CA0, a little-endian
  dword compare, so "wbtc" < "waut") puts truck wheels first. The car gets wbtck_1b wheels, the garage's wheel
  slot filter drops them as not fitting, sub_10035250 writes vehscn.vcf with "null" wheels, and the mission
  13 car has no wheels (vehicle +936/+940/+952/+956 NULL) -> NULL dereference in sub_467470. Saves made after
  mission 12 contain the wbtck wheels (save014 of the playtest).
  Fix: instruction_replacements.sci of i76shell (x86 + x64): the lookup loop's `mov ebp, [strncmp]`
  (0x1000222D) loads i76shell_part_strncmp (gamefixes.c) instead - for wheels it skips entries whose .wdf
  isn't one of the car record's wheel files (+0x83A/+0x847/+0x854) when an entry with the same name and a
  matching file exists. Shell variables exported with global_aliases.sci (i76shell_cars, i76shell_catalogue,
  i76shell_catalogue_count, i76shell_car_index). Verified: bookmark "Scene 12. 14" -> getup -> mission 13
  starts with 4 wheels (x64). Diagnosis tools: gdb breakpoints on the recompiled labels (loc_XXXXXXXX) and on
  fopen_c, `catch syscall openat` (no strace here).
- Open: the 32-bit build aborts ("free(): invalid size") at the out12 cutscene in the same test; the new
  malloc-chunk check in heap_check_all (32-bit, I76_HEAPCHECK=1) reports a freed block whose glibc chunk
  header was zeroed around the int12 cutscene / mission 12 load. With I76_HEAPGUARD=1 glibc still aborts
  (in the GL driver), so the stray write hits memory outside the game heap. The x64 build is not affected.
  An ASan build is not usable (ASan's allocator places game memory above 2 GB).
- Milestone (user, 2026-09-23): the whole game played to the end in the x64 build (with the getup cheat for some
  missions). Decision: further development targets the 64-bit build only; the 32-bit build is kept as it is
  (its open heap corruption above is not being pursued).

### 2026-09-23 — Windows x64 build, step 1 (cross-compiled with MinGW-w64, tested with Wine)
- SConstruct device=pc64-windows: x86_64-w64-mingw32 gcc/g++ (C++ like pc64-linux), nasm -fwin64, objects
  *.w64.o; image base 0x10000000 without ASLR/relocations (recompiled code needs addresses < 2 GB);
  WIN_DEPS=<dir> with SDL2 (official SDL2-devel mingw package) and a static freetype (built with
  --host=x86_64-w64-mingw32 --without-{zlib,png,bzip2,brotli,harfbuzz}); winpthread linked statically ->
  the exe needs only SDL2.dll.
- gen_imports.py: every x64 stub has a Win64 variant (args in ecx/edx/r8d/r9d + [rsp+32...], the frame's
  shadow space is free for calls, _stack* at [rsp+FIRST_PARAMETER_OFFSET], rsi/rdi are callee-saved on Win64).
  PE/COFF weak symbols don't work reliably (some weak aliases were missing -> undefined references), so
  imports-stubs.c/com-stubs.c now only contain ordinary definitions for the functions that have no
  implementation (gen_imports.py scans the C files for `CCALL name_c(...) {`): rerun gen_imports.py after
  implementing an import. _vsnprintf's C function is ms_vsnprintf_c (_vsnprintf_c exists in the Windows CRT).
- x64/c2asm.asm Win64 c_call_asm_n uses asm_unwind.inc's SECTION_PROLOG frame. Bug found on the way: the saved
  esp was kept in a parameter home slot, but asm_fs_mem.asm (Win64) uses those slots as scratch (the game's
  fs:[0] SEH accesses) -> stack->esp = saved RFLAGS (0x200206) -> jmp 0. call_game now saves/restores
  stack->esp in C. raw-asm.asm: no red zone on Win64 (_ftol uses the shadow space).
- New sysmem.c/h (mmap vs VirtualAlloc/map_memory_32bit; file views are read into low memory on Windows),
  compat.c/h (fnmatch, strcasestr for MinGW), I76_LOG=<file> (stderr of GUI programs goes nowhere), a crash
  handler on Windows (registers, host stack, emulated stack via r10 = _stack*).
- Game bug exposed by Windows: SmackBufferOpen's width/height/zoom are u16 in RAD's SDK - the game loads them
  with 16-bit moves and pushes whole registers (garbage upper halves, 0 by luck on Linux) -> masked.
- Result (Wine 10, NVIDIA GL through Wine's Windows driver): intro, main menu, save load, mission 12, getup,
  mission 13 in Glide/OpenGL mode. Next: sound (DirectSound/SDL under Wine), joystick, a Direct3D 11
  renderer, packaging.
- Windows sound/music/joystick (Wine): SDL picks WASAPI; DirectSound buffers, Smacker audio and CD music (mp3)
  mix (checked with the new I76_AUDIO_DUMP=<file>: raw 44100 Hz stereo S16 copy of the mixer output; the
  official SDL2 Windows build has no disk audio driver and Wine ignored SDL_AUDIODRIVER=dummy). Virtual
  joystick (I76_VIRTUAL_JOYSTICK=1) through SDL on Windows: joyGetPosEx values identical to the Linux build.

### 2026-09-23 — Direct3D 11 renderer (Windows)
- render.c dispatches the render.h interface to a backend (render_backend.h): render_gl.c (OpenGL 3.3, all
  platforms) or render_d3d11.c (Windows). SR-I76.cfg graphics_api = d3d11 (Windows default) | opengl; if
  Direct3D 11 can't be initialized (no d3dcompiler_47.dll, no device) display.c recreates the window for
  OpenGL (render_fallback). render_viewport/render_drawable_size are shared (display.c's mouse mapping no
  longer calls SDL_GL_*).
- render_d3d11.c mirrors render_gl.c: HLSL port of the Glide pixel shader (noperspective color/depth, SV_Depth
  for Z/W buffering, Texture2D.Load for the chroma key texel, fog table in 16 float4s), RGBA8 front/back
  targets + D32_FLOAT depth, cached blend/depth/sampler states (GR_CMP_* + 1 = D3D11_COMPARISON_*; the alpha
  blend factors use the *_ALPHA variants of DST/SRC_COLOR), vertex ring buffer (NO_OVERWRITE/DISCARD),
  textures with generated mipmaps, dynamic BGRA textures for 2D and LFB writes, staging copies for LFB
  reads and frame dumps. Shaders compiled at startup with D3DCompile (d3dcompiler_47.dll, loaded dynamically).
- Verified under Wine (wined3d): menus and mission 12 -> 13 look the same as with OpenGL; fallback tested
  with WINEDLLOVERRIDES="d3dcompiler_47=d".

### 2026-09-23 — multiplayer, step 1: recompiled ANETDLL.DLL + DLL\WINET.DLL
- The GOG install ships Activision's Anet (Activenet, Mar 1997): ANETDLL.DLL (dp* API, loads transports from DLL\
  by name: commInit/commTxPkt/... 17 exports) and DLL\WINET.DLL (TCP/IP: UDP via WSOCK32), WIPX/WMODEM/WSERIAL.
  Anet was later released as LGPL (anet-0.10, kegel.com/anet) - useful reference for the protocol.
- Both DLLs are recompiled like strlkup/zglide (new SRW-anetdll, SRW-winet: IDA export, gen_code_fixups,
  external_procedures for the statically linked CRT, gen_ignored); the game's 22 dp* imports are `module`
  imports now; WINET.DLL is in the LoadLibrary module table; DllMains aren't needed (attach counter;
  WSAStartup). New imports: GlobalAlloc/ReAlloc/Free (process heap), _makepath, _ftime, _strcmpi.
  The shell references dpFreeze only from data (dead code) -> `extern dpFreeze` in x86|x64/i76shell/extern.inc.
- SRW changes: WSOCK32 ordinals 11 (inet_ntoa) and 116 (WSACleanup); upstream bug in the x64 and llasm
  translators: when both operands of an instruction carry the same relocated value (`mov dword [X], X`), the
  one's-complement disambiguation flipped the whole value (~value) instead of (value - tofs) + ~tofs like
  SR_full_dos.c -> "fixup operand mismatch" (WINET's ___initstdio).
- Winsock: winsock.c (14 WSOCK32 functions, 32-bit layouts, Winsock error codes) over hostnet.c (BSD sockets /
  Winsock 2; SIO_UDP_CONNRESET off on Windows). WINET learns its own address by broadcasting to its port and
  waiting for the packet; Linux (here: probably the firewall) doesn't deliver own broadcasts, Windows does ->
  own broadcasts are queued locally (sender = hn_local_ip()), host-delivered copies are dropped.
  SR-I76.cfg net_bind_ip (I76_NET_BIND_IP) binds to one local address: two instances on one machine with
  127.0.0.1 / 127.0.0.2 (UDP port 21155).
- Game UI: MELEE -> MULTI MELEE -> HOST/JOIN -> INTERNET (WINET) -> "Connect to server" list (internet.lst:
  u32 count + 96-byte entries name[32] address[64]). Players today use the host's IP as the "server" (Tunngle
  era); LAN play used IPX (IPXWrapper); OTHER = "maybe in the next version". The server list entry 127.0.0.1
  makes the host its own game server.
- Status: host (127.0.0.1) -> BROADCAST GAME -> the host is in the arena; the joiner (127.0.0.2) sees the
  session "The Crater (1/4)", joins (anet traffic both ways), then crashes: it elects itself master
  (sub_454E40: lowest player id among players with a measured ping; the host's ping is still -1.0) and spawns
  locally (sub_456100 -> sub_451030: rand() % spawn point count = 0, no arena loaded) -> SIGFPE. Next: the
  game's ping exchange / why the joiner hasn't measured the host yet (timing, lost packets?).
- Fixed (2026-10-01): the joiner crash was ours. The shell copies the mission name from the host's JO packet
  (dpReceive buffer offset 50 = FullPath) after `_splitpath(FullPath, 0, 0, 0, Ext)` with Ext[80] 130 bytes
  below it; our _splitpath_c used strncpy(ext, dot, 255), which pads to 256 bytes and zeroed FullPath -> the
  game started with an empty mission name (sub_4B42B0("")), no spawn points (dword_540D98 = 0) ->
  rand() % 0 in sub_451030. _splitpath now copies only the string (like MSVCRT). The joiner loads the arena;
  next problem: it waits in the join loop (sub_452F20) for the host's SP packet (reply to its SR), which dpio
  acknowledges but never hands to dpReceive.
