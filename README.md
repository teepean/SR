# SR

A project to [statically recompile](https://en.wikipedia.org/wiki/Binary_translation "Static binary translation") DOS and Windows game executables to create Windows (x86/x64) or Linux (x86/x64/arm/arm64/riscv64) or MacOS (x64/arm64) versions of the games.

Following DOS games are supported:

* [Albion](https://en.wikipedia.org/wiki/Albion_\(video_game\) "Albion")
* [X-Com: UFO Defense (UFO: Enemy Unknown)](https://en.wikipedia.org/wiki/UFO:_Enemy_Unknown "X-Com: UFO Defense (UFO: Enemy Unknown)")
* [X-Com: Terror from the Deep](https://en.wikipedia.org/wiki/X-COM:_Terror_from_the_Deep "X-Com: Terror from the Deep")
* [Warcraft: Orcs & Humans](https://en.wikipedia.org/wiki/Warcraft:_Orcs_%26_Humans "Warcraft: Orcs & Humans")

Following Windows games are supported:

* [Septerra Core: Legacy of the Creator](https://en.wikipedia.org/wiki/Septerra_Core "Septerra Core: Legacy of the Creator")
* [Battle Isle 3: Shadow of the Emperor (Battle Isle 2220: Shadow of the Emperor)](https://en.wikipedia.org/wiki/Battle_Isle_2220 "Battle Isle 3: Shadow of the Emperor (Battle Isle 2220: Shadow of the Emperor)")

The source code is released with MIT license (except libraries, etc. by other people, which have their own license).
For the purpose of using the code in GPL projects, the source code is also released with GPLv2 or later and LGPLv2.1 or later.

<hr/>

The projects consists of following subprojects (read the readme files in subproject directories for more information):

* **SR**
  * The static recompiler (for DOS executables) itself.
  * It takes the original executable as an input together with information about the original executable and produces x86 / arm / llasm / x64 assembler version of the executable.
  * The generated x86/arm/llasm/x64 assembler version of the executable is not part of the project.
* **SRW**
  * The static recompiler (for Windows executables) itself.
  * It takes the original executable as an input together with information about the original executable and produces x86 / llasm / x64 assembler version of the executable.
  * The generated x86/llasm/x64 assembler version of the executable is not part of the project.
* **SRW64**
  * The static recompiler (for 64-bit Windows executables and 64-bit Linux executables) itself.
  * It takes the original 64-bit executable as an input together with information about the original executable and produces x64 assembler version of the executable, which can be compiled into Windows (x64) or Linux (x64) version.
  * Functions of the recompiled program can be replaced or wrapped by native code (instruction replacements, exported original functions, extra objects or libraries at link time).
  * The generated x64 assembler version of the executable is not part of the project.
* **llasm**
  * Program which converts *.llasm* file to [LLVM](https://llvm.org/ "LLVM") language-independent intermediate representation, which can be compiled to native code.
* **SR-games**
  * The information about the original DOS executables.
* **SRW-games**
  * The information about the original Windows executables.
* **SRW64-games**
  * The information about the original 64-bit Windows executables (template for new games).
  * *ps5-agc*: tools for SRW64 builds of PS5 executables converted to Linux executables (running on a Kyty-based Agc emulation): an interposer for the GPU API (`sceAgc*`), game-side hooks linked into the recompiled program, and native GPU passes which replace the RDNA2-specific work of engine functions.
* **games**
  * Game specific source code.
  * Together with the generated assembler versions of the executables, these files can be used to build Windows (x86/x64) or Linux (x86/x64/arm/arm64/riscv64) or MacOS (x64/arm64) versions of the games.
  * Uses plugins to play (or play better) music or to enhance the displayed image in DOS games.
* **midi-plugins**
  * Plugins used by the games to play MIDI (and other types) music.
* **scaler-plugins**
  * Plugins used by the DOS games to enhance the displayed image.
* **pycfg**
  * A configuration utility that can be used on Linux (sorry Windows/MacOS users) to change settings in the configuration files (without editing the files).
* **SR64-loader**
  * Loader for older 64-bit versions. It's no longer needed for them.
  * It can be used for Linux versions of programs recompiled by SRW64 from executables which are not large address aware (it keeps all memory below 2 GB).

<hr/>

Some notes:

* The project supports creating 32-bit or 64-bit, Windows or Linux or MacOS versions of the games.
* The generated versions are little-endian, not big-endian.
* The generated arm version supports softfp and hardfp calling conventions.
