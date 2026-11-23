# SR - instructions for agents

This repository contains static recompilers of DOS and Windows game executables and game specific code.
The generated assembler versions of the original executables (and the original executables) are not part of the repository -- never commit them.

| Directory | Contents |
|---|---|
| `SR` | static recompiler for 32-bit DOS executables (output: x86, arm, llasm, x64) |
| `SRW` | static recompiler for 32-bit Windows executables (output: x86, llasm, x64) |
| `SRW64` | static recompiler for 64-bit Windows executables and statically linked 64-bit Linux executables (output: x64) -- see [SRW64/AGENTS.md](SRW64/AGENTS.md) |
| `SR-games`, `SRW-games`, `SRW64-games` | information about the original executables (SCI files, build scripts) |
| `games` | game specific code (replacement of DOS/Windows functions, ...) |
| `llasm` | converter of llasm files to LLVM IR |
| `SR64-loader` | Linux loader which keeps all memory below 2 GB (for programs which store pointers in 32 bits) |

Choosing the recompiler: 32-bit DOS executable (LE format, DOS extender) -> `SR`, 32-bit Windows executable (PE32) -> `SRW`, 64-bit Windows executable (PE32+, x86-64) or statically linked 64-bit Linux executable (ELF64, not PIE) -> `SRW64`.

Each recompiler is built using scons in its directory (the result is `SR.exe`, `SRW.exe`, `SRW64.exe`). The output type of SR and SRW is selected by `OUTPUT_TYPE` in `SR_defs.h`.
