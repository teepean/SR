# Building Interstate '76 (SR-I76) for Linux x64

The recompiled game consists of the generated assembler versions of the game's modules
(i76.exe, i76shell.dll, ZGLIDE.DLL, STRLKUP.DLL) and the runtime in `games/Interstate 76/SR-I76`
(Win32 API, DirectDraw, DirectSound, Glide → OpenGL, CRT, ...).
The generated assembler files are not part of the repository; they are created from the original game files.

The 64-bit build is the supported one. The 32-bit build (`scons` without `device`) still works,
but it's not developed further (see PORTING_NOTES.md).

## Requirements

- the original game files, e.g. the GOG version installed with Wine or extracted with innoextract
  (default location `~/.wine/drive_c/i76`, set `GAME=...` otherwise)
- gcc/g++, python3, nasm (not 2.15.03–2.15.05), scons
- development files of SDL2 and freetype

## Steps

```sh
cd "SRW-games/Interstate 76"

# 1. SRW with x64 output (built out of tree, the repository's SRW stays in x86 mode)
./build_srw64.sh /path/to/srw64-build

# 2. generate the assembler versions of the game modules
#    (a new directory $WORK/gen-NNN each run, default WORK=/home/teemu/sorsa/i76work;
#     the results are copied to games/Interstate 76/SR-I76/x64/<module>/)
ARCH=x64 SRW64=/path/to/srw64-build/SRW64.exe GAME=/path/to/i76 WORK=/path/to/workdir ./gen_all.sh

# 3. build the runtime and link the game -> SR-I76-x64
cd "../../games/Interstate 76/SR-I76"
scons device=pc64-linux

# 4. release archive SR-I76-linux-x64-<version>.tar.gz (stripped binary, I76.sh, SR-I76.cfg, readme)
sh "../release/linux/make_package.sh"
```

`gen_imports.py` (in SR-I76) regenerates the import stubs (`x86/`, `x64/` imports-asm.asm, imports.inc,
com-asm.asm, imports-stubs.c) from `imports.spec` and `com.spec`; the generated files are committed,
run it after changing a spec file.

## Running without installing

Run the binary in the game directory (it reads the game files from the current directory):

```sh
cd /path/to/i76 && /path/to/SR-I76-x64
```

Useful environment variables for testing: `I76_DEBUG=1|2` (log), `I76_INPUT_SCRIPT=file` (scripted
input), `I76_DUMP_FRAMES=dir` (existing directory), `SDL_VIDEODRIVER=offscreen` (no window),
`I76_HEAPCHECK=1`, `I76_HEAPGUARD=1` (heap debugging). Every SR-I76.cfg setting can be given as
`I76_<SETTING>`. See PORTING_NOTES.md for details.

## Where things are

| path | contents |
|---|---|
| `SRW-games/Interstate 76/SRW*/x86`, `x64` | SCI files per module (code fixups, relocations, aliases, game patches) |
| `SRW-games/Interstate 76/SRW/relocations.csv` | reconstructed relocations of i76.exe (its relocation table is stripped) |
| `SRW-games/Interstate 76/tools` | IDA export scripts and tools used to create the SCI files |
| `SRW-games/Interstate 76/PORTING_NOTES.md` | the porting log: problems, causes and fixes |
| `games/Interstate 76/SR-I76` | runtime sources |
| `games/Interstate 76/release/linux` | start script, example configuration, readme, packaging script |
