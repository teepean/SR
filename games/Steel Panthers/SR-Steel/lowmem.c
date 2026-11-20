/*
 * Memory the recompiled program can address (below 4 GB).
 *
 * - lowmem_*: page-granular blocks from mmap(MAP_32BIT) (first 2 GB), used for DPMI linear
 *   memory (INT 31h 0501h) and for buffers the runtime hands to the program.
 * - dosmem_*: "conventional memory" below 1 MB, mapped at a fixed address so that
 *   segment * 16 is a valid flat pointer, as under DOS/4GW.
 */
#define _GNU_SOURCE
#include "sp.h"
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>

typedef struct { uint32_t addr, size; } Block;

static Block *blocks;
static int nblocks, cap_blocks;

#define DOS_BASE 0x20000u   /* segment 0x2000 */
#define DOS_END  0xA0000u
#define DOS_PARAS ((DOS_END - DOS_BASE) >> 4)

static uint8_t *dos_area;
/* paragraph allocation map: owner[i] = first paragraph of the block that owns paragraph i, or 0 */
static uint32_t *dos_owner;
static uint32_t *dos_len;

void lowmem_init(void)
{
    void *p = mmap((void *)(uintptr_t)DOS_BASE, DOS_END - DOS_BASE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || (uintptr_t)p != DOS_BASE) {
        sp_log("lowmem: conventional memory at 0x%x not available, DOS memory disabled\n", DOS_BASE);
        if (p != MAP_FAILED) munmap(p, DOS_END - DOS_BASE);
        dos_area = NULL;
    } else {
        dos_area = (uint8_t *)p;
        dos_owner = (uint32_t *)calloc(DOS_PARAS, sizeof(uint32_t));
        dos_len = (uint32_t *)calloc(DOS_PARAS, sizeof(uint32_t));
    }
}

static void add_block(uint32_t addr, uint32_t size)
{
    if (nblocks == cap_blocks) {
        cap_blocks = cap_blocks ? cap_blocks * 2 : 64;
        blocks = (Block *)realloc(blocks, cap_blocks * sizeof(Block));
        if (!blocks) sp_fatal("out of memory\n");
    }
    blocks[nblocks].addr = addr;
    blocks[nblocks].size = size;
    nblocks++;
}

static int find_block(uint32_t addr)
{
    for (int i = 0; i < nblocks; i++)
        if (blocks[i].addr == addr) return i;
    return -1;
}

/* Each block gets a readable zero page before and after it: DOS programs read a little outside
   their blocks (heap headers in front of a pointer, guard bytes) and under DOS/4GW that memory is
   always there. */
#define LOW_GUARD 0x1000u

void *lowmem_alloc(uint32_t size)
{
    size_t sz = ((size_t)size + 0xfff) & ~(size_t)0xfff;
    if (sz == 0) sz = 0x1000;
    uint8_t *base = (uint8_t *)mmap(NULL, sz + 2 * LOW_GUARD, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (base == MAP_FAILED) return NULL;
    if ((uintptr_t)base + sz + 2 * LOW_GUARD > 0xFFFFF000u) { munmap(base, sz + 2 * LOW_GUARD); return NULL; }
    void *p = base + LOW_GUARD;
    add_block(H2G(p), (uint32_t)sz);
    return p;
}

void lowmem_free(void *p)
{
    int i = find_block(H2G(p));
    if (i < 0) return;
    munmap((uint8_t *)p - LOW_GUARD, blocks[i].size + 2 * LOW_GUARD);
    blocks[i] = blocks[--nblocks];
}

void *lowmem_resize(void *p, uint32_t size)
{
    int i = find_block(H2G(p));
    if (i < 0) return NULL;
    uint32_t old = blocks[i].size;
    void *n = lowmem_alloc(size);
    if (!n) return NULL;
    memcpy(n, p, old < size ? old : size);
    lowmem_free(p);
    return n;
}

uint32_t lowmem_largest_free(void)
{
    return 256u * 1024 * 1024;
}

char *lowmem_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)lowmem_alloc((uint32_t)n);
    if (d) memcpy(d, s, n);
    return d;
}

void *dosmem_alloc(uint32_t paragraphs, uint16_t *segment)
{
    if (!dos_area || paragraphs == 0 || paragraphs > DOS_PARAS) return NULL;
    uint32_t run = 0;
    for (uint32_t i = 0; i < DOS_PARAS; i++) {
        if (dos_owner[i]) { run = 0; continue; }
        if (++run == paragraphs) {
            uint32_t first = i + 1 - paragraphs;
            for (uint32_t k = first; k <= i; k++) dos_owner[k] = first + 1;
            dos_len[first] = paragraphs;
            uint8_t *p = dos_area + first * 16;
            memset(p, 0, paragraphs * 16);
            *segment = (uint16_t)((DOS_BASE >> 4) + first);
            return p;
        }
    }
    return NULL;
}

int dosmem_free(uint16_t segment)
{
    if (!dos_area) return 0;
    uint32_t first = (uint32_t)segment - (DOS_BASE >> 4);
    if (first >= DOS_PARAS || dos_owner[first] != first + 1) return 0;
    for (uint32_t k = first; k < first + dos_len[first]; k++) dos_owner[k] = 0;
    dos_len[first] = 0;
    return 1;
}
