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
| `YSR_FUNCS=<n>` | the n most frequent `sceAgc*` functions with their calls per frame (state setting calls included) |
| `YSR_PUSHMAP=<file>` | complete marker push map, rewritten every interval: count, pushes per frame, depth, marker, parent marker, call site and three callers from the rbp chain. PS5 code keeps frame pointers. `tools/pushmap.py` prints it as a tree. |
| `YSR_HELPER=<start>-<end>` | for calls made from inside this function (e.g. the game's dispatch helper), record the helper's caller |
| `YSR_WATCH=<start>-<end>` | record call sites of calls that have an argument register inside the range |
| `YSR_DUMP=<caller>:<rbp offset>`, `YSR_DUMP_DIR`, `YSR_DUMP_DELAY` | samples of dispatches made through the helper for one caller, plus the shader header and the memory it points to |
| `YSR_CLOTHDUMP=<file>`, `YSR_CLOTHSITE=<return address>` | one frame of a job's dispatch records (example of a game-specific dump; `tools/clothdump.py` prints it). In a recompiled program the call sites have new addresses: give the return address of the dispatch call in the SRW64 build. |
| `YSR_TABLEDUMP=<return address>:<rbp offset>:<bytes>:<header pointer address>` | one dispatch's user data table (the pointer is read at `rbp - offset` of the function calling the dispatch) and its shader header; the reporter thread writes the table, the header, the shader code (64 KB from header + 0x38) and the memory behind every buffer descriptor in the table to `YSR_DUMP_DIR` (after `YSR_DUMP_DELAY` seconds). `tools/disgcn.py` disassembles the code. |
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
| `SRH_NATIVE=cloth` | the cloth job's dispatch loop (one single-workgroup dispatch per cloth instance, a full drain between dependency phases) records one native call instead. Values can be combined: `SRH_NATIVE=particles,cloth`. |
| `SRH_SPIN_PAUSE=1` | the game's wait loops (65 loops spinning on `rdtsc` until a GPU label, a job or a query result is ready) execute `pause` in each iteration. Every loop tests a byte of the hook library, so the same build runs with and without it. |
| `SRH_QUERY_TRACE=1` | per frame, the time (CLOCK_MONOTONIC ns) the GPU query is recorded and the time its label becomes visible to the CPU (`[srhooks-qtrace]` lines; joined with the emulator's label trace by label address and value) |
| `SRH_WAIT_TRACE=1`, `SRH_WAIT_TRACE_FILE=<file>` | the start and end (CLOCK_MONOTONIC ns) of every wait in the main thread's GPU-bound waits: the job wait that the GPU-completion relay releases (site 1), the GPU query label wait (site 2), the job group wait (site 3), two GPU label fences in the frame submit function (sites 4, 5) and the GPU-completion relay thread that releases site 1 (site 6: its event-queue wait, site 7: event to release, with the label). Sites 4, 5, 7 add the label address and awaited value. One line `site tid entry_ns exit_ns` per wait, written every 100 ms (default file `srh-wait-trace.txt`). The four hook calls keep all registers and flags; measured overhead: none (19.3 fps traced vs 19.4) |
| `SRH_TUNABLE_DUMP=<file>` | once, at the first frame: the game's runtime tunable table (424 entries of 32 bytes: value at +0, id at +0x10; the quality code multiplies some of them, e.g. ids 4, 0x125, 0x126, into view distances) as text lines |
| `SRH_CLOTH_CAPTURE=<dir>` | when `<dir>/capture-now` appears (or at call `SRH_CLOTH_CAPTURE_AT`): the inputs of every cloth instance of that frame (tables, piece data, positions, palettes, collider sets: everything the solver reads) as `<dir>/cloth-capture.bin`, for the offline solver bench `shader-sr/clothbench.cpp` |
| `SRH_CLOTH_DUMP=<dir>` | once, at the first cloth job with work: the shader header (0x100 bytes) and code (up to 64 KB) of the 4 cloth pipelines, as `<dir>/cloth-slot<N>-header.bin` / `-code.bin` (`tools/disgcn.py` disassembles the code) |
| `SRH_NATIVE=pdraw` | the particle draw code no longer reads a GPU-written value (an emitter's first particle index) on the CPU: a GPU copy into the draw's user data table is recorded instead (`sceAgcDcbDmaData`). Needed with `particles`: without the CPU read in the finalize recorder, this read would wait for the GPU. A small assembler stub replaces one load in the middle of the function and keeps all registers. |

Profiling tools (`tools/`, numpy): `waitsum.py` sums the wait trace per site; `framechain.py` joins it with the emulator's label, queue and interrupt traces and execute timeline into the per-frame chain (frame-end submit, frame labels, which write released each main-thread wait and how long each leg took); `funcmap.py` builds the maps that `cpuprof.py` uses to attribute nsys CPU samples (SQLite export, DWARF backtraces) in the recompiled code to the original game functions, per thread. `exectree.py` prints one thread's inclusive call tree from an nsys SQLite export in ms per frame (below a root frame, e.g. the emulator's command processor), to split a thread's frame time into its paths. `gcnfootprint.py` lists a dumped shader's memory footprint from its `disgcn.py` disassembly (user-data and pointer-table bytes read, descriptors used, dynamic offsets). `callersof.py` is the reverse view: the call paths above every sample that holds a given frame (e.g. `mprotect` on the execute thread), and `broken.py` lists the outermost known frames of samples whose unwinds lost the thread's root (JIT code, missing unwind info). `gapjoin.py` takes an nsys capture with the Vulkan GPU workload trace and CPU sampling: GPU busy share, the idle gaps ("starved" when the next workload's vkQueueSubmit came after the gap began), and what the emulator's execute thread ran inside the starved gaps (inclusive and leaf frames, off-CPU time from the scheduler events). `rbsites.py` maps the emulator's `[kyty-readback-sites]` log lines (`KYTY_READBACK_SITES=1`: per 10 s the instructions that faulted on GPU-written memory, with count, time and data address) through the same maps, so a CPU read of GPU results that stalls a frame shows up as an original game address and function (host addresses are left for `addr2line` on the emulator library).

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

The `cloth` payload, version 1, little endian:

| Offset | Field |
|---|---|
| +0 | u32 `'YNP1'` |
| +4 | u32 version |
| +8 | u32 entry count |
| +12 | u32 bytes per entry (24) |
| +16 | u64 shader code address per pipeline slot (4 slots) |
| +48 | u32 thread group size per slot (4 slots) |
| +64 | entries, in the game's order (sorted by dependency phase) |

Each 24-byte entry:

| Offset | Field |
|---|---|
| +0 | u64 address of the instance's 304-byte user data table |
| +8 | u32 phase (an attached piece runs in a later phase than its parent) |
| +12 | u32 pipeline slot (0: ≤ 64 vertices, 1: ≤ 512, 2: ≤ 1152, 3: more) |
| +16 | u32 vertex count |
| +20 | u32 reserved (0) |

The native side inserts its own barriers between phases. For an unknown pipeline (e.g. the PS5 Pro variants) the hook lets the original loop run.

## Tools

The scripts read paths from the environment: `PS5_EBOOT` (converted executable), `IDA_FUNCS` (IDA function list, `<address> <name> - <start>-<end>` per line), `IDA_DECOMP` (optional Hex-Rays output).

* `pushmap.py <pushmap file> [min per frame]`: marker tree with the game functions that push each marker and their callers (`SR_EXE=<SRW64 build>` when the log comes from the recompiled program)
* `srmap.py <SRW64 build> <address>...`: maps addresses of the recompiled program back to the original program, using the `loc_<original address>` symbols (also a module for the other scripts)
* `disgcn.py <code file> [bytes]`: PS5 GPU (gfx1013) disassembly of a dumped shader up to the first `s_endpgm` (needs `llvm-mc`)
* `rdnaflow.py <code file> [--blocks]`: control-flow and lane-structure analysis of a dumped compute shader for a
  lane-per-thread translation: basic blocks, exec-mask writes, branches on exec/vcc (pure per-lane skips vs. skipped
  code with live scalar side effects), cross-lane operations (DPP, permlane, readlane, ballots), LDS accesses, barriers
* `gcnlits.py <code file> <out>`: every instruction with a 32-bit literal, taken from the raw bytes (llvm-mc prints
  `v_fma_mix` literals truncated to 16 bits)
* `clothdump.py <file>`: prints a `YSR_CLOTHDUMP` snapshot (work items per phase, pipelines with thread group sizes, per-record tables with the fields that are shared and those that differ)
* `map_sites.py <log> [pattern]`: maps addresses from logs (e.g. readback sites) to functions and disassembly
* `strxref.py <string>...`: code and data references to strings
* `dumptab.py <address> <before> <after>`: data qwords with relocations, strings and floats

Profiling the recompiled program: `perf record --call-graph fp` works for the game code, because PS5 code keeps rbp frame pointers (the recompiled code keeps them too). Code addresses in logs and profiles of the recompiled program are addresses of the SRW64 build: map them with `srmap.py`.
