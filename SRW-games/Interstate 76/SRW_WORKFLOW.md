# SRW (Windows PE) workflow — practical notes

Learned while porting Interstate '76 (see `SRW-games/Interstate 76/PORTING_NOTES.md` in the SR repo).

## Build SRW on Linux
- `cd SRW && scons` → `SRW.exe` (a Linux ELF despite the name). Needs scons (pip) and a C/C++ compiler.
- nasm: avoid 2.15.03–2.15.05. Building nasm from source into `~/.local` works without root.

## Inputs next to the exe
| file | purpose |
|---|---|
| `relocations.csv` | `fixup_va,target_va[,imagebase]` — REQUIRED when the PE has stripped relocations (most game exes). SRW checks the dword at fixup_va == target_va. |
| `bssborder.csv` | `addr,align_minus` — split the uninitialised tail of .data into .bss (addr = .data start + SizeOfRawData, or where zero padding starts). |
| `SR.cfg` | `esp_dword_aligned=yes`, `ebp_dword_aligned=no` |
| `*.sci` | copied from `x86/` (see sci_files_guide.md) |

## Code roots in x86 output mode
- The PE entry point is NOT a root (the CRT startup gets replaced by C `main()`).
- Add WinMain in `global_aliases.sci`: `loc_<WinMain>,WinMain_`. The runtime's main() calls `WinMain_asm`.
- Callback functions (`push offset f`, WndProc, qsort comparators, C++ EH funclets) are only roots
  when listed in `fixup_interpret_as_code.sci`. Generate candidates with
  `SRW --list_invalid_code_fixups=cand.txt game.exe out.asm`, keep those that IDA sees as code heads,
  and drop jump tables. Iterate until nothing new appears.
- Put the unused CRT startup (`start`, `_initterm`/`_controlfp`/`_XcptFilter` thunks, its SEH scope table)
  in `ignored_areas.sci` (`loc_ADDR,length`). Remember that `_initterm` runs C++ static constructors,
  so the replacement main() must run them.

## Reconstructing relocations (stripped PE)
- IDA headless: `idat -A -c -o db.i64 -L log -S"script.py outdir" game.exe`. The script path must not contain
  spaces. IDAPython may need `idapyswitch --force-path /usr/lib/libpython3.X.so.1.0`.
- Code refs: for each instruction operand take the dword at `offb`/`offo`, but ONLY if it equals the decoded
  `op.addr`/`op.value`. Otherwise disp8+imm bytes create bogus refs (SRW error "error converting fixup").
- Data refs: scan every byte offset of non-code areas for values inside the image. Reject:
  low 16 bits == 0 (float halves), inside IDA strings, inside float/double/qword items, misaligned in
  dword/word items, 3 printable ASCII bytes pointing into .bss without an IDA head (short strings look like
  bss pointers when .bss is large), targets in .text that are not instruction heads, targets mid-block
  (linear decode from previous code head; previous insn not ret/jmp/padding) with no other xrefs,
  overlapping candidates (keep a single aligned one whose target is a head).
- Don't trust IDA's offset flag (`is_off0`) blindly: it can mark strings like "NEC\0" as offsets.
- Validate the heuristics on a game that already has a hand-made `relocations.csv` (Septerra Core):
  code refs matched exactly; data refs about 20 FP / 10 FN out of 21k.
- SRW errors that point at bad relocations: "wrong relocation target address" (target outside the section's
  VirtualSize), "output not found - sec - ofs" (a relocation points into the middle of an instruction).

## SRW limitations hit (and fixes)
- Imports by ordinal: SRW has hard-coded ordinal→name tables in `SRW_loader.c` (dsound, wsock32,
  comctl32). Add a table for the DLL (names from the DLL's export table, e.g. with pefile).
- bss split exactly at a labelled address (no padding gap) → nasm "label inconsistently redefined".
  Fix in `SR_initial_disassembly`: don't set `has_label` on the section-end sentinel when another
  section starts at that address.
- Port I/O (`in`/`out`) becomes calls to `x86_in_al_imm`/`x86_out_imm_al`. Replace such blocks with
  `instruction_replacements.sci` (`loc_ADDR,len,asm|asm ; comment`). Replace the I/O block itself:
  replacing only the function entry doesn't stop SRW emitting the rest of the body.

## Runtime architecture (games/<game>/)
- Imports come out as `extern Name`. `x86/extern.inc` does `%define Name Name_asm2c`. `Name_asm2c` stubs
  (macros `Call_Asm_StackN` in `asm-calls.inc`) call C `Name_c` in `WinApi-*.c` (SDL2 based).
- Septerra Core's `games/Septerra Core/SR-Septerra` is the template: kernel32/user32/gdi32/ddraw/dsound/winmm.

## Safety
- Don't use `rm` with globs or shell variables in helper commands. Write each SRW run into a new directory.

## Runtime lessons (Interstate '76)
- Generate asm2c stubs + weak "unimplemented" C stubs from an import spec (conv/arg dwords from IDA
  prototypes via ida_nalt.enum_import_names + idc.get_type); `double` args count as 2 dwords; `div` returns
  div_t in edx:eax (use uint64_t); import names equal to x86 mnemonics (`div`) must be renamed in the asm.
- Hex-Rays decompiles headless (`ida_hexrays.decompile`) - a full decompiled.c per module is the best guide.
- Recompiled DLLs: exports become global symbols; emulate LoadLibrary/GetProcAddress with a table and run the
  DLL's `__xc_a..__xc_z` constructors (global_aliases) instead of DllMain.
- Static-CRT DLLs: redirect CRT entry points with external_procedures.sci to one shared MSVCRT emulation;
  keep FILE in MS layout with `_cnt = 0` so inline getc macros call `_filbuf`.
- Win95 quirks matter: e.g. BitBlt returning the number of scan lines.
- ptrace attach is blocked (yama=1): run the game under gdb and send SIGINT to inspect hangs.
