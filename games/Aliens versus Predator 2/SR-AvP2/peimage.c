/**
 *
 *  PE images as data: lithtech.exe at its image base (its headers and resources, which the C runtime and the
 *  resource functions read through GetModuleHandle(NULL)), and resource-only DLLs (ltmsg.dll, ...) loaded with
 *  LoadLibrary. The images are mapped like the Windows loader does (headers, then every section at its RVA),
 *  below 2 GB; their code is never run (the recompiled modules are linked in).
 *
 *  Resources: FindResource / LoadResource / LockResource / SizeofResource / LoadString on such images.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "platform.h"
#include "sysmem.h"
#include "vfs.h"
#include "peimage.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

extern int winapi_debug;

#define MAX_IMAGES 32
static struct { uint8_t *base; uint32_t size; char name[64]; } images[MAX_IMAGES];

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void *map_fixed(uint32_t addr, uint32_t size)
{
#ifdef _WIN32
    return VirtualAlloc((void *)(uintptr_t)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    return sys_map_fixed(addr, size);       // sysmem.c: Linux and macOS
#endif
}

uint8_t *pe_image_load(const char *winpath, int at_image_base)
{
    char host[1024];
    FILE *f;
    long len;
    uint8_t *file, *base = NULL;
    uint32_t pe, size, headers, imagebase, nsec, i;
    const char *slash;

    if (!vfs_resolve(winpath, host, sizeof(host))) snprintf(host, sizeof(host), "%s", winpath);
    f = fopen(host, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc(len);
    if ((file == NULL) || (fread(file, 1, len, f) != (size_t)len)) { fclose(f); free(file); return NULL; }
    fclose(f);

    if ((len < 0x40) || (rd16(file) != 0x5A4D) || ((pe = rd32(file + 0x3c)) + 0xf8 > (uint32_t)len) || (rd32(file + pe) != 0x4550))
    {
        free(file);
        return NULL;
    }
    nsec = rd16(file + pe + 6);
    imagebase = rd32(file + pe + 0x34);
    size = (rd32(file + pe + 0x50) + 0xfff) & ~0xfffu;
    headers = rd32(file + pe + 0x54);

    // at its image base (lithtech.exe at 0x400000); with pointer offsets the program itself is there (the game's
    // window starts just below it): anywhere in the window (the recompiled code doesn't need the address)
#ifndef PTROFS_64BIT
    if (at_image_base) base = (uint8_t *)map_fixed(imagebase, size);
#endif
    if (base == NULL) base = (uint8_t *)sys_map_low(size, -1, 0);
    if (base == NULL) { free(file); return NULL; }
    memset(base, 0, size);
    memcpy(base, file, (headers < (uint32_t)len) ? headers : (uint32_t)len);
    for (i = 0; i < nsec; i++)
    {
        const uint8_t *sh = file + pe + 0xf8 + 40 * i;
        uint32_t va = rd32(sh + 12), rawsize = rd32(sh + 16), rawptr = rd32(sh + 20);
        if (rawptr + rawsize > (uint32_t)len) rawsize = (rawptr < (uint32_t)len) ? (uint32_t)len - rawptr : 0;
        if (va + rawsize > size) rawsize = (va < size) ? size - va : 0;
        memcpy(base + va, file + rawptr, rawsize);
    }
    free(file);

    slash = strrchr(winpath, '\\');
    if (slash == NULL) slash = strrchr(winpath, '/');
    for (i = 0; i < MAX_IMAGES; i++)
    {
        if (images[i].base == NULL)
        {
            images[i].base = base;
            images[i].size = size;
            snprintf(images[i].name, sizeof(images[i].name), "%s", (slash != NULL) ? slash + 1 : winpath);
            break;
        }
    }
    if (winapi_debug) eprintf("PE image %s at %p (%u bytes)\n", winpath, base, size);
    return base;
}

int pe_image_is_loaded(const void *base)
{
    int i;
    for (i = 0; i < MAX_IMAGES; i++) if ((images[i].base != NULL) && (images[i].base == base)) return 1;
    return 0;
}

uint8_t *pe_image_find(const char *name)
{
    int i;
    const char *slash = strrchr(name, '\\');
    if (slash == NULL) slash = strrchr(name, '/');
    if (slash != NULL) name = slash + 1;
    for (i = 0; i < MAX_IMAGES; i++) if ((images[i].base != NULL) && (strcasecmp(images[i].name, name) == 0)) return images[i].base;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* resources                                                           */

// id: MAKEINTRESOURCE (< 0x10000) or a name (case-insensitive, as Windows)
static const uint8_t *res_find_entry(const uint8_t *rsrc, const uint8_t *dir, uintptr_t id)
{
    uint16_t named = rd16(dir + 12), ids = rd16(dir + 14), i;
    const uint8_t *e = dir + 16;
    for (i = 0; i < named + ids; i++, e += 8)
    {
        uint32_t nm = rd32(e);
        if (id < 0x10000)
        {
            if (!(nm & 0x80000000) && (nm == id)) return e;
        }
        else if (nm & 0x80000000)
        {
            const uint8_t *s = rsrc + (nm & 0x7fffffff);
            uint16_t n = rd16(s), k;
            const char *want = (const char *)id;
            if (strlen(want) != n) continue;
            for (k = 0; k < n; k++)
            {
                uint16_t c = rd16(s + 2 + 2 * k);
                if (((c < 128) ? toupper(c) : c) != (uint16_t)toupper((uint8_t)want[k])) break;
            }
            if (k == n) return e;
        }
    }
    return NULL;
}

// returns the IMAGE_RESOURCE_DATA_ENTRY of (type, name, first language)
const uint8_t *pe_resource_find(const uint8_t *base, uintptr_t name, uintptr_t type)
{
    uint32_t pe, rva;
    const uint8_t *rsrc, *e, *dir;

    if (base == NULL) return NULL;
    pe = rd32(base + 0x3c);
    rva = rd32(base + pe + 0x88);
    if ((rva == 0) || (rd32(base + pe + 0x8c) == 0)) return NULL;
    rsrc = base + rva;

    // a name "#123" is the number 123
    if ((name >= 0x10000) && (((const char *)name)[0] == '#')) name = (uintptr_t)atoi((const char *)name + 1);
    if ((type >= 0x10000) && (((const char *)type)[0] == '#')) type = (uintptr_t)atoi((const char *)type + 1);

    e = res_find_entry(rsrc, rsrc, type);
    if ((e == NULL) || !(rd32(e + 4) & 0x80000000)) return NULL;
    dir = rsrc + (rd32(e + 4) & 0x7fffffff);
    e = res_find_entry(rsrc, dir, name);
    if ((e == NULL) || !(rd32(e + 4) & 0x80000000)) return NULL;
    dir = rsrc + (rd32(e + 4) & 0x7fffffff);
    if (rd16(dir + 12) + rd16(dir + 14) == 0) return NULL;
    e = dir + 16;   // the first language
    if (rd32(e + 4) & 0x80000000) return NULL;
    return rsrc + rd32(e + 4);
}

void *pe_resource_data(const uint8_t *base, const uint8_t *entry, uint32_t *size)
{
    if (size != NULL) *size = rd32(entry + 4);
    return (void *)(base + rd32(entry));
}

const uint8_t *pe_image_of(const void *p)
{
    int i;
    for (i = 0; i < MAX_IMAGES; i++)
        if ((images[i].base != NULL) && ((const uint8_t *)p >= images[i].base) && ((const uint8_t *)p < images[i].base + images[i].size)) return images[i].base;
    return NULL;
}

// RT_STRING: block id/16 + 1 holds 16 counted UTF-16 strings
int pe_load_string(const uint8_t *base, uint32_t id, char *buffer, int size)
{
    const uint8_t *entry = pe_resource_find(base, (id >> 4) + 1, 6);
    const uint8_t *p;
    uint32_t i, n, k;

    if ((entry == NULL) || (size <= 0)) { if (size > 0) buffer[0] = 0; return 0; }
    p = (const uint8_t *)pe_resource_data(base, entry, NULL);
    for (i = 0; i < (id & 15); i++) p += 2 + 2 * rd16(p);
    n = rd16(p);
    for (k = 0; (k < n) && ((int)k < size - 1); k++)
    {
        uint16_t c = rd16(p + 2 + 2 * k);
        buffer[k] = (c < 256) ? (char)c : '?';
    }
    buffer[k] = 0;
    return (int)k;
}

EXTERN_C_END
