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

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#if defined(__x86_64__)
#include <ucontext.h>
#endif

/* Stray reads (Linux, 64-bit build): the game sometimes reads a little outside its blocks (e.g. object.lto's
 * sub_1018A370 indexes a per-character table with -1 for a character without an id). On Windows the heap is
 * contiguous and such a read returns garbage; here the heap is made of separate mappings and the read can hit an
 * unmapped page. A read (not a write) of an unmapped page below 2 GB gets a page of zeros and continues. */
#if defined(__x86_64__) && !defined(__SANITIZE_ADDRESS__)
static struct sigaction old_segv;
static volatile int stray_pages;

static void segv_handler(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = (ucontext_t *) ctx;
    uintptr_t addr = (uintptr_t) info->si_addr;
    int is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;

    if (!is_write && (addr >= 0x10000) && (addr < 0x80000000u) && (stray_pages < 256))
    {
        void *page = (void *)(addr & ~(uintptr_t) 4095);
        void *m = mmap(page, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m == page)
        {
            static const char hex[] = "0123456789abcdef";
            char msg[112] = "memory: read of unmapped game memory at 0x";
            size_t n = strlen(msg);
            int i;
            for (i = 7; i >= 0; i--) msg[n++] = hex[(addr >> (i * 4)) & 15];
            memcpy(msg + n, " (rip 0x", 8);
            n += 8;
            for (i = 15; i >= 0; i--) msg[n++] = hex[((uint64_t) uc->uc_mcontext.gregs[REG_RIP] >> (i * 4)) & 15];
            memcpy(msg + n, "): zeros\n", 9);
            n += 9;
            if (write(2, msg, n) < 0) { }
            stray_pages = stray_pages + 1;
            return;     // the instruction runs again
        }
        if (m != MAP_FAILED) munmap(m, 4096);
    }
    // a real crash: the previous handler (the default one: core dump)
    sigaction(SIGSEGV, &old_segv, NULL);
    (void) sig;
}

void sys_install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
}
#else
void sys_install_crash_handler(void)
{
}
#endif

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
