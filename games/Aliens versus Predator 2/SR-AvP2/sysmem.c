/**
 *
 *  Host memory mappings visible to the game (below 2 GB): POSIX (mmap: Linux, macOS) and Windows (VirtualAlloc).
 *
 */

#include "sysmem.h"
#include "Game-Memory.h"
#include "platform.h"

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

void *sys_map_fixed(uintptr_t addr, size_t len)
{
    void *p = VirtualAlloc((void *) addr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if ((p != NULL) && (p != (void *) addr))
    {
        VirtualFree(p, 0, MEM_RELEASE);
        return NULL;
    }
    return p;
}

int sys_protect_exec(void *addr, size_t len)
{
    DWORD old;
    return VirtualProtect(addr, len, PAGE_EXECUTE_READ, &old) != 0;
}

int sys_reserve_fixed(uintptr_t addr, size_t len) { return 0; }
void sys_dump_low_map(void) {}

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
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#if defined(__x86_64__) || defined(__aarch64__)
#include <ucontext.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

/* Stray reads (Linux, 64-bit build): the game sometimes reads a little outside its blocks (e.g. object.lto's
 * sub_1018A370 indexes a per-character table with -1 for a character without an id). On Windows the heap is
 * contiguous and such a read returns garbage; here the heap is made of separate mappings and the read can hit an
 * unmapped page. A read (not a write) of an unmapped page below 2 GB gets a page of zeros and continues. */
#if (defined(__x86_64__) || defined(__aarch64__)) && !defined(__SANITIZE_ADDRESS__)
static struct sigaction old_segv;
static volatile int stray_pages;
static uintptr_t fault_page_size = 4096;   // 16 KB on Apple Silicon (sys_install_crash_handler)

// the fault: a write? (x86-64: the page fault's error code; arm64: the data abort's ESR, WnR = bit 6) and the
// instruction's address
#if defined(__x86_64__) && defined(__APPLE__)
#define FAULT_IS_WRITE(uc) ((((uc)->uc_mcontext->__es.__err) & 2) != 0)
#define FAULT_RIP(uc) ((uc)->uc_mcontext->__ss.__rip)
#elif defined(__x86_64__)
#define FAULT_IS_WRITE(uc) ((((uc)->uc_mcontext.gregs[REG_ERR]) & 2) != 0)
#define FAULT_RIP(uc) ((uc)->uc_mcontext.gregs[REG_RIP])
#elif defined(__APPLE__)
#define FAULT_IS_WRITE(uc) ((((uc)->uc_mcontext->__es.__esr) >> 6) & 1)
#define FAULT_RIP(uc) ((uc)->uc_mcontext->__ss.__pc)
#else
// Linux arm64: the ESR is a record in the signal frame's extension area
static int fault_is_write(ucontext_t *uc)
{
    const uint8_t *p = (const uint8_t *) uc->uc_mcontext.__reserved;
    const uint8_t *end = p + sizeof(uc->uc_mcontext.__reserved);
    while (p + 8 <= end)
    {
        uint32_t magic, size;
        memcpy(&magic, p, 4);
        memcpy(&size, p + 4, 4);
        if ((magic == 0) || (size < 8)) break;
        if ((magic == 0x45535201u) && (size >= 16))     // ESR_MAGIC
        {
            uint64_t esr;
            memcpy(&esr, p + 8, 8);
            return (int) ((esr >> 6) & 1);
        }
        p += size;
    }
    return 1;   // unknown: treat as a write (a real crash)
}
#define FAULT_IS_WRITE(uc) fault_is_write(uc)
#define FAULT_RIP(uc) ((uc)->uc_mcontext.pc)
#endif
#if defined(__APPLE__)
// macOS: a page that is reserved but inaccessible raises SIGBUS instead of SIGSEGV
static struct sigaction old_bus;
#endif

static void segv_handler(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = (ucontext_t *) ctx;
    uintptr_t addr = (uintptr_t) info->si_addr;
    int is_write = FAULT_IS_WRITE(uc);
    uintptr_t page_size = fault_page_size;
#ifdef PTROFS_64BIT
    uintptr_t gaddr = addr - (uintptr_t) pointer_offset;   // the game's address (its window above pointer_offset)
#else
    uintptr_t gaddr = addr;
#endif

    if (!is_write && (gaddr >= 0x10000) && (gaddr < 0x80000000u) && (stray_pages < 256))
    {
        void *page = (void *)(addr & ~(page_size - 1));
        void *m = sys_map_fixed((uintptr_t) page, page_size);
        if (m == NULL) m = MAP_FAILED;
        if (m == page)
        {
            static const char hex[] = "0123456789abcdef";
            char msg[112] = "memory: read of unmapped game memory at 0x";
            size_t n = strlen(msg);
            int i;
            for (i = 7; i >= 0; i--) msg[n++] = hex[(gaddr >> (i * 4)) & 15];
            memcpy(msg + n, " (rip 0x", 8);
            n += 8;
            for (i = 15; i >= 0; i--) msg[n++] = hex[((uint64_t) FAULT_RIP(uc) >> (i * 4)) & 15];
            memcpy(msg + n, "): zeros\n", 9);
            n += 9;
            if (write(2, msg, n) < 0) { }
            stray_pages = stray_pages + 1;
            return;     // the instruction runs again
        }
        if (m != MAP_FAILED) munmap(m, page_size);
    }
    // a real crash: the previous handler (the default one: core dump)
    sigaction(SIGSEGV, &old_segv, NULL);
#if defined(__APPLE__)
    sigaction(SIGBUS, &old_bus, NULL);
#endif
    (void) sig;
}

void sys_install_crash_handler(void)
{
    struct sigaction sa;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) fault_page_size = (uintptr_t) ps;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
#if defined(__APPLE__)
    sigaction(SIGBUS, &sa, &old_bus);
#endif
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

// ranges reserved by sys_reserve_fixed (mapped over by sys_map_fixed)
static struct { uintptr_t addr; size_t len; } reserved[8];

static int is_reserved(uintptr_t addr, size_t len)
{
    for (unsigned i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if ((reserved[i].len != 0) && (addr >= reserved[i].addr) && (addr + len <= reserved[i].addr + reserved[i].len)) return 1;
    return 0;
}

int sys_reserve_fixed(uintptr_t addr, size_t len)
{
#ifdef MAP_FIXED_NOREPLACE
    void *p = mmap((void *) addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
#else
    void *p = mmap((void *) addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#endif
    if (p == MAP_FAILED) return 0;
    if (p != (void *) addr)
    {
        munmap(p, len);
        return 0;
    }
    for (unsigned i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if (reserved[i].len == 0)
        {
            reserved[i].addr = addr;
            reserved[i].len = len;
            return 1;
        }
    return 1;
}

void sys_dump_low_map(void)
{
#if defined(__linux__)
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    if (f == NULL) return;
    fprintf(stderr, "memory below 4 GB:\n");
    while (fgets(line, sizeof(line), f) != NULL)
        if (strtoull(line, NULL, 16) < 0x100000000ull) fprintf(stderr, "  %s", line);
    fclose(f);
#elif defined(__APPLE__)
    mach_vm_address_t a = 0;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count;
    mach_port_t object_name;
    fprintf(stderr, "memory below 4 GB:\n");
    for (;;)
    {
        count = VM_REGION_BASIC_INFO_COUNT_64;
        if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t) &info, &count, &object_name) != KERN_SUCCESS) break;
        if (a >= 0x100000000ull) break;
        fprintf(stderr, "  %08llx-%08llx prot %d\n", (unsigned long long) a, (unsigned long long) (a + size), info.protection);
        a += size;
    }
#endif
}

void *sys_map_fixed(uintptr_t addr, size_t len)
{
    if (is_reserved(addr, len))
    {
        // ours: map over the reservation
        void *r = mmap((void *) addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        return (r == (void *) addr) ? r : NULL;
    }
#ifdef MAP_FIXED_NOREPLACE
    void *p = mmap((void *) addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#else
    // macOS: no MAP_FIXED_NOREPLACE; a hint is taken when the range is free
    void *p = mmap((void *) addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (p == MAP_FAILED) return NULL;
    if (p != (void *) addr)
    {
        munmap(p, len);
        return NULL;
    }
    return p;
}

int sys_protect_exec(void *addr, size_t len)
{
    return mprotect(addr, len, PROT_READ | PROT_EXEC) == 0;
}

#ifdef __cplusplus
}
#endif

#endif
