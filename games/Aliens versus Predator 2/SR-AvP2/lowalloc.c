/**
 *
 *  SR_BLOB: the runtime's malloc & co. on the low memory allocator (lowalloc.h). Pointers below 2 GB are that
 *  allocator's; others came from the system (strdup by a library, SDL_GetBasePath, ...) and go back to it.
 *
 */

#ifdef SR_BLOB

#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "Game-Memory.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOW_LIMIT 0x80000000u

static int is_low(const void *p)
{
    return (uintptr_t) p < LOW_LIMIT;
}

void *rt_malloc(size_t size)
{
    if (size >= LOW_LIMIT) return NULL;
    return x86_malloc((unsigned int) ((size != 0) ? size : 1));
}

void *rt_calloc(size_t n, size_t size)
{
    if ((size != 0) && (n > (LOW_LIMIT - 1) / size)) return NULL;
    return x86_calloc((unsigned int) ((n != 0) ? n : 1), (unsigned int) ((size != 0) ? size : 1));
}

void *rt_realloc(void *p, size_t size)
{
    if (p == NULL) return rt_malloc(size);
    if (!is_low(p)) return realloc(p, size);
    if (size >= LOW_LIMIT) return NULL;
    return x86_realloc(p, (unsigned int) ((size != 0) ? size : 1));
}

void rt_free(void *p)
{
    if (p == NULL) return;
    if (is_low(p)) x86_free(p);
    else free(p);
}

char *rt_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *) rt_malloc(n);
    if (d != NULL) memcpy(d, s, n);
    return d;
}

#ifdef __cplusplus
}
#endif

#endif
