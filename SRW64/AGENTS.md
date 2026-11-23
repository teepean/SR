# SRW64 - instructions for agents

SRW64 statically recompiles 64-bit Windows executables (PE32+, x86-64) into relocatable NASM x64 source.
The output can be built into a new Windows executable (win64) or linked into a Linux program (elf64) together with replacement code.
64-bit Linux executables (ELF64 -- statically linked, dynamically linked, PIE) are supported too: data stays at original addresses (PIE: at 0x400000 + original address), only code is moved (see "Linux executables" in README.md).
User documentation (formats of SCI files, options, output conventions) is in [README.md](README.md) -- read it before changing behavior.

## Build

    cd SRW64
    scons                      # or: ./build_gcc.sh   (scons may not be installed: python3 -m venv venv && venv/bin/pip install scons)
    CFLAGS="-O0 -g" ./build_gcc.sh   # debug build (run under gdb)

Result: `SRW64/SRW64.exe` (a native Linux/Windows program despite the name -- same convention as SR/SRW).
The build must stay warning-free with `-Wall` (gcc) and must keep working with `scons judy=1` (Judy library) -- new per-section lists need implementations in both `container_helpers.*` and `judy_helpers.*`.

## Test (run after every change)

    tests/run_tests.sh
    WINDOWS_DIR=/path/to/Windows/System32 tests/run_tests.sh   # also recompiles sort.exe and where.exe made by Microsoft

All tests must print PASS. The tests need NASM, MinGW-w64, Wine, GCC and Python 3 (see [tests/README.md](tests/README.md)).
To try any 64-bit executable: `tests/roundtrip_win64.sh program.exe /tmp/work [console|windows]`, then run the result with Wine and compare with the original.
For a statically linked Linux executable: `tests/roundtrip_elf.sh program /tmp/work`, then run the result natively.

## Workflow: recompiling a new executable

1. Create a working directory (template: [../SRW64-games/template](../SRW64-games/template)) and put the original executable there. Never commit original executables or generated `.asm`/`.inc` files.
2. Run `SRW64.exe game.exe game.asm` and read stderr: entry point, TLS warning, notes about large address awareness, counts of converted exception data, warnings.
3. Create `global_aliases.sci` -- at least the entry point (`loc_<entry>,entry_name`), `_tls_used` for the TLS directory if reported.
4. Assemble: `nasm -f win64 -O1 -w-number-overflow -i../SRW64/x64-support/ game.asm -o game.obj` (or `-f elf64`); on "short jump is out of range" run `repair_short_jumps.py` and assemble again.
5. Link (win64): import libraries for the used DLLs (`tests/mkimplibs.py game.asm .` creates them from the `extern` lines), resources (`windres -i game.res -O coff -o res.o`), `-nostartfiles -nostdlib -Wl,-e,entry_name`.
   Link (elf64): `gcc -no-pie` with replacement implementations of all imported functions (`__attribute__((ms_abi))`), see `tests/msvc_patterns/runtime_linux.c`.
6. Run and compare with the original. Fix problems with SCI files (see below), iterate.

Linux executables: steps 3-5 are replaced by `nasm -f elf64 ...` and `ld -static --eh-frame-hdr -T game.ld game.o -o game_srw64` (statically linked) or `ld $(tests/mkstubs_elf.py game.imports stubs) -E --eh-frame-hdr -T game.ld game.o -o game_srw64` followed by `tests/fixcopy_elf.py game_srw64 game.imports` (dynamically linked); no entry point alias -- the entry point is `srw_entry`.
Test a game headless and isolated: `gamescope --backend headless -- ./game` with `HOME` set to a scratch directory (saves, settings) and an `.asoundrc` with `pcm.!default { type null }`; compare logs of the original and the recompiled game. Building with `keep_original_code=no` in `SR.cfg` checks that no original code is executed.
Read the notes: functions kept in original code (AVX-512), cold parts of functions, IRELATIVE relocations, FDEs in `.eh_frame` (converted / original code / failed) and converted LSDAs.

## Diagnosing problems

| Symptom | Cause / fix |
|---|---|
| `Error: invalid instruction: 0x...` | data decoded as code, or unsupported instruction (AVX-512) -- find who adds the entry (jump target, fixup), add the address to `fixup_do_not_interpret_as_code.sci` or `ignored_areas.sci` |
| `Error: instruction overlaps another instruction` / `Warning: label inside instruction` | jump into the middle of an instruction or data decoded as code -- use `fixup_do_not_interpret_as_code.sci`, `displaced_labels.sci` or `instruction_replacements.sci` |
| `Error: output not found` | an entry points inside an already decoded instruction -- same as above |
| `Warning: jump table not recognized` / `shorter than expected` | jump table bound not found -- check the table manually, use `rva_data.sci` (image relative entries) and `fixup_interpret_as_code.sci` for the targets |
| `Unrecognized language specific handler data` | listed per handler -- `__GSHandlerCheck` data contains no addresses (OK); other handlers: check if their data contains addresses |
| `unable to convert C++ exception handling ... map` / `unable to create copy of shared C++ exception handling data` | an address in `__CxxFrameHandler4` data is not at instruction boundary -- check the function; verify with `tests/verify_fh4.py original.exe recompiled.exe` |
| NASM `label ... inconsistently redefined` | two labels with the same address -- should not happen; check section splitting / end-of-section labels in `SR_output.c` |
| NASM `undefined symbol loc_...` | reference to a removed area (`ignored_areas.sci`, import/export tables) -- replace the referencing instruction or adjust the ignored area |
| Linker: undefined `__imp_Name` / `Name` | missing import library (win64) or missing replacement function (elf64); imported variables are only accessible through `__imp_Name` |
| Crash in raw bytes (`db ...`) in code section | a function was not disassembled (its address is used only by code and it has no unwind info) -- run with `--list_invalid_code_fixups=file` and add the address to `fixup_interpret_as_code.sci` |
| Crash on `movdqa`/`movaps` with address -1 | misaligned data -- data sections must keep the original offset in page (see `SR_write_section_header`) |
| Wrong results with `[reg+index*4+0x...]` | image relative displacement not converted (or converted wrongly) -- `--list_rva_displacements=file`, fix with `rva_displacements.sci` |
| ELF: `Note: function is kept in original code` | the function contains an instruction which udis86 can't decode (the bytes are printed, usually AVX-512 `62 ...`) -- OK, the original code is used |
| ELF: crash in original code (address below the recompiled code) | the program jumped to original code through a pointer which wasn't converted -- find the jump (see below), check jump tables and pointers in data |
| ELF: C++ exception terminates the program (`terminate called after throwing`) | the new `.eh_frame` is missing an FDE (`Warning: unable to convert .eh_frame FDE`) or the program wasn't linked with `--eh-frame-hdr`; a crash in raw bytes after a throw = landing pad not disassembled |
| ELF: ld `.eh_frame_hdr refers to overlapping FDEs` / NASM `expression is not simple or relocatable` in `.eh_frame` | an address range end was converted wrongly -- check `eh_label` / `eh_end_label` in `SRW64_elf_eh.c` (ends are rounded up to the next recompiled instruction, end of section = `srw_end_<section>`) |
| ELF (dynamic): crash in original code (`srw_orig_N`) with a pointer which looks unrelocated (small value) | position independent executable: a pointer to code wasn't converted -- check relative relocations (`Relative relocations converted: X/Y`) |
| ELF (dynamic): original code reads 0 from a library object (e.g. `stdout`) | COPY relocation wasn't moved to the original address -- run `fixcopy_elf.py` |
| ELF (dynamic): `symbol lookup error: undefined symbol: X` | the symbol was linked against a stub library but the real library doesn't define it -- check `X` in *game.imports* (symbols defined in the executable must not be imports) |
| ELF (dynamic): linker `undefined reference to X` | the import is missing in *game.imports* or the stub libraries, or an exported symbol is an alias of an import (`X equ <import>`) |
| ELF: glibc reports a fatal error at startup (e.g. `unexpected reloc type in static binary`) | a reference to the end of a data section was converted to the start of the next (code) section -- immediates at data/code boundaries are not converted (`SR_elf_is_function_start`) |

Runtime debugging (Linux executables): the linked program contains the `loc_<original address>` symbols, so gdb shows original addresses (`loc_4C6840+3`).
To find where the program leaves the recompiled code, single-step it with a gdb Python script until `$pc` is below the address of recompiled code and print the previous instruction.

Runtime debugging (Wine):

* Use a separate Wine prefix for tests (`WINEPREFIX=/tmp/prefix wineboot -i`), for GUI programs set the null display driver in it: `wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f` -- don't modify the user's default prefix.
* `WINEDEBUG=+seh` shows exceptions (code and address), `WINEDEBUG=+pid,+relay` shows API calls with process ids (filter the program's process id; all Wine processes are logged).
* The linked executable contains the `loc_<original address>` symbols: `x86_64-w64-mingw32-objdump -d --start-address=... -M intel program.exe` maps addresses in the new executable back to original addresses.
* Compare API call sequences of the original and the recompiled program to find the first difference.

## Code map

| File | Purpose |
|---|---|
| `SR_main.c` | command line, `SR.cfg`, order of passes, address -> section lookup |
| `SRW64_loader.c` | PE32+ loader: sections (long names, debug sections), relocations (DIR64, HIGHLOW, `relocations.csv`, guessing), imports (64-bit thunks, ordinals), exports, splitting of import/export/IAT ranges, notes about large address awareness; ELF files are passed to `SRW64_LoadElf` |
| `SRW64_elf.c` | Linux executables (ELF64): loader (sections, functions from symbol table and `.eh_frame`, cold parts, noreturn names, IRELATIVE relocations), prescan (instruction starts, data addresses referenced by code), functions kept in original code, pointers to code in data, `srw_orig_`/`srw_abs_` labels, linker script |
| `SRW64_elf_dyn.c` | dynamically linked Linux executables: dynamic symbols with versions/libraries, GOT entries, PLT stubs, COPY, data and relative (RELA, RELR) relocations, exports, references for COPY relocations, file *.imports* |
| `SRW64_elf.h` | ELF64 structures and constants |
| `SRW64_elf_eh.c` | Linux executables: `.eh_frame` parser (CIEs, FDEs, LSDAs), landing pads as code entries, new `.eh_frame` and LSDAs (section `srw.except`) for recompiled code |
| `SRW64_pdata.c` | exception directory: function list (`SR_find_pdata`), code entries, image relative fixups in .pdata, unwind info, SEH scope tables, C++ FuncInfo (FH3), delay load descriptors, prolog offsets as label differences (after disassembly), prescan of `lea reg, [__ImageBase]` |
| `SRW64_eh4.c` | C++ exception handling data version 4 (`__CxxFrameHandler4`): in place conversion of RVAs, regenerated IP to state maps and handler maps (section `srweh4`, offsets as `dd (((loc_A - loc_B) << 4) \| 7)`), copies for functions sharing the data |
| `SR_source_code_information.c` | loading SCI files |
| `SR_basic.c` | labels (`loc_%llX`, label expressions with displaced labels), application of fixups to data (`dq`, `dd`, `dd RVA(...)`) and code entries from fixups |
| `SR_full.c` | disassembly driver (entry list), ignored areas, instruction replacements, alignment of code and data in code sections |
| `SR_full_win64.c` | instruction conversion: RIP-relative operands, imports, relocations in instructions, image base register tracking (`run_state`), jump tables, control flow |
| `SR_full_helpers.c` | adding code entries, conditional jumps, relocated values in instructions, noreturn functions (incl. imports) |
| `SR_output.c` | writing the .asm/.inc/.def/.res/.resdump files, section declarations (page offset preservation), externs, `__imp_` pointers for elf64 |
| `container_helpers.*`, `judy_helpers.*` | per-section lists (C++ containers or Judy) |
| `x64-support/srw64inc.inc` | NASM macros for generated code: `RVA()`, `__ImageBase` (win64: linker symbol, elf64: zero) |
| `udis86/` | disassembler (fork with AVX2/FMA/BMI), local changes documented in `udis86/README.SRW64.md` |
| `tests/` | test programs and scripts |

Key internal conventions:

* Addresses are `uint64_t` virtual addresses, offsets inside sections are `uint_fast32_t`; print addresses with `PRIx64`/`PRIX64`, labels with `SR_get_label` / `SR_get_label_expr`.
* Fixup types: `FT_NORMAL` (32-bit absolute), `FT_DIR64` (64-bit absolute), `FT_RVA32` (32-bit image relative); `FF_NOENTRY` = target is only a label, not a code entry; `SEC_HEADER` = target is in the PE header (`__ImageBase+offset`).
* Output strings: `dd RVA(loc_X+n)` -- the `RVA()` macro argument must not be parenthesized (`wrt ..imagebase` doesn't work inside parentheses).
* FH4 compressed integers are generated in the 4-byte form (Wine doesn't decode the 5-byte form).
* Code entries come from: exception directory, exports, aliases, fixups from data, call/jump targets, `lea` of function addresses, jump tables, SCI files. `SR_check_code_reference` decides whether `lea reg, [rip+code]` is a function.
* Image relative addressing is only converted when the base (or unscaled index) register is known to contain the image base (`run_state.ib_mask`); hints for jump targets are stored in `hint_list` (register mask + jump table bound).
* The `int3` instruction ends linear disassembly (it is used as padding after calls to functions which don't return).
* `InputFormat` (`INPUT_PE` / `INPUT_ELF`) selects input specific behavior. ELF: `ImageBase` is the address of the first loaded segment, `CodeBase` the address of recompiled code, `section[].tls` marks TLS sections (`.tbss` is excluded from address lookups); `SR_get_label` is used for references (it returns import symbols for PLT stubs and objects copied into the executable), `SR_get_label_def` for label definitions; `SR_get_label` returns `srw_orig_<section>+<offset>` for original code which is kept (label in section `srw.<name>.orig`) and `srw_abs_<address>` for addresses outside of sections (defined in the linker script); `srw_end_<section>` is the end of a recompiled code section.
* udis86 must be initialized with `ud_set_vendor(&ud, UD_VENDOR_ANY)` (the default vendor makes Intel only instructions like `endbr64` invalid).

## Coding style

* Same style as SR/SRW: C (C89 compatible declarations at block start), 4 spaces, MIT license header in every file (keep the original copyright in files derived from SRW).
* Keep the generated output assemblable by NASM for both `win64` and `elf64` -- every change in output must be tested with both (`tests/run_tests.sh` does).
* Don't use unguarded `rm` with shell variables in scripts.
