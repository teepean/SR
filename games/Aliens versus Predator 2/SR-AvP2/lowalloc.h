/**
 *
 *  SR_BLOB: the runtime's own C allocations come from low memory (below 2 GB). The program itself is then above
 *  4 GB (position independent; macOS), and its handles, buffers and strings must stay visible to the 32-bit game
 *  code. Force-included (-include) into the runtime's C files (not Game-Memory.c, the allocator, nor the Godot
 *  ones); lowalloc.c has the functions. Memory from system functions (above 2 GB) can still be freed with free().
 *
 */

#if !defined(_LOWALLOC_H_INCLUDED_) && (defined(SR_BLOB) || defined(PTROFS_64BIT))
#define _LOWALLOC_H_INCLUDED_

// the real declarations first (their names must stay intact)
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
#include <cstdlib>
#include <cstring>
extern "C" {
#endif

void *rt_malloc(size_t size);
void *rt_calloc(size_t n, size_t size);
void *rt_realloc(void *p, size_t size);
void rt_free(void *p);
char *rt_strdup(const char *s);

#ifdef __cplusplus
}
#endif

#define malloc(n) rt_malloc(n)
#define calloc(n, s) rt_calloc(n, s)
#define realloc(p, n) rt_realloc(p, n)
#define free(p) rt_free(p)
#define strdup(s) rt_strdup(s)

#endif
