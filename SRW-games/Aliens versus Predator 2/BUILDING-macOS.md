# Aliens versus Predator 2 (+ Primal Hunt) on macOS

Status (2026-11-08): the macOS build is new and not yet tested on a Mac. Its memory layout (the recompiled code as
a separate image at a fixed low address, the program position independent and above 4 GB) is the Linux build's
`blob=1` mode, which runs the game (AvP2 levels, menus, movies, Primal Hunt).

## How it works

The recompiled game code is x86-64 that addresses everything with 32-bit addresses, so it must live below 2 GB.
On Linux it is linked into the program at 0x10000000. A macOS program can't do that: 64-bit Mach-O executables are
position independent and live above 4 GB, and Mach-O has no 32-bit absolute relocations. So:

- the recompiled code (SRW's modules and the runtime's assembler stubs) is linked on Linux into one image at
  0x20000000 (`tools/mkblob.py`): `SR-AvP2.blob` / `SR-AvP2PH.blob`, plus `blob_link_<program>.cpp` (where the
  image's symbols are, and which C functions it calls through its thunks);
- the program (only C/C++, built with clang) loads the image when it starts (`blob.c`), and the runtime's own
  allocations and the objects the game sees come from low memory (`lowalloc.h`, `lowobj.h`);
- `__PAGEZERO` is shrunk to 64 KB, so that the low 4 GB are free for the image and the game's memory.

It is an x86-64 program: on Apple Silicon it runs through Rosetta 2. Godot renders with Vulkan through MoltenVK
(Godot's Metal driver is only built for arm64).

## What you need

- Apple Silicon: Rosetta 2 - `softwareupdate --install-rosetta --agree-to-license`
- Xcode's command line tools - `xcode-select --install`
- An x86-64 Homebrew in /usr/local (on Apple Silicon next to the arm64 one in /opt/homebrew):
  `arch -x86_64 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"`
  then `arch -x86_64 /usr/local/bin/brew install sdl2 freetype ffmpeg pkg-config cmake jq`
- SCons and Python 3: `python3 -m pip install --user scons`
- The game installed (AvP2 1.0.9.6; Primal Hunt 1.0.2.2 optional), as for the Linux build.
- The recompiled-code kit, made on Linux (it contains code generated from the game's files: keep it private):
  `SRW-games/Aliens versus Predator 2/tools/mkmackit.sh <dir>` after `scons` in
  `games/Aliens versus Predator 2/SR-AvP2`.

All x86-64 builds below run under Rosetta on Apple Silicon (`arch -x86_64 ...`), so that every tool and library
is x86-64.

## 1. Godot 4.7.2 as a library (LibGodot)

```
git clone --branch 4.7.2-stable --depth 1 https://github.com/godotengine/godot.git
cd godot
sh misc/scripts/install_vulkan_sdk_macos.sh          # MoltenVK (the LunarG Vulkan SDK in ~/VulkanSDK)
arch -x86_64 scons platform=macos arch=x86_64 target=template_debug library_type=shared_library \
    disable_path_overrides=no angle=no
```

Result: `bin/libgodot.macos.template_debug.x86_64.dylib`.

## 2. godot-cpp (the commit the Linux build uses)

```
git clone https://github.com/godotengine/godot-cpp.git
cd godot-cpp && git checkout b472519
arch -x86_64 scons platform=macos arch=x86_64 target=template_debug api_version=4.7
```

Result: `bin/libgodot-cpp.macos.template_debug.x86_64.a`.

## 3. dmusic (the music; optional - without it there is no music)

```
git clone https://github.com/GothicKit/dmusic.git
cd dmusic && git checkout b73b270
git apply "<repo>/games/Aliens versus Predator 2/SR-AvP2/dmusic-patches/avp2.patch"
arch -x86_64 cmake -B build-static -DDM_ENABLE_ASAN=OFF -DDM_BUILD_STATIC=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=x86_64
arch -x86_64 cmake --build build-static
```

## 4. The program

Put the kit's files (`SR-AvP2.blob`, `blob_link_SR-AvP2.cpp`, and the same for `SR-AvP2PH`) into
`games/Aliens versus Predator 2/SR-AvP2/mac-kit/` (or pass `blob_kit=<dir>`), then:

```
cd "<repo>/games/Aliens versus Predator 2/SR-AvP2"
arch -x86_64 env PKG_CONFIG_PATH=/usr/local/lib/pkgconfig GODOT_CPP=<godot-cpp> DMUSIC=<dmusic> \
    scons device=pc64-macos
cp <godot>/bin/libgodot.macos.template_debug.x86_64.dylib libgodot.dylib
```

Result: `SR-AvP2`, `SR-AvP2PH`, `SR-AvP2.blob`, `SR-AvP2PH.blob` (the image must stay next to its program) and
`libgodot.dylib`; the `godot/` directory next to them is the embedded Godot project.

## 5. Running

Like on Linux: from the game's directory (where lithtech.exe and the .REZ files are), e.g.

```
cd "<AvP2 directory>"
AVP2_DEBUG=1 AVP2_LOG=avp2.log "<repo>/games/Aliens versus Predator 2/SR-AvP2/SR-AvP2"
```

Settings: `SR-AvP2.cfg` in the game directory (written with comments the first time; `show_fps = 1` shows the
frame rate). Primal Hunt: `SR-AvP2PH` in Primal Hunt's directory (`avp2_dir` in its config names AvP2's).

If macOS refuses to run downloaded files: `xattr -dr com.apple.quarantine <file or directory>`.

## When it doesn't work - what to send back

- `avp2.log` (with `AVP2_DEBUG=1`; `AVP2_DEBUG=2` for more).
- "can't map the recompiled code at 0x20000000", "can't map lithtech.exe at 0x400000" or "can't initialize the
  low memory allocator": something else took the low memory first; the log lists what is below 4 GB. Also
  `vmmap <pid>` of the running program if it gets that far.
- A crash: the report in Console.app (`~/Library/Logs/DiagnosticReports/`), and the log.
- A black or missing window: Godot's errors in the log (Vulkan/MoltenVK); try `rendering_driver = opengl3` and
  `rendering_method = gl_compatibility` in `SR-AvP2.cfg`.

Known risks, in the order they would show up: the program might not start at all with the small `__PAGEZERO`
under Rosetta; libraries or the system allocator might take the low addresses the game needs; Godot's window and
Vulkan under Rosetta; Rosetta's x87 floating point speed (the recompiled code uses the x87 FPU).
