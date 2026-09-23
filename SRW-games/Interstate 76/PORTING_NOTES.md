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
