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

Lane pairs: one NVIDIA thread runs guest lanes j and j + 32 of its wave, so one guest wave64 is one warp and a guest
workgroup of N lanes is a host workgroup of N / 2 threads. Every per-lane instruction is translated twice (VGPR
`v<n>_0` / `v<n>_1`, exec bit `EXB0` / `EXB1`); scalar instructions run once per thread (wave-uniform values computed
redundantly by the 32 threads, as the guest's scalar unit computes them once per wave).

- exec is the thread's two bits (`EXM`); a lane mask in SGPRs keeps the thread's two bits in place;
- every branch on exec or a lane mask takes the wave's decision with a subgroup vote (`subgroupAny`), so control
  flow is the guest's own, wave-wide; VALU instructions are guarded by the lane's exec bit;
- cross-lane operations are subgroup operations of the one warp (ballot per half, readlane / readfirstlane by
  shuffle, DPP and permlane by shuffles); LDS written by other lanes of the wave is ordered by a subgroup barrier,
  `s_barrier` is the workgroup barrier.

The first model (one lane per thread, a wave in two warps) needed a handshake between the two warps at every wave
decision; inside the cloth solver's loops that cost more than the solver itself (2x slower than the emulator's own
program); lane pairs have no cross-warp synchronisation at all.

## Memory

Guest memory is reached by guest address: `s_load`, `s_buffer_load`, `buffer_*` (through V# descriptors read at run
time, including `buffer_load_format_*` with the V#'s data/number format), `global_*`. The translation uses the
emulator's own guest memory map: Kyty's BDA page table (one device address per 16 KB guest page, 0 = not cached on
the GPU), the table Kyty's own device address loads use.
- Scalar loads: the emulator's own translation reads the tables behind `s_load` chains flattened on the CPU, so the
  GPU copies of CPU-written tables may be stale. The translator follows the chains from the user data
  (`scalar_reach`: nodes are pointers in user SGPRs or loaded by `s_load`, plus the V#s `s_buffer_load` reads
  through) and writes them to `<name>.reach`; before each dispatch Kyty resolves the chains on the CPU and uploads a
  snapshot of every small (<= 4 KB) clean range, one ScalarTable entry per node in node order. Each `s_load` knows
  its node at translation time and checks only that entry (`sxlate_k`; the hot loops issue dozens of scalar loads
  per iteration, so no search); `s_buffer_load` translates its V# once (`sbuf_xlate`, shared by all loads through
  it); a node without a snapshot (GPU-written, large) and a pointer the chains do not reach (loaded at a run-time
  offset) are read through the page table, as the emulator's own program reads them.
- V# accesses: one translation of the descriptor's base per instruction (Kyty keeps a V# range in one cached
  buffer); `global_*`: each lane translates its own address and marks the page as used.
- `glc` / `dlc` buffer loads go through a `coherent volatile` reference: spin and compare-swap retry loops reread
  what other waves' atomics wrote (a plain load can be served stale from L1 or hoisted out of the loop: the wave
  spins forever, a GPU hang).
- Constant tables in the code: a V# built from `s_getpc_b64` + constants (`s_add_u32` / `s_addc_u32 0`, words 2-3
  from `s_mov`) whose pair feeds nothing else (`code_tables`) becomes an embedded const array; `build.py` extends
  `<name>.code` over the table, so the emulator's code match covers it.
- A miss reads 0 / skips the store and records the page in Kyty's fault bitmap (one page per thread, at the end of
  the program), which Kyty turns into a cached buffer after the submit, as for its own programs.
- Kyty prepares the memory: the translated pipeline is built with the pipeline layout of Kyty's program for the same
  guest code, and Kyty's whole dispatch preparation runs as for its own program (buffer sync, DMA reach, page table);
  before it, every V# range of the dispatch is made readable through the page table (Kyty may bind a streamed copy).
- The translated program's own push constants (112 of the layout's 128 bytes): page table, fault buffer, user SGPRs,
  workgroup id enables; or a table of user data per workgroup (one guest dispatch per workgroup, for batching).
- Bypass (Kyty clone, `KYTY_TRANSLATED_BYPASS=1` with batch mode): a dispatch of a program with a `<name>.bypass` is
  batched before any of Kyty's preparation (Kyty's execute thread is the frame's bottleneck). The host then does
  Kyty's memory work for the dispatch itself, at take time so that the bookkeeping keeps the guest order: every
  range the dispatch reads is registered and synced, every range it writes too and is then marked GPU-written in
  the open tick (marks at flush time, or a generation-gated full sync instead of per-dispatch syncs, broke the cloth;
  marking a whole V# whose range also holds CPU-owned data the program does not write broke other rendering). The
  ranges: `read` / `write <sgpr> <o1> ... <on>` (p = the user pointer, p = *(p + o) for each offset but the last,
  the V# at p + on), derived by `store_vsharps` (reaching definitions of the SGPRs: each SGPR of the V# reached by one
  `s_load` / `s_buffer_load` with a constant offset, whose base is again such a chain) for a program whose loads and
  stores all have such chains; or `walker cloth 0|1`, the host's exact walker of the cloth solver (collider sets
  reached at run-time offsets, palette writes only at the entries it writes). `build.py` writes the chains with
  `SHADER_SR_BYPASS=1` (opt-in: whole-V# write marks), `bypass_decl.py` a declaration for a native replacement.

Every translated loop counts its back edges and ends after `LOOP_LIMIT` (65,536) iterations: a translation that
would spin gives wrong results instead of hanging the GPU (Xid 109, context switch timeout).

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

## Native programs

`native.py` builds a hand-written GLSL replacement of a guest program on the same runtime (prelude: guest memory
through the page table, V# decoding, batched user data). It drops in where the translation would (`<name>.spv`
next to `<name>.code`). `native/cloth.glsl` is the first one: the 1024-thread cloth solver, rewritten operation for
operation (every multiply and add rounded alone, `fma` where the guest fuses), with NVIDIA-native threads (one cloth
instance per workgroup, subgroups of 32, no guest-wave emulation). Its binary is ~470 KB instead of ~1.8 MB, and the
largest instance of a frame runs in 0.68 ms instead of 3.1 ms.

`clothbench.cpp` replays one captured frame (`hooks` `SRH_CLOTH_CAPTURE`) through any set of programs and compares
the memory afterwards. Two things limit an exact comparison and have switches:
- instances of one phase share memory and constraint groups share vertices, so the guest itself is order dependent:
  `CLOTHBENCH_SERIAL=1` runs one instance at a time, and a capture can be made race free (no shared vertices);
- the driver compiles `sin` differently per shader: `SHADER_SR_SOFT_SIN=1` builds both with the same software sin.
With both, the native solver is bit-identical to the translation on a whole frame. `SHADER_SR_STOP_AT=<pc>` ends a
translation at a barrier, to compare intermediate states.

## Graphics stages

gcn2glsl.py `--stage vs|ps --gres <STAGE>_<hash>_<id>.gres` translates a vertex or pixel program against one of the
emulator's graphics modules (Kyty's graphics plug-in: KYTY_GRAPHICS_DUMP records the module's interface,
KYTY_GRAPHICS_TRANSLATED swaps the module for `<STAGE>_<hash>_<id>.spv`). gfx.py reads the record and the module's
SPIR-V (input/output locations, types, interpolation decorations, builtins).

- One guest lane per invocation (no lane pairs, LMASK = 1u); the host subgroup is the wave for the cross-lane
  instructions.
- Memory as in the compute translations: guest addresses through the BDA page table, the user SGPRs from the push block
  at byte 128 (geometry stage from s[user_data_base]), the s_load chains optionally from a per-draw ScalarTable
  (`.reach` next to the .spv; push bytes 240 pixel / 248 geometry).
- Vertex (NGG) programs: s2 / s3 as the emulator sets them (all lanes vertices and primitives), v5 / v8 = vertex /
  instance index; the fetch is the guest's own (its V#s through the page table); exp prim is dropped, exp pos0 /
  param<n> write gl_Position (with the emulator's clip distance for a vertex at the origin) and the module's outputs.
- Pixel programs: v_interp_p2 reads the module's interpolated input (p1 nothing), v_interp_mov its interpolation
  parameter (per-vertex inputs through gl_BaryCoordEXT, as the emulator does); exec starts as the live mask, s_wqm is a
  quad OR; exp mrt<n> converts per the target's export format into the module's output type; the vm export's exec is
  the pixel's valid mask (discard at the end); the kill block after the main s_endpgm is decoded.
- Images bind as the module's (vk_binding per image, the stage's sampler binding).

Validation: in-game screenshot comparison against the emulator's own modules (control-vs-control noise band).
