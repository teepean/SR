# Building Interstate '76 (SR-I76) for Linux x64 and Windows x64

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

## Windows x64 (cross-compiled on Linux with MinGW-w64)

The generated assembler files are the same as for Linux x64 (step 2 above); nasm assembles them as win64
objects. Needs x86_64-w64-mingw32-gcc/g++ and SDL2 + freetype for MinGW in one directory (`WIN_DEPS`):

```sh
# SDL2: official development package (https://github.com/libsdl-org/SDL/releases, SDL2-devel-2.x-mingw.tar.gz)
tar xzf SDL2-devel-2.32.10-mingw.tar.gz
mkdir -p deps/include deps/lib
cp -r SDL2-2.32.10/x86_64-w64-mingw32/include/SDL2 deps/include/
cp SDL2-2.32.10/x86_64-w64-mingw32/lib/libSDL2*.a deps/lib/

# freetype: static library without optional dependencies
tar xJf freetype-2.14.3.tar.xz && cd freetype-2.14.3
./configure --host=x86_64-w64-mingw32 --prefix=$PWD/../ft --enable-static --disable-shared \
    --without-zlib --without-png --without-bzip2 --without-brotli --without-harfbuzz
make && make install && cd ..
cp -r ft/include/freetype2 deps/include/ && cp ft/lib/libfreetype.a deps/lib/

# build -> SR-I76.exe (needs SDL2.dll next to it)
cd "games/Interstate 76/SR-I76"
WIN_DEPS=/path/to/deps scons device=pc64-windows

# release archive SR-I76-windows-x64-<version>.zip
SDL2DLL=/path/to/SDL2-2.32.10/x86_64-w64-mingw32/bin/SDL2.dll sh "../release/windows/make_package.sh"
```

Testing with Wine: `I76_LOG=SR-I76.log I76_DEBUG=1 wine SR-I76.exe` in the game directory (a Windows GUI
program has no stderr). The Windows build uses Direct3D 11 by default (`graphics_api = opengl` for OpenGL).

## Notes

`gen_imports.py` (in SR-I76) regenerates the import stubs (`x86/`, `x64/` imports-asm.asm, imports.inc,
com-asm.asm, imports-stubs.c, com-stubs.c) from `imports.spec` and `com.spec`; the generated files are
committed. Run it after changing a spec file and after implementing an import (imports-stubs.c only
contains stubs for functions without a `CCALL name_c(...) {` definition in the C files).

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
| `games/Interstate 76/release/linux`, `windows` | start script, example configuration, readme, packaging scripts |
