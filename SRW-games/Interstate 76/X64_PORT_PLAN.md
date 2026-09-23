# Interstate '76: 64-bit (x86-64 Linux) port plan

Based on a survey of SRW's OUT_X64 mode and Septerra Core's x64 runtime (2026-09-23).

## How OUT_X64 code works
- SRW output mode is compile-time: `OUTPUT_TYPE OUT_X64` in `SRW/SR_defs.h` (build a separate SRW64 binary).
- x86 registers = low halves of rax..rdi; **esp -> r11d** (separate emulated x86 stack in low memory, 1 MB per
  thread from x86_malloc); r8-r10 scratch; addresses stay 32-bit (a32 prefixes, `DEFAULT ABS`).
- `CALL`/`RET`/`PUSH32`/`POP32` macros (x64/asm_call.inc, asm_pushx.inc): 32-bit return addresses on the x86 stack.
- All recompiled code/data and C globals referenced from asm must be **below 2 GB** (R_X86_64_32S): non-PIE
  executable linked at 0x10000000 (`-no-pie -Wl,-Ttext-segment,0x10000000`).
- asm2c stubs: same `Call_Asm_StackN func, n` interface (x64/asm-calls.inc, SysV ABI); the C function gets
  zero-extended 32-bit args; only eax is returned (pointer returns must be < 4 GB).
- C -> game calls need trampolines (game code expects r11d = esp, returns with `jmp r8`): Septerra
  x64/functions-asm.asm (c_WinMain_, c_RunWndProc_c2asm...), stack from x86_initialize_stack() (x64/asm-cpu.c).
- fs:[x] -> x86_read_fs_dword/x86_write_fs_dword (x64/asm_fs_mem.asm); x87 FPU native.
- instruction_replacements.sci text must be x64-style (`CALL i76_frame_tick`, r11d for esp).

## SRW changes needed for I76
Unknown instructions in the x64 translator (SRW/SR_full_x64_instr.c): i76.exe fpatan, fprem1, fscale, f2xm1,
fsincos; i76shell setae, jecxz, repe cmpsw; zglide fclex. Add them (FPU pass-through group, setcc group,
jecxz pass-through, cmpsw/cmpsd/scasw/scasd like cmpsb with `a32`).

## Runtime checklist (SR-I76)
1. Glue: gen_imports.py target dir x86|x64 (x64inc.inc, elf64 sections); x64/asm-calls.inc from Septerra plus
   Call_Asm_Stack10/11, VariableStack1, double (xmm0 -> st0) and uint64 (rax -> edx:eax) returns; x64 raw-asm
   (_ftol, EH traps); copy Septerra x64/*.inc, asm_fs_mem.asm, asm-cpu.c, x64_stack.h.
2. C -> game calls via a generic trampoline `c_call_asm_n(_stack*, func, nargs, args)`: WinMain, static
   constructors (4-byte xc_a..xc_z entries!), DLL constructors, WndProc, EnumDisplayModes callback, qsort/bsearch
   comparators. Game function pointers become uint32_t.
3. Game-visible memory below 2 GB (Septerra Game-Memory.c: x86_malloc/map_memory_32bit): HeapAlloc & co,
   VirtualAlloc, all handles (windows, GDI objects, files, find handles, registry keys, ms_FILE, Smack objects),
   buffers (DirectDraw surfaces, LFB, DirectSound buffers, DIB sections, SmackBuf), CREATESTRUCT, EnumDisplayModes
   desc, errno (static copy), anything from host libc handed to the game.
4. Struct layouts: pointer fields -> PTR32 (ptr32.h): GrTexInfo.data, GrLfbInfo_t.lfbPtr, DSBUFFERDESC.lpwfxFormat,
   COM lpVtbl, all T** out-params (ddraw/dsound/QueryInterface/CreateDIBSection), ms_FILE (_ptr/_base/_tmpfname;
   keep 32 bytes, host FILE* in a side table), _pctype, rad_smackbuf.Buffer, WNDCLASSA, MSG.hwnd.
5. CRT: vfscanf/vsscanf va_list tricks -> Septerra sscanf2_c approach.
6. Compile as C++ (`-x c++`, like Septerra) so PTR32 is a checked 4-byte type; extern "C" for everything asm
   references; fix ~24 C++ errors (file_handle name clash, _Static_assert, casts).
7. Build: SConstruct device pc64-linux (-m64 -x c++ -fno-PIE, link at 0x10000000, non-PIE); x64/SConscript
   (-felf64 -ix64/); SRW-games x64/*.sci dirs; gen_all.sh variant with SRW64, nasm -felf64, repair loop.
8. Keep one source tree: PTR32 is plain T* in the 32-bit C build.
