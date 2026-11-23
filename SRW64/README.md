# SRW64

The static recompiler for 64-bit Windows executables (PE32+, x86-64).

This utility takes an original 64-bit Windows executable (or DLL) and information about the original executable as an input and produces x64 assembler version (NASM syntax) of the executable. The generated assembler version contains labels instead of addresses, so it can be modified and compiled into a new Windows executable (win64) or into an object file for Linux (elf64) together with replacement code (for example, replacements of Windows API functions).

SRW64 is based on [SRW](../SRW/README.md) (the static recompiler for 32-bit Windows executables) and uses the same workflow and the same information files (SCI files).

The utility uses files from [udis86](https://github.com/vmt/udis86 "Udis86 Disassembler Library for x86 / x86-64") library -- a fork which supports AVX, AVX2, FMA, BMI and other instruction set extensions; the source is in the *udis86* subdirectory (see [udis86/README.SRW64.md](udis86/README.SRW64.md)).

## Building

The utility can be built using [scons](http://scons.org/ "SCons: A software construction tool") -- the result is an executable file *SRW64.exe*.
Alternatively, the utility can be built using the script *build_gcc.sh*.

The utility uses c++ containers by default or optionally [Judy](http://judy.sourceforge.net/ "Judy Arrays Web Page") library (`scons judy=1`).

## Usage

    SRW64.exe [options] <input.exe> <output.asm>

The utility reads information files (SCI files) and configuration file *SR.cfg* from the current directory.

Options:

* `--list_invalid_code_fixups=<file>` -- list code addresses referenced from code, which are not considered as code (candidates for *fixup_interpret_as_code.sci*)
* `--list_data_to_code_fixups=<file>` -- list code addresses referenced from data (candidates for *fixup_do_not_interpret_as_code.sci*)
* `--list_rva_displacements=<file>` -- list instructions with converted image relative displacements (in format of *rva_displacements.sci*)
* `--generate_relocations=<file>` -- guess relocations for executable with stripped relocations (aligned 64-bit values pointing into the image) and write them to file (in format of *relocations.csv*)
* `--add_tracing` -- add `TRACE_LABEL address` macro after labels of instructions

Output files:

* *output.asm* -- main assembler file (includes the other files)
* *segNN.inc* -- contents of sections
* *output.def* -- module definition file with exports (only if the executable has exports)
* *output.res* -- resources in .res format (can be compiled using windres/rc/cvtres and linked into the recompiled executable)
* *output.resdump* -- raw contents of the resource section

The generated assembler file includes files *srw64inc.inc* (from the *x64-support* subdirectory), *misc.inc* and *extern.inc* (game specific files, they can be empty).

## Configuration file (SR.cfg)

* `use_pdata=yes|no` -- use functions from the exception directory (.pdata) as code entries (default: yes)
* `convert_exception_data=yes|no` -- convert exception directory, unwind information, SEH scope tables and C++ exception handling data (default: yes)
* `detect_jump_tables=yes|no` -- detect jump tables which are not relocated (default: yes)
* `detect_image_relative=yes|no` -- detect image relative addressing (register containing image base + RVA) (default: yes)

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

### Executables which are not large address aware

Executables linked with `/LARGEADDRESSAWARE:NO` can store pointers in 32 bits (Windows keeps their memory below 2 GB). SRW64 prints a note for them.
On Windows link the recompiled executable with `/LARGEADDRESSAWARE:NO`, on Linux run it using [SR64-loader](../SR64-loader) (it keeps all memory below 2 GB).
Executables with 32-bit absolute relocations must be linked below 2 GB on Windows too (`/BASE:0x400000` or `--image-base=0x400000`).

## Known limitations

* C++ exception handling data of GCC (`.gcc_except_table`, `.eh_frame`) is not converted.
* `__CxxFrameHandler4` data of functions with separated code segments is converted only for the first function which uses the data.
* AVX-512 (EVEX) instructions are not supported.
* Self-modifying code, code which jumps into the middle of instructions and similar code must be handled using SCI files (or patched).
* Leaf functions (functions without unwind information) whose address is used only in code are recognized heuristically -- check `--list_invalid_code_fixups`.

## Tests

See [tests/README.md](tests/README.md).
