/**
 *
 *  Texture packs: replaces the game's textures with images from a directory, and dumps them for editing.
 *
 *  A texture is identified by its size and a hash of its decoded pixels (RGBA, after the palette lookup):
 *  <w>x<h>_<hash>.png. SR-I76.cfg:
 *    texture_pack = textures    directory with replacement images (any size; PNG, TGA, BMP, JPG)
 *    texture_dump = 0 | 1       writes every texture the game uses to textures_dump/ (as PNG)
 *  Palette variants of a texture have different hashes. Replacements keep the original's chroma key color
 *  where the game makes pixels transparent (the key is compared with the nearest texel).
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "config.h"
#include "texpack.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef __cplusplus
extern "C" {
#endif

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

extern int winapi_debug;

typedef struct {
    uint64_t key;           // hash (0 = empty slot)
    int w, h;               // replacement size
    uint32_t *pixels;       // replacement (NULL = none)
} texpack_entry;

static int initialized, dump;
static char pack_dir[256];
static texpack_entry *table;
static uint32_t table_size, table_count;
static uint32_t replaced_count, dumped_count;

static uint64_t hash_pixels(int w, int h, const uint32_t *rgba)
{
    // FNV-1a 64 over the size and the pixels
    uint64_t hsh = 0xcbf29ce484222325ull;
    const uint8_t *p = (const uint8_t *)rgba;
    size_t i, n = (size_t)w * h * 4;
    uint32_t dims[2];

    dims[0] = (uint32_t)w;
    dims[1] = (uint32_t)h;
    for (i = 0; i < sizeof(dims); i++) { hsh ^= ((const uint8_t *)dims)[i]; hsh *= 0x100000001b3ull; }
    for (i = 0; i < n; i++) { hsh ^= p[i]; hsh *= 0x100000001b3ull; }
    return (hsh != 0) ? hsh : 1;
}

static texpack_entry *find_slot(uint64_t key)
{
    uint32_t i = (uint32_t)(key ^ (key >> 32)) & (table_size - 1);
    while ((table[i].key != 0) && (table[i].key != key)) i = (i + 1) & (table_size - 1);
    return &table[i];
}

static void grow(void)
{
    texpack_entry *old = table;
    uint32_t i, old_size = table_size;

    table_size = table_size ? table_size * 2 : 1024;
    table = (texpack_entry *)calloc(table_size, sizeof(texpack_entry));
    for (i = 0; i < old_size; i++)
    {
        if (old[i].key != 0) *find_slot(old[i].key) = old[i];
    }
    free(old);
}

static void make_dir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

void texpack_init(void)
{
    const char *s;
    if (initialized) return;
    initialized = 1;
    s = config_get("texture_pack");
    snprintf(pack_dir, sizeof(pack_dir), "%s", ((s != NULL) && (*s != 0)) ? s : "textures");
    dump = config_get_int("texture_dump", 0);
    if (dump) make_dir("textures_dump");
    if (winapi_debug) eprintf("texpack: replacements from %s/%s\n", pack_dir, file_exists(pack_dir) ? "" : " (not found)");
}

const uint32_t *texpack_lookup(int w, int h, const uint32_t *rgba, int *out_w, int *out_h)
{
    uint64_t key;
    texpack_entry *e;
    char name[64], path[512];
    static const char *exts[] = { "png", "tga", "bmp", "jpg" };
    unsigned int k;

    texpack_init();
    key = hash_pixels(w, h, rgba);
    if ((table_count + 1) * 2 > table_size) grow();
    e = find_slot(key);
    if (e->key == key)
    {
        if (e->pixels == NULL) return NULL;
        *out_w = e->w;
        *out_h = e->h;
        return e->pixels;
    }

    // first time this texture is seen
    e->key = key;
    e->pixels = NULL;
    table_count++;
    snprintf(name, sizeof(name), "%dx%d_%016llx", w, h, (unsigned long long)key);

    if (dump)
    {
        snprintf(path, sizeof(path), "textures_dump/%s.png", name);
        if (!file_exists(path) && stbi_write_png(path, w, h, 4, rgba, w * 4)) dumped_count++;
    }

    for (k = 0; k < sizeof(exts) / sizeof(exts[0]); k++)
    {
        int iw, ih, comp;
        unsigned char *data;
        snprintf(path, sizeof(path), "%s/%s.%s", pack_dir, name, exts[k]);
        if (!file_exists(path)) continue;
        data = stbi_load(path, &iw, &ih, &comp, 4);
        if (data == NULL)
        {
            eprintf("texpack: can't load %s: %s\n", path, stbi_failure_reason());
            continue;
        }
        e->pixels = (uint32_t *)malloc((size_t)iw * ih * 4);
        memcpy(e->pixels, data, (size_t)iw * ih * 4);       // RGBA bytes = r in the lowest byte
        stbi_image_free(data);
        e->w = iw;
        e->h = ih;
        replaced_count++;
        if (winapi_debug) eprintf("texpack: %s -> %dx%d replacement\n", name, iw, ih);
        break;
    }
    if (e->pixels == NULL) return NULL;
    *out_w = e->w;
    *out_h = e->h;
    return e->pixels;
}

#ifdef __cplusplus
}
#endif
