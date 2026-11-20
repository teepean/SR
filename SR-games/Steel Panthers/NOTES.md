# Steel Panthers (SSI 1995) – static recompilation notes

Goal: a native Linux x86-64 build of Steel Panthers made by statically recompiling the
original DOS/4GW executable with SR, plus a native SDL3 runtime (games/Steel Panthers).
The game data comes from the owner's install (`~/Steel_Panthers`) and CD image.

## Target executable

Since 2026-11-20 the target is **v1.2** (SSI's final patch): `SR/STEEL.EXE`, 1215455 bytes,
md5 702ee2143a12c8b344a95d3d3a596ca4; code object 0x10000 – 0xc4930, data object 0xd0000 –
0x17c600, entry 0xa05cc, 27493 fixups. How it was ported from v1.1 is in "Port to v1.2" below.
The rest of this file was written for v1.1 and gives v1.1 addresses unless it says otherwise;
`tools/addrmap.py SR/v1.1/STEEL.EXE SR/STEEL.EXE ADDR...` translates them.

v1.1 – identical to the CD's `\STEEL\STEEL.EXE` and to `patches/sp-1.1/STEEL.EXE`, kept in `SR/v1.1/`
with its SCI files:

* size 1159775 bytes, md5 ce8655668264f7cf55121956142ae0eb
* Watcom C/C++32 10.0 (1994), DOS/4GW bound (MZ → BW chain → inner MZ at 0x25d24, LE header at 0x286bc)
* object 1 (code): 0x10000 – 0xbac90, object 2 (data): 0xc0000 – 0x16be40, entry 0x974fc
* 25903 fixups, all 32-bit offsets (flat model)
* BSS 0xd453c – 0x15d3dc

## Toolchain

* Recompiler: SR from the mxvisor fork (~/sorsa/sr-mxvisor, https://github.com/mxvisor/SR)
  plus `SR/sr-x64-dos-instructions.patch` (x64 backend: scasw/scasd, cmpsw/cmpsd, jecxz, bsf,
  remaining x87 instructions, fldpi). `SR/build-sr-x64.sh` builds SR.exe with OUT_X64.
* `SR/build-x64.sh` → `steel-asm.asm` + `seg01.inc` (code) + `seg02.inc` (data) → `steel-asm.o`.
* Backend: x64 (original instructions almost 1:1, exact flags). llasm is the option for ARM later.
* Analysis helpers: `tools/le2elf.py` (LE → ELF with fixups applied, for Ghidra/REA/objdump),
  `tools/lxdump.py` (VFX driver DLLs), `tools/calls.py` (call graph / string refs),
  `work/fixups.json` (all fixup source → target pairs).

## Layout of the code object

| Range | Contents |
|---|---|
| 0x10000 – ~0x95000 | game code (`main_` = 0x10684, game init = 0x22ac3) |
| ~0x90000 – 0x96fff | Miles VFX portable library (panes, shapes) + SSI "api" library (`api.c`, `file.c`) |
| 0x974fc – 0x977d1 | Watcom cstart (not used; translated as dead code) |
| 0x97842 | `exit` |
| 0x97eb3 – 0x9cc00 | Miles AIL 3.03 public API (trace strings name every function) |
| 0x9d000 – 0x9ffff | SSI api `bank.c`, `AILSTUB.c` (SSI sound layer over AIL) |
| 0xa3000 – 0xa4fff | VESA banking helpers, api `heap.c`, CLIB init (`__InitRtns` 0xa427e, `__CMain` 0xa422e, `_fpinit` 0xa4318) |
| 0xa5000 – 0xadfff | AIL internals (DIG/MDI INI, driver loader, VOC, timers), Watcom `int386x` 0xad580, `intr` 0xad5dd, int stub table 0xad65e |
| 0xae000 – 0xbac90 | Watcom CLIB (heap via DPMI 0501h, files, system(), …) |

XI init table 0xd4500–0xd4530 (8 routines), YI fini table 0xd4530–0xd453c (2 routines).
`__CMain` calls `main_(argc=[0x15cf3c], argv=[0x15cf40])` then `exit`.

## Graphics: Miles VFX 1.x driver (VESA480.DLL, MCGA.DLL)

The game loads `vesa480.dll` with `0x9d702("vesa480.dll", 4, 0)` (OS/2 LX DLL, no exports; the
image starts with a 13-pointer table) and `VFX_register_driver` (0x90739) copies the table to
0xd1324. Slots, identified from the DLL code (ports/INT 10h) and the game's call sites:

| slot | addr | function | game calls |
|---|---|---|---|
| 0 | d1324 | describe_driver | 1 |
| 1 | d1328 | init_driver | 1 |
| 2 | d132c | shutdown_driver | 1 |
| 3 | d1330 | area_wipe (5 args) | 0 |
| 4 | d1334 | wait_vblank (port 3DA) | 1 |
| 5 | d1338 | wait_vblank_leading (port 3DA) | 5 |
| 6 | d133c | window_refresh (WINDOW*, x0, y0, x1, y1) | 7 |
| 7 | d1340 | window_read (WINDOW*, x0, y0, x1, y1) | 6 |
| 8 | d1344 | DAC_read (index, RGB*) – port 3C7 | 1 |
| 9 | d1348 | DAC_write (index, RGB*) – port 3C8 | 3 |
| 10 | d134c | ? (1 arg) | 0 |
| 11 | d1350 | pane_refresh (5 args) | 5 |
| 12 | d1354 | ? (4 args, INT 10h) | 0 |

Plan: replace the DLL loader with a native one that returns a table of stubs into C (SDL3).

## Sound: Miles AIL 3.03

All 105 API functions are located (trace strings `AIL_xxx(...)`). Plan as in the Warcraft port:
replace the AIL API used by AILSTUB/game with native SDL3 implementations (external procedures);
AIL internals (real-mode .DIG drivers, DPMI) become dead code.

## SR configuration (SR/x64/*.sci)

* fixup_interpret_as_code.sci: callbacks/function pointers SR would otherwise emit as data
  (game: 2598e 2692a 26ad6 27716 2e2dd 2e2fc 7d1cd 7d21c 8be69; CLIB printf output 977dc/9e2f8,
  SSI 1000 Hz timer a0508, fill-routine stubs b7e50..b9460, ...). Candidates come from
  `SR.exe --list_invalid_code_fixups`, classified with tools/classify_cands.py + manual review.
  The patched SR makes every listed address an entry point with a label.
* noret_procedures.sci: b71f2 (`pop edi; ret` get-address trick; table follows the call at b713d).
* external_procedures.sci: AIL API (40), VFX DLL loader 9d702, int386x 9e48a, SSI CD library
  (ae754 init, ae8fd shutdown, adcac playing?, add6b stop, ade13 play track).
* instruction_replacements.sci (see comments in the file): x87 emulator, lsl, BIOS ICA 0:4F0
  mouse words -> sp_bios_ica, eight `and eax,0f0h; add eax,BASE; jmp eax` dispatches -> label tables.

## Status (2026-11-20)

Playable. Tested with SP_AUTOMATE scripts (screenshots + SDL disk-audio capture):
intro film + soundtrack (bit-exact vs 5001.RAW), main menu, scenario list, briefing films,
player control, tactical map, unit movement, 10 turns of Charge to Glory to the debrief films,
generated battle with combat (hits, kills, smoke, ten different sound effects), save + load,
campaign setup and purchase screens, battle generator, editor screen, Exit to DOS (exit 0).
CD music tracks 2 (menus), 3 (battle), 4 (results) play from the BIN.

Runtime fixes for DOS assumptions: argv[argc] (main_ reads it), guard pages around DPMI blocks
(game heap checks read 12 bytes before a block), PIT channel 0, idle throttling of polling loops.

Keyboard and map scrolling (2026-11-20):
* Input: the UI loop (0x25886) does kbhit (0x9E320, INT 21h 0Bh) → getch (0x973EF, INT 21h 08h)
  → tolower → button hotkey match (0x251A6); the battle map loop (0x29BFE) also takes the keypad
  digits 6/3/1/4/7/9 as hex directions (0x2AB9B). No getch caller reads the second byte of an
  extended key, so under DOS the arrows turned into the hotkeys h/k/m/p; the runtime drops keys
  without an ASCII code from DOS console input.
* Scrolling: 0x2A204 scrolls 2 hexes (0x15BF1C) through 0x2A29E(dx, dy) while the mouse
  (0x15C70C/0x15C710, INT 33h position / 8 with range 0..5112 x 0..3832) is at x 0/639 or
  y 0/479. It is an external procedure now (SR_map_edge_scroll → Game_map_edge_scroll in
  video.c): same edges plus the arrow keys, at most one step per 100 ms (the loops poll it
  ~100000 times a second). 0x2A29E is exported as `map_scroll_by`.
* The mouse mapping had made x 639 / y 479 unreachable (pixel * (range+1) / 640); now the
  first and last pixels map to the ends of the range. A click captures the mouse
  (SDL_SetWindowMouseGrab); Ctrl+Alt or losing focus releases it (Ctrl+F10 is KDE's Present
  Windows shortcut).

Versions (patches/ in the game folder): the installed STEEL.EXE there is v1.00 (1146943 bytes,
md5 2821393362da3a4ab9cdb1369aea5a2f); the CD installs v1.1, which is what is recompiled here.
`sp_v12_ssi.exe` is an RTPatch updater: it needs v1.1 and SCEN/SAVE/EMAIL\HOLDER.TXT from the
installer archive (DATA.A00), and updates STEEL.EXE to v1.2 (1215455 bytes, md5
702ee2143a12c8b344a95d3d3a596ca4) plus README.TXT, OBBR/OBJA/OBMA/OBPZ/OBUS, SB16.DIG, SBLASTER.DIG
and adds SNDSYS.DIG, README.BAT, SAVE\STEEL.PRF. `STEEL_v12_novastar.EXE` (= steel12n.zip) is
"v1.2N" (1215507 bytes): the same code with the version text and three campaign titles of
NovaStar's expansion disk. v1.2 also uses Miles 3.1.2 instead of 3.0.4, so a v1.2 port needs
new addresses for every SCI entry and the AIL hooks.

Not done / not tested: modem and e-mail play, MIDI (the original install has no MDI.INI either),
the VFX driver functions 10 and 12 (never called), long campaigns end to end.
Launchers: `~/Steel_Panthers/steelpanthers-native.sh`, menu entry "Steel Panthers (native)".

## Port to v1.2 (2026-11-20)

The v1.1 SCI files were translated, not rebuilt by hand:

* `tools/addrmap.py`: code addresses are matched as byte windows with fixup targets and E8/E9
  displacements masked, searched only between "anchors" (function starts whose window is unique and
  whose order agrees in both builds); data addresses are mapped through the fixups of the code
  that references them (majority vote). A plain unique-window match was not enough: 10 of the 35
  AIL wrappers matched a look-alike wrapper.
* `tools/ailfind.py`: AIL API functions by their trace strings; port_sci.py uses these names for
  every AIL address. Checked: each hooked function is called and starts with the wrapper prologue
  (`AIL_install_MDI_INI` is not called by v1.2; its real start is 0xa3dfe).
* `tools/port_sci.py OLD.EXE NEW.EXE OLD_SCI NEW_SCI OLD_AIL NEW_AIL`: rewrites every loc_/sp_jt_
  address, checks the replaced instruction bytes, falls back to the shift of the neighbouring
  addresses for repeated code (the CLIB fill blocks), and checks that the order is preserved.
* Candidates: `SR.exe --list_invalid_code_fixups` on v1.2 gave 242; 192 correspond to v1.1
  candidates rejected as data, 2 to accepted ones (b2070, b5c19). Of the 48 new ones 11 are code:
  b02e0, b3280, b5bf4 (the v1.2 forms of AIL internals a6c50, a9a10, ac164) and the mixer routine
  table at 0xbcac8 (bd94f, bd9fe, bdaa1, bdb30, bdbdd, bdcc4, bdd99, bde60); the rest are tables.
* AIL is a newer release ("Mel 3.1.2" vs 3.0.4): the same API except `AIL_install_DIG_INI` and
  `AIL_install_MDI_INI`, which now take a driver handle pointer and return 0 (success), 1 (no INI
  file) or 2 (failure); ail.c follows that.
* Runtime addresses (sp.h, G_*): data object base 0xd0000, Watcom cstart variables 0xd309c… →
  0xe3810… (+0x10774), BSS end 0x16db9c, argc/argv 0x16d6fc/0x16d700, mouse 0x16cea8/0x16ceac,
  scroll step 0x16c69c; edge-scroll check 0x2b65c, scroll function 0x2b6f6.

Key v1.2 addresses: `main_` 0x1068a, `__InitRtns` 0xad8e8, `__CMain` 0xad899, `exit` 0xa0912,
DLL loader 0xa6c8f, `int386x` 0xa79e3, AIL API 0xa0f89 – 0xa4ffa, CD library 0xb76ef – 0xb8394.

Tested on v1.2 (offscreen SP_AUTOMATE runs in a patched copy of the game folder): intro, menus,
generated battle, ten AI turns with combat (152 sound effects, ~20 different samples, CD music),
air strike, save, load (also of a v1.1 save game), campaign purchase, editor, Exit to DOS,
map scrolling by keys and edges.

The owner's game folder was updated with the patch's output (STEEL.EXE, README.TXT/BAT,
OBBR/OBJA/OBMA/OBPZ/OBUS, SB16.DIG, SBLASTER.DIG, SNDSYS.DIG, SAVE\STEEL.PRF and the installer's
SCEN/SAVE/EMAIL\HOLDER.TXT); the replaced v1.00 files are in `patches/backup-v1.00/`.

## SR configuration so far (SR/x64/instruction_replacements.sci)

* 0xa432d: don't call the x87 emulator installer (smsw/sidt/INT 7 hook)
* 0xb4fbd, 0xb51cd: `lsl eax, ds` → 0xffffffff (flat 4 GB DS)
* 0x977d7: YI routine that would uninstall the emulator (cr0 writes) → ret

Status: the whole executable recompiles and assembles (`steel-asm.o`, 9 undefined runtime
symbols: SR_CheckTimer, x86_int, x86_in_al_dx, x86_in_al_imm, x86_out_dx_al, x86_out_imm_al,
X86_InterruptFlag, x86_mov_reg_mem_16/32).

## Runtime plan (games/Steel Panthers, SDL3)

1. Glue: Warcraft x64 pieces (SR-asm-calls.inc macros, c_main_/callback trampolines, x64_stack),
   fork x64-support (asm_int.asm, asm_inout.asm, asm_movmem.asm).
2. Startup: set the cstart variables (0xd30a0…0xd30d7), call `__InitRtns(255)`, then `main_`.
3. DOS services in C: INT 21h (files with case-insensitive path mapping, find first/next, date/time,
   exit), INT 31h (DPMI memory below 4 GB), INT 2Fh (MSCDEX: CD drive letter, Red Book audio from
   the BIN), INT 33h via `int386x` (mouse), keyboard.
4. External procedures: VFX DLL loader, AIL API, `int386x`/`intr`, `__GETDS`.
5. Timers: AIL timer callbacks run from SR_CheckTimer (polled at calls/backward jumps).
