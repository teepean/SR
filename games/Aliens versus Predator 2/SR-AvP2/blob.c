/**
 *
 *  The recompiled code as a separate image (SR_BLOB, tools/mkblob.py): read into fixed memory at its base (below
 *  2 GB), its thunks' slots filled with the C functions they call, its code made executable. Must run before
 *  anything else maps low memory (the low memory allocator, the PE image).
 *
 */

#ifdef SR_BLOB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "sysmem.h"
#include "blob.h"

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

// the image's range and lithtech.exe's image range (0x400000, peimage.c) are taken before anything else can take
// them (macOS: with __PAGEZERO shrunk, libraries and the system allocator may otherwise land there)
#define PE_IMAGE_BASE 0x400000u
#define PE_IMAGE_RESERVE 0x400000u
__attribute__((constructor(101))) static void blob_reserve(void)
{
    sys_reserve_fixed(blob.base, blob.size);
    sys_reserve_fixed(PE_IMAGE_BASE, PE_IMAGE_RESERVE);
}

int blob_load(void)
{
    char path[4096];
    char *dir = SDL_GetBasePath();     // the program's directory (with a trailing separator)
    snprintf(path, sizeof(path), "%s%s", (dir != NULL) ? dir : "", blob.file);
    if (dir != NULL) SDL_free(dir);

    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        eprintf("Error: can't open %s (the recompiled code)\n", path);
        return 1;
    }
    uint8_t *base = (uint8_t *) sys_map_fixed(blob.base, blob.size);
    if (base == NULL)
    {
        eprintf("Error: can't map the recompiled code at 0x%lx (0x%lx bytes)\n", (unsigned long) blob.base, (unsigned long) blob.size);
        sys_dump_low_map();
        fclose(f);
        return 1;
    }
    size_t n = fread(base, 1, blob.file_size, f);
    int extra = fgetc(f);
    fclose(f);
    if ((n != blob.file_size) || (extra != EOF))
    {
        eprintf("Error: %s doesn't belong to this program (%lu bytes, expected %lu)\n", path, (unsigned long) n, (unsigned long) blob.file_size);
        return 1;
    }
    for (unsigned i = 0; i < blob_nimports; i++)
    {
        void (*fn)(void) = blob_imports[i].fn;
        memcpy((void *) blob_imports[i].slot, &fn, sizeof(fn));
    }
    if (!sys_protect_exec(base, blob.text_size))
    {
        eprintf("Error: can't make the recompiled code executable\n");
        return 1;
    }
    if (getenv("AVP2_DEBUG") != NULL) eprintf("blob: %s at 0x%lx-0x%lx, %u imports\n", path, (unsigned long) blob.base, (unsigned long) (blob.base + blob.size), blob_nimports);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
