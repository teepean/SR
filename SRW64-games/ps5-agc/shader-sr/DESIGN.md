# shader-sr: static recompilation of PS5 GPU shaders for NVIDIA

Static recompilation of the GPU side, the counterpart of what SR/SRW/SRW64 do for CPU code: every RDNA shader
(PS5 GPU, LLVM target `gfx1013`) a game uses is translated once, offline, into SPIR-V written for NVIDIA GPUs. The
Agc emulation (Kyty) then loads the translated shader by hash in place of its run-time translation.

## Why

A run-time translator that keeps the RDNA execution model emulates it instruction by instruction. Measured on one
large wave64 compute shader (a cloth solver, 7,984 instructions, 1,024 threads per workgroup), with the NVIDIA
driver's pipeline statistics (`VK_KHR_pipeline_executable_properties`) and `spirv-dis`:

| | count |
|---|---|
| SPIR-V instructions | 110,753 |
| of these: float multiply/add/sub (the actual math) | ~11,300 |
| OpPhi / OpSelect / OpBitcast | 14,770 / 12,545 / 10,406 |
| OpGroupNonUniformBallot (one per branch on exec/vcc) | 565 |
| NVIDIA binary | ~1.8 MB |
| registers per thread | 128 (the limit for 512 threads) plus 240-336 bytes of spills |
| shared memory | 46.5 KB of 48 KB (one workgroup per SM) |

The expansion comes from the execution model, not from the shader's work:
- every VGPR write is `select(exec, new, old)`;
- every branch on exec/vcc is a subgroup ballot (the wave decides together);
- a wave64 runs either as two lanes per NVIDIA thread ("lane pairs", doubling everything) or split across two warps
  with shared-memory exchange around every cross-lane operation.

Three smaller variants of the same shader that already run "split" still compile to ~1 MB with 128 registers. An
NVIDIA thread is not a wave lane, and most RDNA code does not need it to be.

## Execution model of the translation

One guest lane = one NVIDIA thread. A guest wave64 = 64 consecutive threads (two warps). Guest workgroup = host
workgroup with the same number of threads.

The PS5 shader compiler produces structured control flow, and most of it is lane-local. Measured on the 64-thread
variant of the cloth solver (`tools/rdnaflow.py`):
- 252 of 282 forward branches on exec/vcc are pure skips: the skipped code writes no scalar value that is read later;
- the cross-lane operations are one wave reduction (24 DPP, 6 permlanex16, 12 readlane), readfirstlane/s_ff1 on
  uniform values, and a few lane masks stored as data (ballots);
- shared memory carries data between lanes only across iterations of scalar (wave-uniform) loops that run with all
  lanes active at the iteration boundary, so the data needs a sync only there.

So the code is split into two kinds of regions:

**Lane-local regions** (most of the code): exec-mask manipulation becomes per-thread control flow. `v_cmpx` /
`s_and_saveexec` / `s_andn2_b64 exec` / `s_cbranch_execz` become `if`/`else`; exec-controlled loops become per-thread
loops. VALU instructions inside run unpredicated. SGPR lane masks are per-thread booleans (the thread's own bit).
SALU instructions run in every thread (wave-uniform values computed redundantly).

**Wave-coupled regions** (only where needed): regions that contain cross-lane operations, lane masks used as data,
scalar values that depend on which lanes are active, or shared-memory reads of data other lanes write without a
barrier in between. Here all 64 threads of the wave follow the same control flow, VALU instructions are predicated by
the thread's exec bit, and a branch on exec/vcc takes the wave-wide decision (subgroup ballot of each warp combined
through shared memory). The two warps of a wave are synchronised where data crosses between them (workgroup barrier
when the workgroup is one wave; a two-warp barrier in shared memory otherwise).

Wave idioms are recognised and replaced: the DPP/permlanex16/readlane reduction becomes a subgroup reduction plus a
shared-memory combine of the two warps; `readfirstlane` of a uniform value is the thread's own value.

## Memory

Guest memory is reached by guest address: `s_load`, `s_buffer_load`, `buffer_*` (through V# descriptors read at run
time, including `buffer_load_format_*` with the V#'s data/number format), `global_*`. The translation uses the
emulator's own guest memory map: Kyty's BDA page table (one device address per 16 KB guest page, 0 = not cached on
the GPU), the table Kyty's own device address loads use.
- `s_load`: one translation per load; V# accesses: one translation of the descriptor's base per instruction (Kyty
  keeps a V# range in one cached buffer); `global_*`: each lane translates its own address and marks the page as used.
- A miss reads 0 / skips the store and records the page in Kyty's fault bitmap (one page per thread, at the end of
  the program), which Kyty turns into a cached buffer after the submit, as for its own programs.
- Kyty prepares the memory: the translated pipeline is built with the pipeline layout of Kyty's program for the same
  guest code, and Kyty's whole dispatch preparation runs as for its own program (buffer sync, DMA reach, page table);
  before it, every V# range of the dispatch is made readable through the page table (Kyty may bind a streamed copy).
- The translated program's own push constants (112 of the layout's 128 bytes): page table, fault buffer, user SGPRs,
  workgroup id enables; or a table of user data per workgroup (one guest dispatch per workgroup, for batching).

LDS is a `shared` array of the guest LDS size.

## Exactness

- `v_mad_f32` / `v_mac_f32` are unfused; the translation keeps multiply and add separate (`precise`).
- f16 operands (`op_sel`, `v_fma_mix_f32`), SDWA selects and modifiers, `clamp`, `omod`, legacy multiplies,
  `v_sin/v_cos` in revolutions, `v_exp/v_log` base 2, `v_rcp/v_rsq` (approximations; denormal handling as the
  emulation does it).
- llvm-mc must decode as `gfx1013`: as `gfx1030` it rejects `v_mad_f32`/`v_mac_f32` and mis-decodes the following
  dword. It prints `v_fma_mix` literals truncated to 16 bits, so literals are taken from the raw bytes.

## Validation

Check mode (Kyty clone, `translatedShaders.cpp`, `KYTY_TRANSLATED_DIR=<dir>`, `KYTY_TRANSLATED_MODE=check`): one
dispatch in N of each translated program runs the translated program first, its outputs are copied away and the
inputs restored, then Kyty's program runs on the same inputs (the game keeps its results). `compare.comp` counts the
output dwords that differ and those beyond 1e-3 relative (as floats; bitwise for other values) and reports the first
of each. `KYTY_TRANSLATED_MODE=replace` runs only the translated program. A translation is used only for a dispatch
whose guest code matches its `.code` file byte for byte (`build.py` writes `<name>.spv` and `<name>.code`).

## Order

1. The cloth solver's 64-thread variant (one wave, no barriers): translator core, lane-local and wave-coupled
   regions, the wave reduction, LDS, the guest address table, driver statistics, A/B.
2. Its 512/1024-thread variants (several waves, `s_barrier`, global scratch memory).
3. The other large compute shaders of the frame (global illumination, particles, lighting), then all compute shaders.
4. Vertex and pixel shaders.
