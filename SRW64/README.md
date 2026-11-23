# SRW64

The static recompiler for 64-bit Windows executables (PE32+, x86-64) and 64-bit Linux executables (ELF64, x86-64).

This utility takes an original 64-bit Windows executable (or DLL) and information about the original executable as an input and produces x64 assembler version (NASM syntax) of the executable.
Linux executables (statically or dynamically linked) are supported too (see [Linux executables](#linux-executables-elf64)). The generated assembler version contains labels instead of addresses, so it can be modified and compiled into a new Windows executable (win64) or into an object file for Linux (elf64) together with replacement code (for example, replacements of Windows API functions).

SRW64 is based on [SRW](../SRW/README.md) (the static recompiler for 32-bit Windows executables) and uses the same workflow and the same information files (SCI files).

The utility uses files from [udis86](https://github.com/vmt/udis86 "Udis86 Disassembler Library for x86 / x86-64") library -- a fork which supports AVX, AVX2, FMA, BMI and other instruction set extensions; the source is in the *udis86* subdirectory (see [udis86/README.SRW64.md](udis86/README.SRW64.md)).

## Building

The utility can be built using [scons](http://scons.org/ "SCons: A software construction tool") -- the result is an executable file *SRW64.exe*.
Alternatively, the utility can be built using the script *build_gcc.sh*.

The utility uses c++ containers by default or optionally [Judy](http://judy.sourceforge.net/ "Judy Arrays Web Page") library (`scons judy=1`).

## Usage

    SRW64.exe [options] <input executable> <output.asm>

The utility reads information files (SCI files) and configuration file *SR.cfg* from the current directory.

Options:

* `--list_invalid_code_fixups=<file>` -- list code addresses referenced from code, which are not considered as code (candidates for *fixup_interpret_as_code.sci*)
* `--list_data_to_code_fixups=<file>` -- list code addresses referenced from data (candidates for *fixup_do_not_interpret_as_code.sci*)
* `--list_rva_displacements=<file>` -- list instructions with converted image relative displacements (in format of *rva_displacements.sci*)
* `--list_jump_tables=<file>` -- list jumps through jump tables: `<jump address> <table address> <kind> <number of converted entries> <bound or -1> <targets>`, kind is `diff` (entries relative to the table), `rva` (image relative entries) or `abs` (absolute addresses); jumps whose table wasn't found or recognized have 0 entries
* `--list_code_ranges=<file>` -- list address ranges of decoded instructions (`<start> <end>`, end is exclusive) and areas of code sections which are not decoded on purpose (`<start> <end> ignored` -- functions kept in original code, *ignored_areas.sci*)
* `--generate_relocations=<file>` -- guess relocations for executable with stripped relocations (aligned 64-bit values pointing into the image) and write them to file (in format of *relocations.csv*)
* `--add_tracing` -- add `TRACE_LABEL address` macro after labels of instructions

Output files:

* *output.asm* -- main assembler file (includes the other files)
* *segNN.inc* -- contents of sections
* *output.def* -- module definition file with exports (only if the executable has exports)
* *output.res* -- resources in .res format (can be compiled using windres/rc/cvtres and linked into the recompiled executable)
* *output.resdump* -- raw contents of the resource section
* *output.ld* -- linker script (only Linux executables)
* *output.imports* -- interpreter, needed libraries and imported symbols (only dynamically linked Linux executables)
* *output.secNN.bin* -- contents of section NN (only Linux executables -- the generated files include the original data and the original code which is kept using `incbin`)
* *output.symrename* -- renaming of symbols which aren't valid in NASM (only dynamically linked Linux executables, see below)

The generated assembler file includes files *srw64inc.inc* (from the *x64-support* subdirectory), *misc.inc* and *extern.inc* (game specific files, they can be empty).

The lists of jump tables and code ranges start with comment lines (`#`), the line `# address bias: 0x...` gives the difference between the listed addresses and the original addresses (position independent Linux executables are placed at a new address, see below).

### Comparison with IDA Pro

The script [tests/compare_ida.sh](tests/compare_ida.sh) compares the jump tables and the decoded code of SRW64 with the analysis of IDA Pro (headless, IDA 9 with IDAPython) -- switches found by IDA which SRW64 converted differently or not at all, jump tables which IDA doesn't know and code in IDA functions which SRW64 didn't decode:

    IDAT=/path/to/ida/idat tests/compare_ida.sh program /tmp/work

It runs SRW64 in the work directory (with `--list_jump_tables` and `--list_code_ranges`, *SR.cfg* and SCI files from the work directory are used), lets IDA analyze the program ([tests/ida_export.py](tests/ida_export.py), the database and exported files are written into the *ida* subdirectory and reused next time) and prints the differences ([tests/compare_ida.py](tests/compare_ida.py)).
The differences must be checked manually -- IDA is not always right (e.g. its number of cases can be too small for switches without range check), known harmless differences (padding, thunks, dead code after calls of functions which don't return, tables of absolute addresses converted as pointers in data, functions kept in original code) are listed separately.
*ida_export.py* can also decompile all functions using Hex-Rays (`SRW64_IDA_MODE=decompile`, see the script).

## Configuration file (SR.cfg)

* `use_pdata=yes|no` -- use functions from the exception directory (.pdata) as code entries (default: yes)
* `convert_exception_data=yes|no` -- convert exception directory, unwind information, SEH scope tables and C++ exception handling data (default: yes)
* `detect_jump_tables=yes|no` -- detect jump tables which are not relocated (default: yes)
* `detect_image_relative=yes|no` -- detect image relative addressing (register containing image base + RVA) (default: yes)
* `keep_original_code=yes|no` -- Linux executables: functions which can't be decoded are not recompiled, the original code is kept at its original address (default: yes)

## Information files (SCI files)

The addresses are written as `loc_` + 64-bit virtual address in hexadecimal, for example `loc_140001000`.

* *fixup_interpret_as_code.sci* -- `loc_X` -- addresses which are code (functions whose address is used only in code)
* *fixup_do_not_interpret_as_code.sci* -- `loc_X` -- addresses which are not code
* *noret_procedures.sci* -- `loc_X` -- functions which don't return (calls to known imports like `ExitProcess`, `exit`, `abort`, `_CxxThrowException` are recognized automatically)
* *displaced_labels.sci* -- `loc_X,displacement` -- references to address X are written relative to address X+displacement
* *external_procedures.sci* -- `loc_X,name[,alternative action]` -- calls to function X are replaced by calls to external function name
* *global_aliases.sci* -- `loc_X,name` -- global name for address X (for example the entry point)
* *ignored_areas.sci* -- `loc_X,length` -- areas which are not part of the output
* *instruction_replacements.sci* -- `loc_X,length,instructions` -- instructions (separated by `|`) which replace original instructions
* *rva_displacements.sci* (new) -- `loc_X,1` / `loc_X,0` -- force / suppress conversion of image relative displacement in instruction at address X
* *rva_data.sci* (new) -- `loc_X,count[,n]` -- count of 32-bit image relative addresses (RVAs) at address X (`n` -- targets are not code)
* *bssborder.csv* -- `address[,alignment]` -- split uninitialized end of data section into separate bss section
* *relocations.csv* -- `source,target[,type]` -- relocations for executables with stripped relocations (type: empty or `q` -- 64-bit address, `d` -- 32-bit address, `r` -- 32-bit image relative address)

## What is converted

* RIP-relative addresses are converted to labels (`[rel loc_X]`).
* Relocated 64-bit and 32-bit addresses are converted to labels (`dq loc_X`, `dd loc_X`).
* Calls and jumps through import address table are converted to calls and jumps to imported functions (`call ImportName`). Other accesses to import address table use pointers to imports (`[rel __imp_ImportName]`) -- import libraries provide them on Windows, the generated file defines them for elf64 output.
* Image relative addressing generated by MSVC (`lea reg, [__ImageBase]` followed by `[reg + index*scale + RVA]`) is converted to `RVA(label)`.
* Jump tables which are not relocated are detected and converted -- image relative tables generated by MSVC (including two-level tables with byte index table) and tables relative to table start generated by GCC and Clang.
* Functions from the exception directory (.pdata) are used as code entries. Exception directory, unwind information (including prolog offsets, which are converted to label differences), SEH scope tables (`__C_specific_handler`) and C++ exception handling data (`__CxxFrameHandler3` and `__CxxFrameHandler4`) are converted, so exceptions work in the recompiled win64 executable.
  * `__CxxFrameHandler4` data (Visual Studio 2019+) is compressed and contains offsets in functions -- IP to state maps and handler maps with continuation addresses are generated again (section *srweh4*, offsets as label differences) and functions which share the data (folded by linker) get their own copies.
* Delay load import descriptors are converted.
* Imports by ordinal without known name get name `dll_ordinal_N` (the ordinal is written into comment of `extern` declaration).

## Compiling the generated assembler version

The generated files can be compiled using [NASM](https://www.nasm.us/) (version 2.16 or newer is recommended):

    nasm -f win64 -O1 -w-number-overflow -i<path to SRW64/x64-support>/ output.asm -o output.obj
    nasm -f elf64 -O1 -w-number-overflow -i<path to SRW64/x64-support>/ output.asm -o output.o

When NASM reports "short jump is out of range", run *repair_short_jumps.py* (from *SRW64-games/template*) and assemble again (repeat until there are no errors).

### Windows (win64)

* Image relative addresses use `wrt ..imagebase` (native COFF relocation) and `__ImageBase` is provided by the linker.
* Name the entry point using *global_aliases.sci* and link without the startup files of the compiler (for example `x86_64-w64-mingw32-gcc -nostartfiles -nostdlib -Wl,-e,entry_name`), together with import libraries of the used DLLs.
* If the executable uses static TLS, name the TLS directory `_tls_used` using *global_aliases.sci*.
* Compile the resources (*output.res*) using windres (`windres -i output.res -O coff -o res.o`) or rc/cvtres and link them into the executable.
* The load configuration directory (security cookie, Control Flow Guard) isn't registered in the new executable -- the code works without it (the security cookie is initialized by the CRT code).

### Linux (elf64)

* ELF doesn't have image relative relocations, so the image base is zero and image relative addresses are 32-bit absolute addresses -- link the program without PIE (`gcc -no-pie`), so it is placed into the low 2 GB of address space.
* The recompiled code uses the Windows x64 calling convention -- replacement functions must use it too (`__attribute__((ms_abi))` in GCC/Clang).
* Accesses to TEB (`gs:` prefix) and to KUSER_SHARED_DATA (`0x7FFE0000`) are kept unchanged -- they must be emulated (or the code must be replaced).
* Code which reads the PE header through `__ImageBase` must be replaced.

## Linux executables (ELF64)

Statically linked executables (`gcc -static -no-pie`), dynamically linked executables (older Linux programs and games, for example FTL: Faster Than Light) and position independent executables (PIE -- current Linux programs) are supported.
Static-pie executables and shared libraries are not supported.

Executables which are not position independent don't contain relocations, so pointers in data can't be found reliably.
Therefore all sections except code keep their original addresses and only the recompiled code is moved (to a new address after the original image, printed as *Address of recompiled code*).
Position independent executables are placed at address 0x410000 or higher (aligned like the segments; the recompiled executable is not position independent and its headers are at address 0x400000) -- their pointers in data are known from relative relocations (also in compact format RELR), so the data isn't searched for pointers.

Executables without section headers (or with section headers which don't describe all loaded segments -- e.g. executables converted from other platforms) are supported too: sections are created from the program headers (executable segments are code, writable segments are data, the part after the file data is uninitialized data, thread local storage from `PT_TLS`, `.eh_frame` from `PT_GNU_EH_FRAME`) and the dynamic linking information is read using the dynamic tags.

* The generated linker script (*output.ld*) places the sections (named `srw.<original name>`, sections created from program headers are named `srw.load<segment>`) at their original addresses and the recompiled code (section `.text`) at the new address.
* The original data is included from *output.secNN.bin* (`incbin`) and uninitialized data is written as `resb <size>`, so the generated files stay small also for big executables.
* Program headers of the original executable which the linker doesn't create (OS specific headers like process parameters of converted executables, notes in data) are written into *output.phdrs*: the generated file contains placeholder notes (the linker creates `PT_NOTE` program headers for them) and the script [tests/fixup_elf.py](tests/fixup_elf.py) changes them into the original headers after linking: `fixup_elf.py program output.phdrs`.

* Only references to code are converted:
  * RIP-relative addresses, calls and jumps,
  * immediate values which are addresses of functions (in `mov`, `cmp` and `push` instructions),
  * jump tables (with offsets relative to table start or with absolute addresses),
    when the table address isn't loaded in the same block as the jump (e.g. it's loaded before a loop: `lea rdx, [rip+table]` ... `movsxd rax, [rdx+rax*4]; add rax, rdx; jmp rax`), it's found by a data flow analysis of the function (addresses loaded by `lea reg, [rip+table]` and copied by `mov`, propagated along jumps and targets of already converted jump tables) -- `Note: address of jump table found by data flow`;
    if that fails, the function is searched for `lea` instructions which load the base register (or its copy) and the table is used if its entries are instruction starts in the same function (when more tables are valid, the nearest load before the jump is used) -- `Note: address of jump table found in function` / `Warning: address of jump table not found`,
  * addresses of functions in data and groups of pointers into the same function (tables of labels),
  * addresses of IFUNC resolvers in IRELATIVE relocations (`.rela.plt`).
* Functions (and their sizes) are taken from the symbol table, so the executable should not be stripped (without symbols, the code is found only from the entry point and from references).
  Cold parts of functions (`foo.cold`) are considered to be parts of the functions.
* Functions which can't be decoded (AVX-512/EVEX instructions are not supported by udis86 -- glibc contains such variants of string and math functions) are not recompiled: the original code is kept at its original address (sections `srw.<name>.orig` with contents from *output.secNN.bin*) and references to it use labels in these sections (`srw_orig_<section>+<offset>`). Addresses outside of sections (e.g. undefined weak functions with address 0) use symbols `srw_abs_<address>` defined in the linker script.
  The functions are listed by SRW64 (`Note: function is kept in original code`). This can be disabled with `keep_original_code=no` in *SR.cfg* (then such functions must be handled using SCI files).
* Thread local storage keeps its layout (`.tdata`, `.tbss`), so offsets relative to `fs:` stay valid.
* The entry point gets the name `srw_entry` (used in the linker script).
* Calls of known functions which don't return (`exit`, `abort`, `__libc_fatal`, `__assert_fail`, `__cxa_throw`, `_Unwind_Resume`, ...) are recognized by name.
* Exception handling data: the original `.eh_frame` stays at its original address (it describes the original code). A new `.eh_frame` is generated for the whole program -- FDEs of recompiled code use labels (code offsets in call frame instructions are written as `DW_CFA_advance_loc4` with label differences), FDEs of original code which is kept are copied. C++ exception tables (LSDAs) of recompiled functions are generated again (section `srw.except`, call-site table with label differences).
  The program must be linked with `--eh-frame-hdr`, so the unwinder finds the new FDEs using `PT_GNU_EH_FRAME` (C++ exceptions, `pthread_cancel`, backtraces).
  Landing pads from the exception tables are code entries and FDEs which are not in the symbol table are used as functions (useful for stripped executables).

Statically linked executables: the recompiled program contains the recompiled C library (glibc) code of the original executable, so no replacement functions are needed:

    nasm -f elf64 -O1 -w-number-overflow -i<path to SRW64/x64-support>/ output.asm -o output.o
    ld -static --eh-frame-hdr -T output.ld output.o -o program

Dynamically linked executables use the same shared libraries as the original executable (the dynamic linking is done again by the linker):

* Imported symbols are external symbols in the generated file. Calls of PLT stubs and calls/jumps through GOT are calls/jumps of the symbols, other accesses to GOT use GOT of the new executable (`wrt ..gotpc`, `wrt ..gottpoff`), the original GOT entries get the addresses of the symbols (for original code which is kept).
* Objects copied into the executable (COPY relocations, e.g. `stdout`, C++ type information) stay at their original addresses: the generated file contains references which make the linker create COPY relocations, and the script [tests/fixup_elf.py](tests/fixup_elf.py) moves the relocations and the dynamic symbols to the original addresses after linking (the original code which is kept uses the original addresses too). Data relocations with symbols are written as `dq symbol+addend`.
* Symbols exported by the original executable are exported again (link with `-E`); `_init`, `_fini`, `.init_array`, `.fini_array` keep their roles (dynamic tags).
* Symbol names which are valid in NASM get prefix `$` (names like `div` or `abs` aren't keywords then), other names (e.g. names with `+`, `-` or starting with a digit) are written as `srw_sym_<index>` and renamed in the object file using *output.symrename*: `objcopy @output.symrename output.o`.
* The linker needs the libraries with the imported symbols -- the script [tests/mkstubs_elf.py](tests/mkstubs_elf.py) creates stub libraries from *output.imports* (same names, symbols with the same versions, types and sizes), the real libraries are used at runtime:

        objcopy @output.symrename output.o      # only if output.symrename exists
        mkstubs_elf.py output.imports stubs     # prints: -dynamic-linker <interpreter> [-rpath <path>] <stub libraries>
        ld <printed arguments> -E --eh-frame-hdr -T output.ld output.o -o program
        fixup_elf.py program output.imports     # only if output.imports contains lines "copy"

The script [tests/roundtrip_elf.sh](tests/roundtrip_elf.sh) does all steps.

### Executables which are not large address aware

Executables linked with `/LARGEADDRESSAWARE:NO` can store pointers in 32 bits (Windows keeps their memory below 2 GB). SRW64 prints a note for them.
On Windows link the recompiled executable with `/LARGEADDRESSAWARE:NO`, on Linux run it using [SR64-loader](../SR64-loader) (it keeps all memory below 2 GB).
Executables with 32-bit absolute relocations must be linked below 2 GB on Windows too (`/BASE:0x400000` or `--image-base=0x400000`).

## Known limitations

* C++ exception handling data of GCC in Windows executables (MinGW: `.gcc_except_table`, `.eh_frame`) is not converted (in Linux executables it is).
* `__CxxFrameHandler4` data of functions with separated code segments is converted only for the first function which uses the data.
* AVX-512 (EVEX) instructions are not supported.
* Self-modifying code, code which jumps into the middle of instructions and similar code must be handled using SCI files (or patched).
* Leaf functions (functions without unwind information) whose address is used only in code are recognized heuristically -- check `--list_invalid_code_fixups`.

## Tests

See [tests/README.md](tests/README.md).
