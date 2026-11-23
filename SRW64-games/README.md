# SRW64-games

The information about the original 64-bit Windows executables.

No games are supported yet. The *template* subdirectory contains files for a new game:

* *build-x64.sh* -- generates the assembler version of the original executable and assembles it (`build-x64.sh elf64` or `build-x64.sh win64`)
* *compact_source.py* -- shortens the generated files (repeated `db` and `resb` lines)
* *repair_short_jumps.py* -- repairs short jumps which are out of range (reported by NASM)
* *SR.cfg* -- SRW64 configuration
* *x64* -- information files (SCI files) and game specific include files (*misc.inc*, *extern.inc*)

To add a new game, copy the *template* subdirectory (for example to *Game/SRW64*), put the static recompiler (*SRW64.exe*) and the original executable there, set the name of the executable in *build-x64.sh* and add information about the executable into the SCI files (see [SRW64/README.md](../SRW64/README.md)).

[NASM](https://www.nasm.us/) (Netwide Assembler) is needed to assemble the generated files.
