# udis86 for SRW64

This directory contains the disassembler library used by SRW64.

## Origin

* Base: the udis86 fork https://github.com/FlorianSoler/udis86
  (commit `5336633af70f3917760a6d441ff02d93477b0c86`, 2022-10-13).
  The fork merges upstream https://github.com/vmt/udis86 (master, including
  pull request #67 "Fix duplicate &lt;opr&gt; for sqrtss") with several other forks
  and supports AVX, AVX2, FMA, F16C, BMI1/2, ADX, AES, SHA, CET and others.
  Upstream udis86 1.7.2 (used by SR and SRW) doesn't support AVX at all, and
  upstream master doesn't support FMA/AVX2/BMI, which are used by the
  Visual Studio 2013+ x64 C runtime (math functions, memcpy, ...).
* `libudis86/syn-intel.c` comes from upstream vmt/udis86 master
  (commit `56ff6c87c11de0ffa725b14339004820556e343d`), because the fork changed
  the Intel syntax output to MASM style (`xmmword`, `gs:[...]`), which NASM
  doesn't accept.

Only the files needed to build the library are included (no tests, no udcli,
no build system).

## Local modifications

* `libudis86/syn-intel.c` (NASM compatible output):
  * prefixes `xacquire`, `xrelease` and `bnd` from the fork are printed
  * 512-bit operands are printed as `zword`
  * 64-bit absolute memory offsets are printed as `[qword 0x...]`
  * `vmovd` with 64-bit general purpose register is printed as `vmovq`
  * multi-byte `nop` with memory operand gets operand size
  * `prefetchw` / `prefetch` don't get operand size
* `libudis86/extern.h`: removed `register` keyword from the prototype of
  `ud_clear_insn` (C++17 doesn't allow it).
* `libudis86/itab.c`, `libudis86/itab.h`: generated files (see below).

## Regenerating opcode tables

The opcode tables are generated from `docs/x86/optable.xml`
(requires Python 3.6 or newer):

    python3 scripts/ud_itab.py docs/x86/optable.xml libudis86

## Verification

The library (with the modified Intel syntax output) was checked on 64-bit
Windows executables:

* instruction lengths were compared with `llvm-objdump` (about 400000 instructions),
* all unique instructions from `ucrtbase.dll` (about 25000) were assembled with
  NASM and disassembled again, with identical results (except equivalent
  encodings of the same instruction).
