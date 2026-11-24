# ps5-agc: recompiled PS5 executables with hooks and native GPU passes

Tools and hooks for running an SRW64 build of a PS5 game executable on Linux, and for replacing the game's RDNA2-specific GPU work (Agc command recording and RDNA2 shaders) with native code step by step.

The input is a PS5 executable that a relinker (AnyPS5 / AnyKyty) has converted to a Linux ELF64 executable (ET_EXEC at 0x400000, relative relocations, imports by NID). The executable runs on a Kyty-based Agc emulation (`libSceAgcDriver.prx`). SRW64 recompiles the converted executable like any other dynamically linked Linux executable (see [SRW64/README.md](../../SRW64/README.md)). The recompiled program runs at the same speed as the converted one. It is the base for the changes below.

Addresses in all files are PS5 addresses (the converted executable adds 0x400000; SCI files use the converted addresses).

## Contents

| Directory | Contents |
|---|---|
| `interposer` | `libysr.so`: an LD_PRELOAD interposer for the game's `sceAgc*` imports. It counts draws, dispatches and syncs per GPU marker, call sites, marker push sites with callers, and can dump data. |
| `hooks` | `libsrhooks.so`: game-side hooks linked into the SRW64 build, plus the SCI files (`instruction_replacements.sci`, `global_aliases.sci`, `extern.inc`) that redirect game functions to it. |
| `tools` | Analysis scripts for the converted executable and the interposer's logs. |

## Interposer (libysr)

`interposer/build.sh` generates trampolines for the imports in `agc_imports.txt` (`<NID> <name>`) and builds `libysr.so`. Each trampoline calls `ysr_hook()` with the original argument registers and the caller's frame, then jumps to the real function, so the caller's stack is unchanged. The hook is compiled with `-mgeneral-regs-only` and calls no libc functions. A reporter thread prints to stderr every 10 s.

Load it only into the game: `LD_PRELOAD=.../libysr.so ./game_srw64`.

| Environment | Effect |
|---|---|
| `YSR_INTERVAL`, `YSR_MARKERS`, `YSR_SITES`, `YSR_DETAILS` | report interval (s); numbers of markers, call sites and sync-argument lines printed |
| `YSR_PUSHMAP=<file>` | complete marker push map, rewritten every interval: count, pushes per frame, depth, marker, parent marker, call site and three callers from the rbp chain. PS5 code keeps frame pointers. `tools/pushmap.py` prints it as a tree. |
| `YSR_HELPER=<start>-<end>` | for calls made from inside this function (e.g. the game's dispatch helper), record the helper's caller |
| `YSR_WATCH=<start>-<end>` | record call sites of calls that have an argument register inside the range |
| `YSR_DUMP=<caller>:<rbp offset>`, `YSR_DUMP_DIR`, `YSR_DUMP_DELAY` | samples of dispatches made through the helper for one caller, plus the shader header and the memory it points to |
| `YSR_CLOTHDUMP=<file>` | one frame of a job's dispatch records (example of a game-specific dump) |
| `YSR_POKE=<addr>:<byte>[,...]`, `YSR_POKE_DELAY`, `YSR_POKE_US` | keep bytes of the game's data at a value, e.g. to switch off a feature |

## Hooks (libsrhooks) and the SRW64 build

`hooks/build.sh <converted eboot> <work dir>` builds `libsrhooks.so`, copies the SCI files and runs `SRW64/tests/roundtrip_elf.sh` with `LDFLAGS_EXTRA` (link with the hook library, `rpath $ORIGIN/sr`). At runtime `libsrhooks.so` goes into `<exe dir>/sr`.

How a game function is replaced:

* `instruction_replacements.sci`: a call site (or a thunk) is replaced by `call srh_xxx wrt ..plt`.
* `global_aliases.sci`: the original function is exported under a name (`loc_<address>,srh_xxx_orig`), so the replacement can still call it. Game helpers the replacement needs are exported the same way.
* `extern.inc`: declares the replacement symbols for the generated assembler file.

The hooks in `srhooks.c` (all off unless their environment variable is set):

| Environment | Hook |
|---|---|
| `SRH_CLOTH_RATE=N` | run a cloth simulation job only every Nth call (experiment) |
| `SRH_QUERY=1` | a GPU query that the main thread waits for (wave heights from an async compute queue) returns the previous frame's results from a per-entry cache, without waiting. Measured: no gain, because the emulated async queue runs more than a frame behind. Kept as an example of a data-level hook. |
| `SRH_NATIVE=particles` | the particle binning/finalize recorder (4 phases × N emitters of tiny RDNA2 dispatches) records one native call instead (see below) |

## Native passes

A native pass replaces the RDNA2 work of one engine function. The recompiled game records a marker in place of that work, on the same command buffer and in order with the rest:

    PushMarker(cb, "#native:<id>:<hex payload pointer>"); PopMarker(cb);

The Agc emulation recognizes the marker while it executes the command buffer. At that point in the stream it calls the native implementation (Vulkan) for `<id>` with the payload. The payload is allocated from the game's per-frame allocator and lives until the frame's GPU work is done. The emulation copies what it needs when it executes the marker. The game's own barriers stay before and after the call.

The `particles` payload, version 1, little endian:

| Offset | Field |
|---|---|
| +0 | u32 `'YNP1'` (0x31504e59) |
| +4 | u32 version |
| +8 | u32 entry count |
| +12 | u32 bytes per entry (48) |
| +16 | u32 flags (bit 0: phases 3 and 4 run) |
| +32 | 64-byte view data |
| +96, +112, +128 | three global buffer descriptors (V#) |
| +144 | entries |

Each 48-byte entry:

| Offset | Field |
|---|---|
| +0 | 16-byte bins V# |
| +16 | u64 counter block address |
| +24, +28 | u32 flags |
| +32 | u32 views |
| +36 | u32 phase mask |
| +40 | u64 emitter (debug) |

Thread counts are the views (phases 1 and 3) and the live count at counter block +0x30 (phases 2 and 4).

## Tools

The scripts read paths from the environment: `PS5_EBOOT` (converted executable), `IDA_FUNCS` (IDA function list, `<address> <name> - <start>-<end>` per line), `IDA_DECOMP` (optional Hex-Rays output).

* `pushmap.py <pushmap file> [min per frame]`: marker tree with the game functions that push each marker and their callers
* `map_sites.py <log> [pattern]`: maps addresses from logs (e.g. readback sites) to functions and disassembly
* `strxref.py <string>...`: code and data references to strings
* `dumptab.py <address> <before> <after>`: data qwords with relocations, strings and floats

Profiling the recompiled program: `perf record --call-graph fp` works for the game code, because PS5 code keeps rbp frame pointers.
