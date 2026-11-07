/**
 *
 *  Host memory mappings visible to the game (below 2 GB): Linux (mmap) and Windows (VirtualAlloc).
 *
 */

#include "sysmem.h"
#include "Game-Memory.h"

#ifdef _WIN32

#include <windows.h>
#include <io.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void *sys_map_low(size_t len, int fd, int64_t offset)
{
    uint8_t *base;

    base = (uint8_t *) map_memory_32bit((unsigned int) len, 0);
    if (base == NULL) return NULL;
    if (fd >= 0)
    {
        // no copy-on-write file mappings at fixed addresses: read the data
        int64_t pos = _lseeki64(fd, 0, SEEK_CUR);
        size_t done = 0;
        _lseeki64(fd, offset, SEEK_SET);
        while (done < len)
        {
            int n = _read(fd, base + done, (unsigned int)((len - done > 0x40000000) ? 0x40000000 : (len - done)));
            if (n <= 0) break;
            done += n;
        }
        _lseeki64(fd, pos, SEEK_SET);
    }
    return base;
}

void sys_unmap(void *addr, size_t len)
{
    unmap_memory_32bit(addr, (unsigned int) len);
}

void sys_protect_none(void *addr, size_t len)
{
    DWORD old;
    VirtualProtect(addr, len, PAGE_NOACCESS, &old);
}

static LONG CALLBACK crash_handler(PEXCEPTION_POINTERS info)
{
    PCONTEXT c = info->ContextRecord;
    uint64_t *sp;
    int i;

    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        info->ExceptionRecord->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        info->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;

    fprintf(stderr, "crash: exception 0x%08lx at %p (address %p)\n", info->ExceptionRecord->ExceptionCode,
            info->ExceptionRecord->ExceptionAddress, (info->ExceptionRecord->NumberParameters >= 2) ? (void *)info->ExceptionRecord->ExceptionInformation[1] : NULL);
    fprintf(stderr, "rax %016llx rbx %016llx rcx %016llx rdx %016llx\n", (unsigned long long)c->Rax, (unsigned long long)c->Rbx, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
    fprintf(stderr, "rsi %016llx rdi %016llx rbp %016llx rsp %016llx\n", (unsigned long long)c->Rsi, (unsigned long long)c->Rdi, (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
    fprintf(stderr, "r8  %016llx r9  %016llx r10 %016llx r11 %016llx\n", (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->R10, (unsigned long long)c->R11);
    fprintf(stderr, "r12 %016llx r13 %016llx r14 %016llx r15 %016llx\n", (unsigned long long)c->R12, (unsigned long long)c->R13, (unsigned long long)c->R14, (unsigned long long)c->R15);
    sp = (uint64_t *) c->Rsp;
    fprintf(stderr, "stack:");
    for (i = 0; i < 48; i++) fprintf(stderr, "%s%016llx", (i % 4) ? " " : "\n  ", (unsigned long long)sp[i]);
    fprintf(stderr, "\nemulated x86 stack (r11d):");
    if ((c->R11 & 0xFFFFFFFF) > 0x10000)
    {
        uint32_t *esp = (uint32_t *)(uintptr_t)(c->R11 & 0xFFFFFFFF);
        for (i = 0; i < 32; i++) fprintf(stderr, "%s%08x", (i % 8) ? " " : "\n  ", esp[i]);
    }
    // r10 = _stack * in the asm2c stubs' epilogue
    if ((c->R10 > 0x10000) && (c->R10 < 0x80000000) && !IsBadReadPtr((void *)c->R10, 4))
    {
        uint32_t esp = *(uint32_t *)c->R10;
        fprintf(stderr, "\nstack->esp %08x:", esp);
        if ((esp > 0x10000) && !IsBadReadPtr((void *)(uintptr_t)(esp - 64), 192))
        {
            uint32_t *p = (uint32_t *)(uintptr_t)(esp - 64);
            for (i = 0; i < 48; i++) fprintf(stderr, "%s%08x", (i % 8) ? " " : "\n  ", p[i]);
        }
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void sys_install_crash_handler(void)
{
    AddVectoredExceptionHandler(1, crash_handler);
}

#ifdef __cplusplus
}
#endif

#else

void sys_install_crash_handler(void)
{
}

#include <errno.h>
#include <sys/mman.h>

#ifdef __cplusplus
extern "C" {
#endif

void *sys_map_low(size_t len, int fd, int64_t offset)
{
    void *addr;
#ifdef __cplusplus
    // 64-bit build: reserve low address space, map over it
    void *base = map_memory_32bit((unsigned int) len, 1);
    if (base == NULL) return NULL;
    if (fd >= 0) addr = mmap(base, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, (off_t) offset);
    else addr = mmap(base, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return (addr == MAP_FAILED) ? NULL : addr;
#else
    // 32-bit build: the kernel places mappings high (0xE0000000...), but game code assumes Win32 addresses
    // below 2 GB (e.g. sub_469B00 computes NULL - pointer as a signed length) - search the low range
    static uintptr_t hint = 0x20000000;
    uintptr_t start = hint;
    size_t alen = (len + 4095) & ~(size_t)4095;
    int flags = ((fd >= 0) ? MAP_PRIVATE : (MAP_PRIVATE | MAP_ANONYMOUS)) | MAP_FIXED_NOREPLACE;
    for (;;)
    {
        if (hint + alen > 0x7FFF0000u) hint = 0x10000000;
        addr = mmap((void *) hint, len, PROT_READ | PROT_WRITE, flags, fd, (fd >= 0) ? (off_t) offset : 0);
        if (addr != MAP_FAILED)
        {
            hint += alen;
            return addr;
        }
        if (errno != EEXIST) return NULL;
        hint += alen;
        if ((hint <= start) && (hint + alen > start)) return NULL;    // wrapped around: no room
    }
#endif
}

void sys_unmap(void *addr, size_t len)
{
    munmap(addr, len);
}

void sys_protect_none(void *addr, size_t len)
{
    mprotect(addr, len, PROT_NONE);
}

#ifdef __cplusplus
}
#endif

#endif
