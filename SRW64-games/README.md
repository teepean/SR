# SRW64-games

The information about the original 64-bit Windows executables (and 64-bit Linux executables).

No games are supported yet. The *template* subdirectory contains files for a new game:

* *build-x64.sh* -- generates the assembler version of the original executable and assembles it (`build-x64.sh elf64` or `build-x64.sh win64`)
* *compact_source.py* -- shortens the generated files (repeated `db` and `resb` lines)
* *repair_short_jumps.py* -- repairs short jumps which are out of range (reported by NASM)
* *SR.cfg* -- SRW64 configuration
* *x64* -- information files (SCI files) and game specific include files (*misc.inc*, *extern.inc*)

To add a new game, copy the *template* subdirectory (for example to *Game/SRW64*), put the static recompiler (*SRW64.exe*) and the original executable there, set the name of the executable in *build-x64.sh* and add information about the executable into the SCI files (see [SRW64/README.md](../SRW64/README.md)).

Linux executables: set `INPUT=$NAME` in *build-x64.sh*, assemble with `build-x64.sh elf64` and link using the generated linker script: `ld -static --eh-frame-hdr -T game.ld game.elf64.o -o game_srw64` (statically linked) or `ld $(../../SRW64/tests/mkstubs_elf.py game.imports stubs) -E --eh-frame-hdr -T game.ld game.elf64.o -o game_srw64` (dynamically linked).

[NASM](https://www.nasm.us/) (Netwide Assembler) is needed to assemble the generated files.
