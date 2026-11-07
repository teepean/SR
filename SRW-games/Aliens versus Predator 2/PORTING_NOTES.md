# Aliens versus Predator 2 + Primal Hunt — porting notes

Target: Linux x64 first (Windows x64 to follow, as for Interstate '76). Branch `AvP2` (from `I76_64bit`).

## The games

| | AvP2 1.0.9.6 (Europe) | Primal Hunt 1.0.2.2 (Europe) |
|---|---|---|
| retail | `avp2src/rez/games/avp2-eu` | `avp2src/rez/games/avp2-ph-eu` |
| engine | `lithtech.exe` (950,272 bytes, **no relocations**, base 0x400000) | `lithtech.exe` (1,007,616, no relocations) |
| server | `server.dll` (relocations) | `server.dll` |
| renderer | `d3d.ren` (DirectDraw 7 / Direct3D 7) | `d3d.ren` |
| game | `CShell.DLL`, `Object.LTO` inside `AVP2DLL.REZ` | `CSHELL.DLL`, `OBJECT.LTO` inside its `AVP2DLL.REZ` |

The two engines are not interchangeable: Primal Hunt added slots to `ILTCommon`, `ILTServer` and `ILTClient`
(avp2src `docs/PLAN.md`, "7. engine"), so AvP2's game DLLs need AvP2's engine and vice versa.

Reference material:
- `/home/teemu/sorsa/avp2src` (nolffan/avp2src): Monolith's 1.0.9.6 game source (`src/AVP2`, `src/LT2` SDK and
  lithshared), and reconstructions of both engines, renderers, servers and Primal Hunt's game DLLs; scored
  byte-for-byte against retail (game 95-97 %, engines 69-77 %).
- `/home/teemu/sorsa/AVP2-Reconstructed` (lemiur): AvP2 1.0.9.6 `lithtech.exe` (90.5 % matched) and `d3d.ren`
  (53 %); `include/renderstruct.h` is the renderer interface. **Its binary is another build** (937,984 bytes,
  SHA-1 432a8d41..., distribution not stated) than avp2src's EU 1.0.9.6 (950,272 bytes, fda88197...), so its
  addresses (`config/symbols.csv`) don't apply to ours; its source and structures do.
- Neither project links a complete engine on its own yet (the gaps are filled with retail bytes).

## Architecture (hybrid; decided 2026-11-07)

One Linux x64 executable, non-PIE, everything that touches engine data below 4 GB:

```
 +--------------------- AvP2 (host executable) ----------------------+
 | SRW x64 recompiled:  lithtech.exe  +  server.dll   (AvP2 1.0.9.6) |
 |        ^  ILTClient / ILTServer / ... (generated bridge)  |        |
 |        v                                                  v        |
 | native C++: game DLLs from Monolith's source       renderer:       |
 |   CShell  (client shell)                           RenderStruct    |
 |   Object.lto (server objects)                      on Godot        |
 | runtime: Win32 subset, Winsock (hostnet), REZ/files, Miles, DInput |
 +----------------------------------+--------------------------------+
                                    | LibGodot 4.7 (libgodot.so)
                              Godot: window, rendering, audio, input, video
```

- **Engine and server: SRW x64 recompile** of the retail binaries (no complete source exists). The code/data
  partition comes from AVP2-Reconstructed's `symbols.csv`; `lithtech.exe`'s relocations are reconstructed (as
  `i76.exe`'s were). Reconstructed source replaces recompiled functions where useful, as Interstate '76's game
  fixes do.
- **Game DLLs: native**, compiled with GCC from Monolith's 1.0.9.6 source (with avp2src's matching fixes) — fixable
  and extensible in C++ (widescreen, FOV, bug fixes behind a modern switch).
- **Low-memory model**: native code keeps every pointer it shares with the engine below 4 GB — the executable is
  linked low, the game's heap comes from a MAP_32BIT arena, the game runs on a stack allocated below 4 GB. The
  game's pointer-to-uint32 habits (user data, handles) then lose nothing, and the engine can hold native
  pointers in its 32-bit fields. Structures shared with the engine keep the engine's 32-bit layout (`ptr32`
  members in the bridge's copy of the SDK structures).
- **Bridge, generated from the SDK headers**: native `ILTClient`/`ILTServer`/`ILTCommon`/`ILTModel`/... objects whose
  methods call the recompiled engine (emulated x86 stack, `call_game`), and asm entry stubs for the engine's
  calls into the game (`GetClientShellFunctions`, `ObjectDLLSetup`, `IClientShell`/`IServerShell` vtables, class
  `EngineMessageFn`s, callbacks).
- **Godot via LibGodot**: the host creates the `GodotInstance` (`libgodot_create_godot_instance`) and steps it once
  per engine frame. `d3d.ren` is replaced by a native `RenderStruct` (`RenderDLLSetup`) on Godot's
  `RenderingServer`: world BSP + lightmaps -> meshes, models, sprites, particles, 2D surfaces/blits -> canvas.
  DirectInput, Miles (`mss32`, 89 functions), DirectMusic and Bink/Smacker map to Godot's input, audio and
  video.
- **Primal Hunt**: its engine and server recompiled the same way; its game DLLs (Third Law's code, never
  released; avp2src's reconstruction still has unwritten functions) are recompiled with SRW until that source is
  complete.

## Milestones

1. SRW x64 recompile of AvP2's `lithtech.exe` and `server.dll`; links with stubs.
2. Host + LibGodot: a Godot window; the engine starts and reaches its console with a null renderer.
3. Native game DLLs + bridge: the main menu (2D through the renderer's surfaces).
4. Godot renderer: worlds, models, sprites, effects; a mission is playable.
5. Sound, music, input, movies.
6. Multiplayer; Primal Hunt.

## Log

### 2026-11-07 — survey
- `tools/rez.py` (avp2work): RezMgr v1 reader (layout from `lithshared/rezmgr/rezmgr.cpp` `ReadDirBlock`): directory
  entries (1: pos, size, time, name), resources (0: pos, size, time, id, type, key count, name, description,
  keys); the type is the extension's 4 characters reversed. `AVP2DLL.REZ` holds only the two game DLLs.
- Game DLL exports: `CShell.DLL` GetClientShellFunctions, GetClientShellVersion, SetInstanceHandle;
  `Object.LTO` GetServerShellFunctions, GetServerShellVersion, ObjectDLLSetup, SetInstanceHandle. Both base
  0x10000000 with relocations.
- `d3d.ren` exports RenderDLLSetup, GetSupportedModes, FreeModeList (REA ev_7286ed94...).
- Monolith's game source: 885 files, 370k lines; 56 explicit pointer-to-integer casts (harmless under the
  low-memory model).

### 2026-11-07 — milestone 1: SRW translates AvP2's lithtech.exe
- Function table: avp2src `tools/enumfuncs.py` (plain Python) on the EU binary: 96.2 % of .text in functions.
- Relocations: IDA 9.3 headless + Interstate '76's `tools/ida_export.py` and `gen_relocs.py` (19,543;
  `SRW-lithtech/relocations.csv`). First try from AVP2-Reconstructed's symbol map produced nonsense in places,
  which is how the build mismatch showed. `bssborder.csv`: .data's uninitialized part from 0x4e2000.
- SRW fixes (SRW/SR_full_x64_instr.c): x87 `fldl2e`, `fldl2t`, `fldpi`, `fprem`, `fxam`, `fldenv`, `fnstenv` were
  unknown to the x64 translator; image-base relocations (`,i`: `lea eax, [edi+0x400000]`, RVA arithmetic)
  crashed it (it indexed `section[]` with the image base).
- `instruction_replacements.sci`: 0x4b91bd `cmp esi, fs:[0]` (CRT SEH unwind; SRW translates only mov/push/pop
  with fs:) -> `x86_read_fs_dword` + `cmp esi, r9d`.
- Result: SRW rc 0; nasm (after short-jump repair) leaves only imports, runtime helpers (x86_int, x86_mov_*)
  and four labels inside the CRT's memcpy/memmove (jump tables interleaved with code).
- Next: the statically linked CRT (libcmt, 0x4b42e0-0x4c3675, 481 functions incl. startup) is redirected to
  runtime implementations (external_procedures.sci) and its bodies excluded (ignored_areas.sci), as for the
  Interstate '76 DLLs; then server.dll.
- LibGodot: Godot 4.7.2-stable (`/home/teemu/sorsa/godot`), `scons platform=linuxbsd target=template_debug
  library_type=shared_library` -> `bin/libgodot.linuxbsd.template_debug.x86_64.so` (2.5 minutes, 32 cores).

### 2026-11-07 — milestone 2 (first part): the recompiled engine boots to the client shell
- Code roots: SRW doesn't take the PE entry point (or anything only reachable from it) as code, so WinMain and
  everything below it was never translated at first. `fixup_interpret_as_code.sci` = every IDA function start
  + the entry; `global_aliases.sci` loc_4B9F1B = lithtech_start (main.c calls the CRT startup, which runs the
  C++ constructors and WinMain as on Windows).
- The statically linked CRT (libcmt, 0x4b73d0-0x4c6775, identified with avp2src's tools/libscan.py against
  toolchains/msvc-6.00-8966/LIB/libcmt.lib) is translated as is; only memcpy/memmove (jump tables between the
  code) go to the runtime (external_procedures.sci + ignored_areas.sci). 0x4c6290-0x4c64a0 is lithshared code.
- Relocations: `relocations_include.csv` adds what gen_relocs.py's heuristics rejected but are real: a C++
  initializer at 0x410000 (round values look like constants), three vtable slots, a console command entry.
- SRW fix: `mov fs:[0], esp` (every SEH frame) was translated with the native esp instead of r11d
  (X86REGSTR_ESP); I76 had it too, but nothing there dereferenced the SEH chain (the CRT's setjmp does).
- Runtime (games/Aliens versus Predator 2/SR-AvP2, from I76's): lithtech.exe's image is mapped at 0x400000
  as data (the CRT reads its headers, the resource functions its .rsrc); resource-only DLLs (ltmsg.dll) are
  mapped by LoadLibrary (peimage.c); real threads (pthreads, each with its own emulated stack - asm-cpu.c's
  stack is thread-local), events, waits, critical sections (mutex pointer in the struct's first dword), per
  thread TLS and SEH frame, heap lock; the CRT startup's kernel32 functions; the launcher's command line
  (-rez archives) is built when none is given.
- Imports spec: tools/mkspec.py (IDA prototypes; reference: Interstate '76's spec). A pointer to a 64-bit type
  is one stack slot (QueryPerformanceCounter was wrong and unbalanced the emulated stack).
- avp2src's reconstructed engine source (src/engine/lithtech/clientwin.cpp ...) explains the code at each
  address, but its AvP2 reference is again another build (addresses shifted).
- Now: the engine opens all REZ archives, extracts cshell.dll from AVP2DLL.REZ into temp\ and calls its
  GetClientShellFunctions - the game DLLs are next.
