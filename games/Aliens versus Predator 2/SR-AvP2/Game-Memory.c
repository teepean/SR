/**
 *
 *  Copyright (C) 2025-2026 Roman Pauer
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 */

#ifdef __cplusplus

#if (defined(__WIN32__) || defined(__WINDOWS__)) && !defined(_WIN32)
#define _WIN32
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#else
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#if !defined(MAP_FIXED_NOREPLACE) && defined(MAP_EXCL)
#define MAP_FIXED_NOREPLACE (MAP_FIXED | MAP_EXCL)
#endif
#include <pthread.h>
#ifdef __linux__
#include <linux/prctl.h>
#include <sys/prctl.h>
#endif
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include "Game-Memory.h"


#define HEAPBLOCK_BASE_SIZE (1024 * 1024)


/* The allocator: TLSF (two-level segregated fit - M. Masmano, I. Ripoll, A. Crespo, J. Real: "TLSF: a new dynamic
 * memory allocator for real-time systems", ECRTS 2004) over pools in the 32-bit window (map_memory_32bit).
 * malloc and free take constant time: the free blocks are in lists by size class (32 classes per power of two),
 * and two bitmaps find a non-empty list big enough. (The allocator before it took the beginning of the largest free
 * block and walked the free list to find the next largest one - ~20% of the CPU time in AvP2, whose engine
 * allocates and frees thousands of small blocks every frame.)
 * Blocks: 8-byte aligned, an 8-byte header (the size and flags) before the data; the previous block's address is
 * kept in the last 8 bytes of a free block (for merging), free blocks keep their list links in their data. */

typedef struct _tlsf_block_t
{
    struct _tlsf_block_t *prev_phys; // the previous block - only while it's free (its last 8 bytes)
    uint64_t size; // data size (multiple of 8) | BLOCK_FREE | BLOCK_PREV_FREE | BLOCK_FIRST
    struct _tlsf_block_t *next_free; // free blocks: the size class's list
    struct _tlsf_block_t *prev_free;
} tlsf_block_t;

#define BLOCK_FREE 1u
#define BLOCK_PREV_FREE 2u
#define BLOCK_FIRST 4u // the first block of its pool
#define BLOCK_FLAGS 7u
#define BLOCK_OVERHEAD 8 // the size field (an allocated block's own memory overhead)
#define BLOCK_START 16 // offset of the data from the block's header (prev_phys + size)
#define BLOCK_SIZE_MIN 24 // free block: next_free, prev_free and the next block's prev_phys
#define POOL_OVERHEAD 16 // the first block's size field and the end sentinel's
// the pool's first page is left unused (zeros): the games read a little before a block sometimes - AvP2's
// CDestructible::UpdateProgressiveDamage reads the character attributes of set -1 (0x270 bytes before the array) for
// every prop with progressive damage. Windows' heap always has headers and other blocks there; a block at the start
// of a pool here had unmapped memory before it (a crash when the array was a new pool's first block)
#define POOL_LEAD 4096

#define TLSF_ALIGN_LOG2 3
#define TLSF_ALIGN (1 << TLSF_ALIGN_LOG2)
#define TLSF_SL_LOG2 5
#define TLSF_SL_COUNT (1 << TLSF_SL_LOG2)
#define TLSF_FL_SHIFT (TLSF_SL_LOG2 + TLSF_ALIGN_LOG2)
#define TLSF_FL_MAX 32
#define TLSF_FL_COUNT (TLSF_FL_MAX - TLSF_FL_SHIFT + 1)
#define TLSF_SMALL_BLOCK (1 << TLSF_FL_SHIFT) // below it, the classes are 8 bytes apart

#define POOL_SIZE (16 * 1024 * 1024) // address space; the pages are mapped when they are used
#define MAX_POOLS 1024

typedef struct
{
    uint8_t *mem;
    uint64_t bytes;
} pool_t;

static uint32_t fl_bitmap;
static uint32_t sl_bitmap[TLSF_FL_COUNT];
static tlsf_block_t *free_lists[TLSF_FL_COUNT][TLSF_SL_COUNT];
static pool_t pools[MAX_POOLS];
static int num_pools;
static uintptr_t pools_low = UINTPTR_MAX, pools_high = 0;

#ifdef _WIN32
static CRITICAL_SECTION mutex;
#else
static pthread_mutex_t mutex;
#endif


#ifdef PTROFS_64BIT
extern "C" void (*ptr_initialize_pointers)(uint64_t pointer_offset);

uint64_t pointer_offset;
unsigned int pointer_reserved_length;
#else
#define pointer_offset 0
#endif


int x86_init_malloc(void)
{
    if (sizeof(void *) > 4)
    {
#ifdef _WIN32
        InitializeCriticalSection(&mutex);
        return 0;
#else
        pthread_mutexattr_t attr;

        if (pthread_mutexattr_init(&attr))
        {
            return 1;
        }

        if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE))
        {
            pthread_mutexattr_destroy(&attr);
            return 2;
        }

        if (pthread_mutex_init(&mutex, &attr))
        {
            pthread_mutexattr_destroy(&attr);
            return 3;
        }

        pthread_mutexattr_destroy(&attr);
        return 0;
#endif
    }
    else
    {
        return 0;
    }
}

void x86_deinit_malloc(void)
{
    if (sizeof(void *) > 4)
    {
#ifdef _WIN32
        DeleteCriticalSection(&mutex);
#else
        pthread_mutex_destroy(&mutex);
#endif
    }
}

#ifdef _WIN32
    #define LOCK_MUTEX EnterCriticalSection(&mutex);
    #define UNLOCK_MUTEX LeaveCriticalSection(&mutex);
#else
    #define LOCK_MUTEX pthread_mutex_lock(&mutex);
    #define UNLOCK_MUTEX pthread_mutex_unlock(&mutex);
#endif

static inline uint64_t block_size(const tlsf_block_t *block)
{
    return block->size & ~(uint64_t)BLOCK_FLAGS;
}

static inline void block_set_size(tlsf_block_t *block, uint64_t size)
{
    block->size = size | (block->size & BLOCK_FLAGS);
}

static inline int block_is_free(const tlsf_block_t *block)
{
    return (block->size & BLOCK_FREE) != 0;
}

static inline int block_is_prev_free(const tlsf_block_t *block)
{
    return (block->size & BLOCK_PREV_FREE) != 0;
}

static inline int block_is_last(const tlsf_block_t *block)
{
    return block_size(block) == 0; // the pool's end sentinel
}

static inline void *block_to_ptr(const tlsf_block_t *block)
{
    return (void *)(BLOCK_START + (uint8_t *)block);
}

static inline tlsf_block_t *ptr_to_block(const void *ptr)
{
    return (tlsf_block_t *)(((uint8_t *)ptr) - BLOCK_START);
}

static inline tlsf_block_t *block_next(const tlsf_block_t *block)
{
    return (tlsf_block_t *)((block_size(block) - BLOCK_OVERHEAD) + (uint8_t *)block_to_ptr(block));
}

// links the next block back to this one (which is free); returns the next block
static inline tlsf_block_t *block_link_next(tlsf_block_t *block)
{
    tlsf_block_t *next;

    next = block_next(block);
    next->prev_phys = block;
    return next;
}

static inline void block_mark_free(tlsf_block_t *block)
{
    tlsf_block_t *next;

    next = block_link_next(block);
    next->size |= BLOCK_PREV_FREE;
    block->size |= BLOCK_FREE;
}

static inline void block_mark_used(tlsf_block_t *block)
{
    tlsf_block_t *next;

    next = block_next(block);
    next->size &= ~(uint64_t)BLOCK_PREV_FREE;
    block->size &= ~(uint64_t)BLOCK_FREE;
}

static inline int fls64(uint64_t value) // the highest set bit (value != 0)
{
    return 63 - __builtin_clzll(value);
}

static inline int ffs32(uint32_t value) // the lowest set bit (value != 0)
{
    return __builtin_ctz(value);
}

// the size class of a block of this size
static void mapping_insert(uint64_t size, int *fli, int *sli)
{
    int fl, sl;

    if (size < TLSF_SMALL_BLOCK)
    {
        fl = 0;
        sl = (int)size / (TLSF_SMALL_BLOCK / TLSF_SL_COUNT);
    }
    else
    {
        fl = fls64(size);
        sl = (int)(size >> (fl - TLSF_SL_LOG2)) ^ (1 << TLSF_SL_LOG2);
        fl -= TLSF_FL_SHIFT - 1;
    }

    *fli = fl;
    *sli = sl;
}

// the first size class whose blocks are all at least this big
static void mapping_search(uint64_t size, int *fli, int *sli)
{
    if (size >= TLSF_SMALL_BLOCK)
    {
        size += (UINT64_C(1) << (fls64(size) - TLSF_SL_LOG2)) - 1;
    }
    mapping_insert(size, fli, sli);
}

static tlsf_block_t *search_suitable_block(int *fli, int *sli)
{
    int fl, sl;
    uint32_t sl_map, fl_map;

    fl = *fli;
    sl = *sli;
    if (fl >= TLSF_FL_COUNT) return NULL;

    sl_map = sl_bitmap[fl] & (~0u << sl);
    if (sl_map == 0)
    {
        // no block in this first level class: the next non-empty bigger one
        fl_map = (fl + 1 < 32) ? (fl_bitmap & (~0u << (fl + 1))) : 0;
        if (fl_map == 0) return NULL;

        fl = ffs32(fl_map);
        *fli = fl;
        sl_map = sl_bitmap[fl];
    }

    sl = ffs32(sl_map);
    *sli = sl;
    return free_lists[fl][sl];
}

static void remove_free_block(tlsf_block_t *block, int fl, int sl)
{
    tlsf_block_t *prev, *next;

    prev = block->prev_free;
    next = block->next_free;
    if (next != NULL) next->prev_free = prev;
    if (prev != NULL)
    {
        prev->next_free = next;
    }
    else
    {
        free_lists[fl][sl] = next;
        if (next == NULL)
        {
            sl_bitmap[fl] &= ~(1u << sl);
            if (sl_bitmap[fl] == 0) fl_bitmap &= ~(1u << fl);
        }
    }
}

static void insert_free_block(tlsf_block_t *block, int fl, int sl)
{
    tlsf_block_t *current;

    current = free_lists[fl][sl];
    block->next_free = current;
    block->prev_free = NULL;
    if (current != NULL) current->prev_free = block;
    free_lists[fl][sl] = block;
    fl_bitmap |= 1u << fl;
    sl_bitmap[fl] |= 1u << sl;
}

static void block_remove(tlsf_block_t *block)
{
    int fl, sl;

    mapping_insert(block_size(block), &fl, &sl);
    remove_free_block(block, fl, sl);
}

static void block_insert(tlsf_block_t *block)
{
    int fl, sl;

    mapping_insert(block_size(block), &fl, &sl);
    insert_free_block(block, fl, sl);
}

static inline int block_can_split(const tlsf_block_t *block, uint64_t size)
{
    return block_size(block) >= sizeof(tlsf_block_t) + size; // the rest: BLOCK_OVERHEAD + at least BLOCK_SIZE_MIN
}

// keeps size bytes in the block; returns the rest, marked free (not in a list; its BLOCK_PREV_FREE is clear)
static tlsf_block_t *block_split(tlsf_block_t *block, uint64_t size)
{
    tlsf_block_t *rest;

    rest = (tlsf_block_t *)((size - BLOCK_OVERHEAD) + (uint8_t *)block_to_ptr(block));
    rest->size = block_size(block) - (size + BLOCK_OVERHEAD);
    block_set_size(block, size);
    block_mark_free(rest);
    return rest;
}

// the block after prev joins it (prev keeps its flags)
static tlsf_block_t *block_absorb(tlsf_block_t *prev, tlsf_block_t *block)
{
    prev->size += block_size(block) + BLOCK_OVERHEAD;
    block_link_next(prev);
    return prev;
}

static tlsf_block_t *block_merge_prev(tlsf_block_t *block)
{
    tlsf_block_t *prev;

    if (block_is_prev_free(block))
    {
        prev = block->prev_phys;
        block_remove(prev);
        block = block_absorb(prev, block);
    }
    return block;
}

static tlsf_block_t *block_merge_next(tlsf_block_t *block)
{
    tlsf_block_t *next;

    next = block_next(block);
    if (block_is_free(next)) // the end sentinel is never free
    {
        block_remove(next);
        block = block_absorb(block, next);
    }
    return block;
}

// a free block that is being allocated: its end goes back to the free lists
static void block_trim_free(tlsf_block_t *block, uint64_t size)
{
    tlsf_block_t *rest;

    if (block_can_split(block, size))
    {
        rest = block_split(block, size);
        block_link_next(block);
        rest->size |= BLOCK_PREV_FREE;
        block_insert(rest);
    }
}

// an allocated block that becomes smaller: its end goes back to the free lists
static void block_trim_used(tlsf_block_t *block, uint64_t size)
{
    tlsf_block_t *rest;

    if (block_can_split(block, size))
    {
        rest = block_split(block, size);
        rest = block_merge_next(rest);
        block_insert(rest);
    }
}

static inline uint64_t adjust_size(unsigned int size)
{
    uint64_t adjusted;

    adjusted = (size + (uint64_t)(TLSF_ALIGN - 1)) & ~(uint64_t)(TLSF_ALIGN - 1);
    return (adjusted < BLOCK_SIZE_MIN) ? BLOCK_SIZE_MIN : adjusted;
}

static inline tlsf_block_t *pool_first_block(const pool_t *pool)
{
    return (tlsf_block_t *)(pool->mem + POOL_LEAD - BLOCK_OVERHEAD);
}

static int pool_is_empty(const pool_t *pool)
{
    const tlsf_block_t *block;

    block = pool_first_block(pool);
    return block_is_free(block) && block_is_last(block_next(block));
}

// a new pool with room for a block of this size; returns 0 if there's no memory
static int add_pool(uint64_t size)
{
    uint64_t bytes;
    uint8_t *mem;
    tlsf_block_t *block, *last;

    if (num_pools >= MAX_POOLS) return 0;

    // the size class search rounds up by up to 1/32
    bytes = size + (size >> 5) + 64 + POOL_LEAD;
    if (bytes <= POOL_SIZE)
    {
        bytes = POOL_SIZE;
    }
    else
    {
        bytes = (bytes + HEAPBLOCK_BASE_SIZE - 1) & ~(uint64_t)(HEAPBLOCK_BASE_SIZE - 1);
        if (bytes >= UINT64_C(0x80000000)) return 0;
    }

    mem = (uint8_t *)map_memory_32bit((unsigned int)bytes, 0);
    if (mem == NULL) return 0;

#ifdef PR_SET_VMA_ANON_NAME
    prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, mem, bytes, "heap32");
#endif

    pools[num_pools].mem = mem;
    pools[num_pools].bytes = bytes;
    num_pools++;
    if ((uintptr_t)mem < pools_low) pools_low = (uintptr_t)mem;
    if ((uintptr_t)mem + bytes > pools_high) pools_high = (uintptr_t)mem + bytes;

    // one free block after the lead (its prev_phys field is in the lead: never used) and the end sentinel (size 0,
    // used)
    block = (tlsf_block_t *)(mem + POOL_LEAD - BLOCK_OVERHEAD);
    block->size = (bytes - POOL_LEAD - POOL_OVERHEAD) | BLOCK_FREE | BLOCK_FIRST;
    last = block_link_next(block);
    last->size = BLOCK_PREV_FREE;
    block_insert(block);
    return 1;
}

// a pool's memory became free (block: all of it, not in the free lists): keeps one empty pool, releases the others
static void pool_emptied(tlsf_block_t *block)
{
    int index, i, empty;

    index = -1;
    empty = 0;
    for (i = 0; i < num_pools; i++)
    {
        if (pool_first_block(&pools[i]) == block) index = i;
        else if (pool_is_empty(&pools[i])) empty++;
    }

    if (index < 0 || empty == 0)
    {
        block_insert(block);
        return;
    }

    unmap_memory_32bit(pools[index].mem, (unsigned int)pools[index].bytes);
    pools[index] = pools[num_pools - 1];
    num_pools--;
}

// a block that x86_free/x86_realloc may use (cheap checks: the runtime only passes its own blocks)
static int valid_used_block(const tlsf_block_t *block)
{
    uintptr_t ptr;

    ptr = (uintptr_t)block_to_ptr(block);
    if ((ptr & (TLSF_ALIGN - 1)) != 0 || ptr < pools_low || ptr >= pools_high) return 0;
    if (block_is_free(block) || block_size(block) < BLOCK_SIZE_MIN || block_size(block) > pools_high - ptr) return 0;
    if (block_is_prev_free(block_next(block))) return 0;
    return 1;
}

static void invalid_block(const void *ptr, const char *function)
{
    static int reported;

    if (reported < 10)
    {
        reported++;
        fprintf(stderr, "%s: invalid block %p\n", function, ptr);
    }
}

void *x86_malloc(unsigned int size)
{
    if (sizeof(void *) > 4)
    {
        tlsf_block_t *block;
        uint64_t adjusted;
        int fl, sl;

        if (size > 0x80000000u - (2 * BLOCK_OVERHEAD + HEAPBLOCK_BASE_SIZE))
        {
            errno = ENOMEM;
            return NULL;
        }

        adjusted = adjust_size(size);

        LOCK_MUTEX

        mapping_search(adjusted, &fl, &sl);
        block = search_suitable_block(&fl, &sl);
        if (block == NULL)
        {
            if (add_pool(adjusted))
            {
                mapping_search(adjusted, &fl, &sl);
                block = search_suitable_block(&fl, &sl);
            }
            if (block == NULL)
            {
                UNLOCK_MUTEX
                errno = ENOMEM;
                return NULL;
            }
        }

        remove_free_block(block, fl, sl);
        block_trim_free(block, adjusted);
        block_mark_used(block);

        UNLOCK_MUTEX
        errno = 0;
        return block_to_ptr(block);
    }
    else
    {
        return malloc(size);
    }
}

// is ptr in this allocator's memory? (the runtime's free with pointer offsets: the system's heap can be inside the
// game's window too)
int x86_owns(const void *ptr)
{
    uintptr_t address;
    int owns, i;

    if (ptr == NULL || sizeof(void *) <= 4) return 0;

    address = (uintptr_t)ptr;
    owns = 0;

    LOCK_MUTEX

    if (address >= pools_low && address < pools_high)
    {
        for (i = 0; i < num_pools; i++)
        {
            if (address - (uintptr_t)pools[i].mem < pools[i].bytes)
            {
                owns = 1;
                break;
            }
        }
    }

    UNLOCK_MUTEX

    return owns;
}

void x86_free(void *ptr)
{
    if (sizeof(void *) > 4)
    {
        tlsf_block_t *block;

        if (ptr == NULL)
        {
            errno = 0;
            return;
        }

        block = ptr_to_block(ptr);

        LOCK_MUTEX

        if (!valid_used_block(block))
        {
            invalid_block(ptr, "x86_free");
            UNLOCK_MUTEX
            errno = EINVAL;
            return;
        }

        block_mark_free(block);
        block = block_merge_prev(block);
        block = block_merge_next(block);

        if ((block->size & BLOCK_FIRST) && block_is_last(block_next(block)))
        {
            pool_emptied(block);
        }
        else
        {
            block_insert(block);
        }

        UNLOCK_MUTEX
        errno = 0;
    }
    else
    {
        free(ptr);
    }
}

void *x86_calloc(unsigned int nmemb, unsigned int size)
{
    if (sizeof(void *) > 4)
    {
        void *ptr;

        if (size > 1)
        {
            if ((0x80000000u - (2 * BLOCK_OVERHEAD + HEAPBLOCK_BASE_SIZE)) / size < nmemb)
            {
                errno = ENOMEM;
                return NULL;
            }
        }

        ptr = x86_malloc(nmemb * size);
        if (ptr != NULL)
        {
            memset(ptr, 0, nmemb * size);
        }

        errno = 0;
        return ptr;
    }
    else
    {
        return calloc(nmemb, size);
    }
}

void *x86_realloc(void *ptr, unsigned int size)
{
    if (sizeof(void *) > 4)
    {
        tlsf_block_t *block, *next;
        uint64_t current, adjusted;
        void *ptr2;

        if (ptr == NULL)
        {
            return x86_malloc(size);
        }

        if (size == 0)
        {
            x86_free(ptr);
            return NULL;
        }

        if (size > 0x80000000u - (2 * BLOCK_OVERHEAD + HEAPBLOCK_BASE_SIZE))
        {
            errno = ENOMEM;
            return NULL;
        }

        adjusted = adjust_size(size);
        block = ptr_to_block(ptr);

        LOCK_MUTEX

        if (!valid_used_block(block))
        {
            invalid_block(ptr, "x86_realloc");
            UNLOCK_MUTEX
            errno = EINVAL;
            return NULL;
        }

        current = block_size(block);
        if (adjusted > current)
        {
            next = block_next(block);
            if (!block_is_free(next) || adjusted > current + block_size(next) + BLOCK_OVERHEAD)
            {
                // a new block
                ptr2 = x86_malloc(size);
                if (ptr2 == NULL)
                {
                    UNLOCK_MUTEX
                    errno = ENOMEM;
                    return NULL;
                }

                memcpy(ptr2, ptr, current);
                x86_free(ptr);
                UNLOCK_MUTEX
                errno = 0;
                return ptr2;
            }

            // grow into the next block, which is free
            block_remove(next);
            block_absorb(block, next);
            block_mark_used(block);
        }

        block_trim_used(block, adjusted);

        UNLOCK_MUTEX
        errno = 0;
        return ptr;
    }
    else
    {
        return realloc(ptr, size);
    }
}


void *map_memory_32bit(unsigned int size, int only_address_space)
{
#ifdef _WIN32
    uint64_t maddr, reg_base, reg_size;
    void *mem;
    SYSTEM_INFO sinfo;
    MEMORY_BASIC_INFORMATION minfo;

    if (size == 0) return NULL;

    GetSystemInfo(&sinfo);

    // round up requested size to the next page boundary
    size = (size + (sinfo.dwPageSize - 1)) & ~(sinfo.dwPageSize - 1);

    // set starting memory address
    maddr = pointer_offset + 1024*1024 + 65536;

    // round up starting memory address to the nearest multiple of the allocation granularity
    maddr = (maddr + (sinfo.dwAllocationGranularity - 1)) & ~(uintptr_t)(sinfo.dwAllocationGranularity - 1);

    // look for unused memory up to 2GB from pointer_offset
    while (maddr < pointer_offset + UINT64_C(0x80000000) && maddr + size <= pointer_offset + UINT64_C(0x80000000))
    {
        if (0 == VirtualQuery((void *)maddr, &minfo, sizeof(minfo))) return NULL;

        if (minfo.State == MEM_FREE)
        {
            reg_base = (((uintptr_t)minfo.BaseAddress) + (sinfo.dwAllocationGranularity - 1)) & ~(uintptr_t)(sinfo.dwAllocationGranularity - 1);
            if (minfo.RegionSize >= reg_base - (uintptr_t)minfo.BaseAddress)
            {
                reg_size = minfo.RegionSize - (reg_base - (uintptr_t)minfo.BaseAddress);

                if (reg_size >= size)
                {
                    mem = VirtualAlloc((void *)reg_base, size, MEM_RESERVE | (only_address_space ? 0 : MEM_COMMIT), only_address_space ? PAGE_NOACCESS : PAGE_READWRITE);
                    if (mem != NULL) return mem;
                }
            }
        }

        maddr = ((minfo.RegionSize + (uintptr_t)minfo.BaseAddress) + (sinfo.dwAllocationGranularity - 1)) & ~(uintptr_t)(sinfo.dwAllocationGranularity - 1);
    }

    return NULL;
#elif defined(__APPLE__)
    void *mem, *start, *start_aligned;
    int prot, flags;
    long page_size;
    mach_port_t task;
    mach_vm_address_t region_address, free_region_start, free_region_end;
    mach_vm_size_t region_size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count;
    mach_port_t object_name;

    if (size == 0) return NULL;

    prot = only_address_space ? PROT_NONE : (PROT_READ | PROT_WRITE);
    flags = MAP_PRIVATE | MAP_ANONYMOUS | (only_address_space ? MAP_NORESERVE : 0);

    // if platform supports MAP_32BIT, then try mapping memory with it
#if defined(MAP_32BIT) && (MAP_32BIT != 0)
#ifdef PTROFS_64BIT
    if (pointer_offset == 0)
#endif
    {
        mem = mmap(0, size, prot, MAP_32BIT | flags, -1, 0);
        if (mem != MAP_FAILED)
        {
            if (((uintptr_t)mem >= UINT64_C(0x80000000)) || (size + (uintptr_t)mem > UINT64_C(0x80000000)))
            {
                // mapped memory is above 2GB
                munmap(mem, size);
            }
            else
            {
                return mem;
            }
        }
    }
#endif

    // look for unused memory up to 2GB from pointer_offset and try mapping memory there
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    task = current_task();

    region_address = pointer_offset;
    count = VM_REGION_BASIC_INFO_COUNT_64;
    if (KERN_SUCCESS != mach_vm_region(task, &region_address, &region_size, VM_REGION_BASIC_INFO_64, (vm_region_info_t) &info, &count, &object_name)) return NULL;

    // first free region (starting at address zero) belongs to segment __PAGEZERO, so don't try using memory here

    if (region_address >= pointer_offset + UINT64_C(0x80000000)) return NULL;

    free_region_start = region_address + region_size;
    while (free_region_start < pointer_offset + UINT64_C(0x80000000))
    {
        region_address = free_region_start;
        count = VM_REGION_BASIC_INFO_COUNT_64;
        if (KERN_SUCCESS != mach_vm_region(task, &region_address, &region_size, VM_REGION_BASIC_INFO_64, (vm_region_info_t) &info, &count, &object_name))
        {
            region_address = free_region_end = pointer_offset + UINT64_C(0x80000000);
            region_size = 0;
        }
        else
        {
            free_region_end = region_address;
            if (free_region_end >= pointer_offset + UINT64_C(0x80000000))
            {
                free_region_end = pointer_offset + UINT64_C(0x80000000);
                region_size = 0;
            }
        }

        if (free_region_end - free_region_start >= size)
        {
            // try using memory at the end of the free region
            start = (void *)((free_region_end - size) & ~(uintptr_t)(page_size - 1));
            if ((size & (HEAPBLOCK_BASE_SIZE - 1)) == 0)
            {
                start_aligned = (void *)(((uintptr_t)start) & ~(uintptr_t)(HEAPBLOCK_BASE_SIZE - 1));
                if ((uintptr_t)start_aligned >= free_region_start + HEAPBLOCK_BASE_SIZE) start = start_aligned;
            }
            if (start != (void *)free_region_start)
            {
                mem = mmap(start, size, prot, MAP_FIXED | flags, -1, 0);
                if (mem == start) return mem;
                if (mem != MAP_FAILED)
                {
                    munmap(start, size);
                    goto error1;
                }
            }

            // try using memory at the start of the free region
            start = (void *)free_region_start;
            if ((size & (HEAPBLOCK_BASE_SIZE - 1)) == 0)
            {
                start_aligned = (void *)(((HEAPBLOCK_BASE_SIZE - 1) + (uintptr_t)start) & ~(uintptr_t)(HEAPBLOCK_BASE_SIZE - 1));
                if (size + (uintptr_t)start_aligned <= free_region_end) start = start_aligned;
            }
            mem = mmap(start, size, prot, MAP_FIXED | flags, -1, 0);
            if (mem == start) return mem;
            if (mem != MAP_FAILED)
            {
                munmap(start, size);
                goto error1;
            }
        }

        free_region_start = region_address + region_size;
    }

    return NULL;

error1:
    fprintf(stderr, "Error: memory mapped at different address\n");
    return NULL;
#else
    void *mem, *start, *start_aligned;
    int num_matches, prot, flags;
    long page_size;
    FILE *f;
    uintmax_t num0, num1, num2;

    if (size == 0) return NULL;

    prot = only_address_space ? PROT_NONE : (PROT_READ | PROT_WRITE);

#if !defined(MAP_NORESERVE) && defined(MAP_GUARD)
    flags = only_address_space ? MAP_GUARD : (MAP_PRIVATE | MAP_ANONYMOUS);
#else
    flags = MAP_PRIVATE | MAP_ANONYMOUS | (only_address_space ? MAP_NORESERVE : 0);
#endif

    // if platform supports MAP_32BIT, then try mapping memory with it
#if defined(MAP_32BIT) && (MAP_32BIT != 0)
#ifdef PTROFS_64BIT
    if (pointer_offset == 0)
#endif
    {
        mem = mmap(0, size, prot, MAP_32BIT | flags, -1, 0);
        if (mem != MAP_FAILED)
        {
            if (((uintptr_t)mem >= UINT64_C(0x80000000)) || (size + (uintptr_t)mem > UINT64_C(0x80000000)))
            {
                // mapped memory is above 2GB
                munmap(mem, size);
            }
            else
            {
                return mem;
            }
        }
    }
#endif

    // look for unused memory up to 2GB from pointer_offset in memory maps and try mapping memory there
    f = fopen("/proc/self/maps", "rb");
    if (f == NULL)
    {
        char mapname[32];
        sprintf(mapname, "/proc/%" PRIuMAX "/map", (uintmax_t)getpid());
        f = fopen(mapname, "rb");
        if (f == NULL) return NULL;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    mem = NULL;
    num0 = (pointer_offset + 1024*1024 + 65536 + (page_size - 1)) & ~(uintptr_t)(page_size - 1);
    while (num0 < pointer_offset + UINT64_C(0x80000000))
    {
        num_matches = fscanf(f, "%" SCNxMAX "%*[ -]%" SCNxMAX " %*[^\n^\r]%*[\n\r]", &num1, &num2);
        if (num_matches == EOF) break;

        if (num_matches < 2)
        {
            num0 = pointer_offset + UINT64_C(0x80000000);
            break;
        }

        // num1-num2 block is used
        // num0-num1 block is not used

        if (num1 >= pointer_offset + UINT64_C(0x80000000)) break;

        // num0-num1 block is below 2GB

        if ((num0 < num1) && (num1 - num0 >= size))
        {
            // try using memory at the end of the block
            start = (void *)((num1 - size) & ~(uintptr_t)(page_size - 1));
            if ((size & (HEAPBLOCK_BASE_SIZE - 1)) == 0)
            {
                start_aligned = (void *)(((uintptr_t)start) & ~(uintptr_t)(HEAPBLOCK_BASE_SIZE - 1));
                if ((uintptr_t)start_aligned >= num0 + HEAPBLOCK_BASE_SIZE) start = start_aligned;
            }
            if (start != (void *)num0)
            {
                mem = mmap(start, size, prot, MAP_FIXED_NOREPLACE | flags, -1, 0);
                if (mem == start) break;
                if (mem != MAP_FAILED)
                {
                    munmap(start, size);
                    goto error2;
                }
            }

            // try using memory at the start of the block
            start = (void *)num0;
            if ((size & (HEAPBLOCK_BASE_SIZE - 1)) == 0)
            {
                start_aligned = (void *)(((HEAPBLOCK_BASE_SIZE - 1) + (uintptr_t)start) & ~(uintptr_t)(HEAPBLOCK_BASE_SIZE - 1));
                if (size + (uintptr_t)start_aligned <= num1) start = start_aligned;
            }
            mem = mmap(start, size, prot, MAP_FIXED_NOREPLACE | flags, -1, 0);
            if (mem == start) break;
            if (mem != MAP_FAILED)
            {
                munmap(start, size);
                goto error2;
            }

            mem = NULL;
        }

        if (num2 > num0)
        {
            num0 = num2;
        }
    }

    fclose(f);

    if (mem != NULL) return mem;

    if ((num0 < pointer_offset + UINT64_C(0x80000000)) && (num0 + size <= pointer_offset + UINT64_C(0x80000000)))
    {
        // try using memory at the end of 2GB
        start = (void *)((pointer_offset + UINT64_C(0x80000000) - size) & ~(uintptr_t)(page_size - 1));
        if ((size & (HEAPBLOCK_BASE_SIZE - 1)) == 0)
        {
            start_aligned = (void *)(((uintptr_t)start) & ~(uintptr_t)(HEAPBLOCK_BASE_SIZE - 1));
            if ((uintptr_t)start_aligned >= num0 + HEAPBLOCK_BASE_SIZE) start = start_aligned;
        }
        if (start != (void *)num0)
        {
            mem = mmap(start, size, prot, MAP_FIXED_NOREPLACE | flags, -1, 0);
            if (mem == start) return mem;
            if (mem != MAP_FAILED)
            {
                munmap(start, size);
                goto error1;
            }
        }

        // try using memory after the end of the last block
        start = (void *)num0;
        if ((size & (HEAPBLOCK_BASE_SIZE - 1)) == 0)
        {
            start_aligned = (void *)(((HEAPBLOCK_BASE_SIZE - 1) + (uintptr_t)start) & ~(uintptr_t)(HEAPBLOCK_BASE_SIZE - 1));
            if (size + (uintptr_t)start_aligned <= pointer_offset + UINT64_C(0x80000000)) start = start_aligned;
        }
        mem = mmap(start, size, prot, MAP_FIXED_NOREPLACE | flags, -1, 0);
        if (mem == start) return mem;
        if (mem != MAP_FAILED)
        {
            munmap(start, size);
            goto error1;
        }
    }

    return NULL;

error2:
    fclose(f);

error1:
    fprintf(stderr, "Error: memory mapped at different address\n");
    return NULL;
#endif
}

void unmap_memory_32bit(void *mem, unsigned int size)
{
    if (mem != NULL && size != 0)
    {
#ifdef _WIN32
        VirtualFree(mem, 0, MEM_RELEASE);
#else
        munmap(mem, size);
#endif
    }
}


#ifdef PTROFS_64BIT

int initialize_pointer_offset(void)
{
#ifdef _WIN32
    HMODULE hModule;
    uint64_t maddr, min_length, free_offset, free_length;
    void *mem;
    SYSTEM_INFO sinfo;
    MEMORY_BASIC_INFORMATION minfo;

    pointer_offset = 0;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ptr_initialize_pointers, &hModule))
    {
        return 1;
    }

    if (hModule == NULL)
    {
        return 2;
    }

    GetSystemInfo(&sinfo);

    min_length = (1024*1024 + 65536 + (sinfo.dwAllocationGranularity - 1)) & ~(sinfo.dwAllocationGranularity - 1);

    if ((uintptr_t)hModule <= 2*min_length)
    {
        pointer_reserved_length = 0;
        if (ptr_initialize_pointers != NULL)
        {
            ptr_initialize_pointers(pointer_offset);
        }
        return 0;
    }

    min_length = (1024*1024 + 65536 + (sinfo.dwPageSize - 1)) & ~(sinfo.dwPageSize - 1);

    maddr = free_offset = 0;
    for (;;)
    {
        if (0 == VirtualQuery((void *)maddr, &minfo, sizeof(minfo))) return 3;

        if (minfo.State == MEM_FREE)
        {
            if ((uintptr_t)minfo.BaseAddress <= (((minfo.RegionSize + (uintptr_t)minfo.BaseAddress) - min_length) & ~(uint64_t)(sinfo.dwAllocationGranularity - 1)))
            {
                free_offset = ((minfo.RegionSize + (uintptr_t)minfo.BaseAddress) - min_length) & ~(uint64_t)(sinfo.dwAllocationGranularity - 1);
                free_length = (minfo.RegionSize + (uintptr_t)minfo.BaseAddress) - free_offset;
            }
        }

        maddr = ((minfo.RegionSize + (uintptr_t)minfo.BaseAddress) + (sinfo.dwAllocationGranularity - 1)) & ~(uint64_t)(sinfo.dwAllocationGranularity - 1);

        if ((uintptr_t)hModule < maddr)
        {
            if (free_offset == 0) break;

            pointer_offset = free_offset;
            pointer_reserved_length = 0;

            // try reserving memory
            mem = VirtualAlloc((void *)free_offset, free_length, MEM_RESERVE, PAGE_NOACCESS);
            if (mem == (void *)free_offset)
            {
                pointer_reserved_length = (unsigned int)free_length;
            }
            else
            {
                if (mem != NULL) VirtualFree(mem, 0, MEM_RELEASE);
            }

            if (ptr_initialize_pointers != NULL)
            {
                ptr_initialize_pointers(pointer_offset);
            }
            return 0;
        }
    }

    return 4;
#elif defined(__APPLE__)
    Dl_info info;
    void *mem;
    long page_size;
    mach_port_t task;
    mach_vm_address_t region_address, maddr, free_offset;
    mach_vm_size_t region_size, min_length;
    vm_region_basic_info_data_64_t rinfo;
    mach_msg_type_number_t count;
    mach_port_t object_name;

    pointer_offset = 0;

    if (!dladdr(&ptr_initialize_pointers, &info))
    {
        return 1;
    }

    if (info.dli_fbase == NULL)
    {
        return 2;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    min_length = (1024*1024 + 65536 + (page_size - 1)) & ~(page_size - 1);

    if ((uintptr_t)info.dli_fbase <= 2*min_length)
    {
        pointer_reserved_length = 0;
        if (ptr_initialize_pointers != NULL)
        {
            ptr_initialize_pointers(pointer_offset);
        }
        return 0;
    }

    task = current_task();

    maddr = free_offset = 0;
    for (;;)
    {
        region_address = maddr;
        count = VM_REGION_BASIC_INFO_COUNT_64;
        if (KERN_SUCCESS != mach_vm_region(task, &region_address, &region_size, VM_REGION_BASIC_INFO_64, (vm_region_info_t) &rinfo, &count, &object_name)) break;

        if (region_address - maddr >= min_length)
        {
            free_offset = region_address - min_length;
        }

        maddr = region_address + region_size;

        if ((uintptr_t)info.dli_fbase < maddr)
        {
            if (free_offset == 0)
            {
                // arm64: only the load address' slide separates the program from __PAGEZERO (the low 4 GB): the
                // window's first part (game addresses below 1 MB, never used: the NULL page) can be in __PAGEZERO
                pointer_offset = ((uintptr_t)info.dli_fbase - min_length) & ~(mach_vm_address_t)(page_size - 1);
                pointer_reserved_length = 0;
                if (ptr_initialize_pointers != NULL)
                {
                    ptr_initialize_pointers(pointer_offset);
                }
                return 0;
            }

            pointer_offset = free_offset;
            pointer_reserved_length = 0;

            // try reserving memory
            mem = mmap((void *)free_offset, min_length, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (mem == (void *)free_offset)
            {
                pointer_reserved_length = (unsigned int)min_length;
            }
            else
            {
                if (mem != MAP_FAILED) munmap(mem, min_length);
            }

            if (ptr_initialize_pointers != NULL)
            {
                ptr_initialize_pointers(pointer_offset);
            }
            return 0;
        }
    }

    return 4;
#else
    Dl_info info;
    void *mem;
    FILE *f;
    long page_size;
    int num_matches, flags;
    uintmax_t num0, num1, num2, min_length, free_offset;

    pointer_offset = 0;

    if (!dladdr(&ptr_initialize_pointers, &info))
    {
        return 1;
    }

    if (info.dli_fbase == NULL)
    {
        return 2;
    }

    f = fopen("/proc/self/maps", "rb");
    if (f == NULL)
    {
        char mapname[32];
        sprintf(mapname, "/proc/%" PRIuMAX "/map", (uintmax_t)getpid());
        f = fopen(mapname, "rb");
        if (f == NULL) return 3;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    min_length = (1024*1024 + 65536 + (page_size - 1)) & ~(page_size - 1);

    if ((uintptr_t)info.dli_fbase <= 2*min_length)
    {
        pointer_reserved_length = 0;
        if (ptr_initialize_pointers != NULL)
        {
            ptr_initialize_pointers(pointer_offset);
        }
        return 0;
    }

    num0 = free_offset = 0;
    for (;;)
    {
        num_matches = fscanf(f, "%" SCNxMAX "%*[ -]%" SCNxMAX " %*[^\n^\r]%*[\n\r]", &num1, &num2);
        if ((num_matches == EOF) || (num_matches < 2)) break;

        // [num1 .. num2) block is used
        // [num0 .. num1) block is not used

        if (num1 - num0 >= min_length)
        {
            free_offset = num1 - min_length;
        }

        if (num2 > num0)
        {
            num0 = num2;
        }

        if ((uintptr_t)info.dli_fbase < num2)
        {
            if (free_offset == 0) break;

            pointer_offset = free_offset;
            pointer_reserved_length = 0;

            // try reserving memory
#if !defined(MAP_NORESERVE) && defined(MAP_GUARD)
            flags = MAP_GUARD;
#else
            flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#endif

            mem = mmap((void *)free_offset, min_length, PROT_NONE, MAP_FIXED_NOREPLACE | flags, -1, 0);
            if (mem == (void *)free_offset)
            {
                pointer_reserved_length = (unsigned int)min_length;
#ifdef PR_SET_VMA_ANON_NAME
                prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, free_offset, min_length, "base32");
#endif
            }
            else
            {
                if (mem != MAP_FAILED) munmap(mem, min_length);
            }

            fclose(f);
            if (ptr_initialize_pointers != NULL)
            {
                ptr_initialize_pointers(pointer_offset);
            }
            return 0;
        }
    }

    fclose(f);
    return 4;
#endif
}

#endif /* PTROFS_64BIT */

#endif /* __cplusplus */

