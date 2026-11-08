# Aliens versus Predator 2 (+ Primal Hunt) on Windows x64

Cross-compiled on Linux with MinGW-w64 (`scons device=pc64-windows`): the same recompiled code as the Linux x64
build (SRW's x86-64 output, assembled by nasm as win64 objects) and the same runtime. Tested under Wine (GE-Proton):
AvP2's menu, movies and m1s1, Primal Hunt's exp1s1 play like on Linux. Not tried on a real Windows yet.

The recompiled code uses 32-bit absolute addresses, so the program and the game's memory stay below 2 GB: no ASLR,
and the program is linked at 0x3f0000 with an uninitialized section first (x64/lithimg.asm, 0x3f1000 - 0x510000)
where lithtech.exe's image goes (it must be at 0x400000; Wine and Windows put heaps and mapped files there before the
program runs). SConstruct makes the linker script for that (pc64-windows.ld) from MinGW ld's default one.

## What you need (on Linux)

- MinGW-w64 (`x86_64-w64-mingw32-gcc`/`g++`, GCC 13 or newer), nasm, SCons, Python 3, CMake.
- SDL2 and freetype for MinGW: a directory with `include/SDL2`, `include/freetype2` and `lib`
  (SDL2-devel-2.x-mingw's `x86_64-w64-mingw32`, freetype built with MinGW) - `WIN_DEPS=<dir>`; SDL2.dll from
  SDL2-devel's `x86_64-w64-mingw32/bin` goes next to the program.
- The game installed (AvP2 1.0.9.6; Primal Hunt 1.0.2.2 optional), and the recompiled modules generated
  (`SRW-games/Aliens versus Predator 2/gen_all.sh`, `GAME=ph gen_all.sh`) as for the Linux build.

## 1. Godot 4.7.2 as a DLL (LibGodot)

```
cd <godot 4.7.2 checkout>
scons platform=windows arch=x86_64 target=template_debug library_type=shared_library use_mingw=yes \
    d3d12=no winrt=no accesskit=no angle=no disable_path_overrides=no
```

Result: `bin/godot.windows.template_debug.x86_64.so` - a DLL despite the name (cross-compiled; Vulkan and OpenGL;
`d3d12=no`: no Agility SDK needed). Copy it as `libgodot.dll` next to the program.

## 2. godot-cpp (the commit the Linux build uses)

```
cd <godot-cpp> && git checkout b472519
scons platform=windows arch=x86_64 target=template_debug use_mingw=yes api_version=4.7
```

Result: `bin/libgodot-cpp.windows.template_debug.x86_64.a` (`GODOT_CPP=<godot-cpp>`).

## 3. FFmpeg (the Bink movies; optional - without it the movies are skipped)

A static build with only what Bink needs (`FFMPEG=<prefix>`):

```
./configure --prefix=<prefix> --enable-cross-compile --target-os=mingw32 --arch=x86_64 --cross-prefix=x86_64-w64-mingw32- \
    --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect --disable-debug \
    --enable-static --disable-shared --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
    --enable-demuxer=bink --enable-decoder=bink,binkaudio_rdft,binkaudio_dct --enable-protocol=file
make && make install
```

## 4. dmusic (the music; optional)

```
cd <dmusic> && git checkout b73b270 && git apply "<repo>/games/Aliens versus Predator 2/SR-AvP2/dmusic-patches/avp2.patch"
cmake -B build-mingw -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ -DDM_ENABLE_ASAN=OFF -DDM_BUILD_STATIC=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
```

(`DMUSIC=<dmusic>`: the Windows build uses `build-mingw/`.)

## 5. The program

```
cd "<repo>/games/Aliens versus Predator 2/SR-AvP2"
WIN_DEPS=<dir> GODOT_CPP=<godot-cpp> FFMPEG=<prefix> DMUSIC=<dmusic> scons device=pc64-windows
```

Result: `SR-AvP2.exe`, `SR-AvP2PH.exe`. Next to them: `SDL2.dll`, `libgodot.dll` (step 1's DLL, renamed) and the
`godot` directory (the embedded Godot project).

## 6. Running

From the game's directory (where lithtech.exe and the .REZ files are): `"<dir>\SR-AvP2.exe"`, or with
`AVP2_DEBUG=1` and `AVP2_LOG=avp2.log` set for a log. Settings: `SR-AvP2.cfg` in the game directory, as on Linux;
Primal Hunt's `avp2_dir` can be an absolute Windows path (`D:\Games\Aliens versus Predator 2`).

When it doesn't work: the log (`AVP2_DEBUG=1`, `AVP2_LOG=avp2.log`). "can't map lithtech.exe at 0x400000" lists
what is in the low 2 GB.
