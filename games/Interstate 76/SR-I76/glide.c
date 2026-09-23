/**
 *
 *  Glide 2.x (glide2x.dll) emulation for the recompiled ZGLIDE.DLL renderer.
 *
 *  - state tracking (combine units, blending, depth, chroma key, fog, culling) -> render_glide_state
 *  - texture memory emulation: grTexDownloadMipMap writes into an emulated TMU memory; grTexSource
 *    looks up (or decodes) a backend texture for (address, format, lod, aspect, palette)
 *  - triangles are culled on the CPU and batched until the state changes
 *  - linear frame buffer: writes go to a 16-bit buffer that is composited over the back/front buffer
 *    on unlock; reads return the buffer contents converted to RGB565
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "winapi.h"
#include "render.h"
#include "display.h"

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#define FXTRUE 1
#define FXFALSE 0

// 2 MB like a Voodoo 1: ZGLIDE's allocator has a bug past 2 MB (it records the wrong start address)
#define TMU_MEMORY (2 * 1024 * 1024)
#define TMU_SLACK (512 * 1024)             // writes past the reported end don't corrupt anything
#define LFB_STRIDE_PIXELS 1024              // like a Voodoo: 2048 bytes per line
#define SNAP_BIAS 786432.0f                 // (3 << 18): ZGLIDE adds it to x/y for the Voodoo's fixed-point vertex snapping

#pragma pack(push, 4)
typedef struct {
    float x, y, z;
    float r, g, b;
    float ooz;
    float a;
    float oow;
    struct { float sow, tow, oow; } tmuvtx[3];
} GrVertex;

typedef struct {
    int32_t smallLod;
    int32_t largeLod;
    int32_t aspectRatio;
    int32_t format;
    void *data;
} GrTexInfo;

typedef struct {
    int32_t size;
    void *lfbPtr;
    uint32_t strideInBytes;
    int32_t writeMode;
    int32_t origin;
} GrLfbInfo_t;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* state                                                               */

// everything grGlideGetState/grGlideSetState save (must fit into GrState = 312 bytes)
typedef struct {
    int32_t rgb_src, rgb_dst, alpha_src, alpha_dst;
    int32_t depth_mode, depth_func, depth_mask;
    int32_t cc[5], ac[5], tc[6];
    uint32_t constant_color;
    int32_t chromakey_enable;
    uint32_t chromakey_value;
    int32_t fog_mode;
    uint32_t fog_color;
    int32_t cull_mode;
    int32_t render_buffer;
    int32_t filter_min, filter_mag, clamp_s, clamp_t, mipmap;
    // texture source
    int32_t tex_valid;
    uint32_t tex_address;
    int32_t tex_evenodd;
    int32_t tex_small_lod, tex_large_lod, tex_aspect, tex_format;
} glide_state;

_Static_assert(sizeof(glide_state) + 4 <= 312, "GrState size");

#define STATE_MAGIC 0x47534C49

static glide_state gs;
static int gl_open;
static int screen_w, screen_h;
static int color_format;            // GR_COLORFORMAT_*
static int origin_lower_left;
static float fog_table[64];

static uint8_t *tmu_mem;
static uint32_t palette[256];       // 0x00RRGGBB
static uint32_t palette_hash;

// current backend texture and its scale
static int cur_texture;
static float cur_s_scale, cur_t_scale;
static int texture_dirty = 1;

// batching
static render_glide_vertex *batch;
static int batch_count, batch_capacity, batch_primitive;
static int state_dirty = 1;
static render_glide_state rstate;

// LFB: lfb = what the game sees, lfb_orig = the buffer contents at lock time
static uint16_t *lfb, *lfb_orig;
static int lfb_locked_write, lfb_locked_buffer, lfb_locked_origin;


/* ------------------------------------------------------------------ */
/* colors                                                              */

// converts a color in the application's color format to ARGB
static uint32_t to_argb(uint32_t c)
{
    uint32_t a, r, g, b;
    switch (color_format)
    {
        case 1: // ABGR
            a = c >> 24; b = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; r = c & 0xFF;
            break;
        case 2: // RGBA
            r = c >> 24; g = (c >> 16) & 0xFF; b = (c >> 8) & 0xFF; a = c & 0xFF;
            break;
        case 3: // BGRA
            b = c >> 24; g = (c >> 16) & 0xFF; r = (c >> 8) & 0xFF; a = c & 0xFF;
            break;
        default: // ARGB
            return c;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}


/* ------------------------------------------------------------------ */
/* textures                                                            */

static int lod_size(int lod) { return 256 >> lod; }

static void tex_dims(int lod, int aspect, int *w, int *h)
{
    int l = lod_size(lod);
    // GR_ASPECT_8x1 = 0 ... 1x1 = 3 ... 1x8 = 6 (width x height)
    int shift = aspect - 3;
    if (shift < 0) { *w = l; *h = l >> -shift; }
    else { *h = l; *w = l >> shift; }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static int tex_bpp(int format)
{
    return (format >= 8) ? 2 : 1;
}

static uint32_t mipmap_size(int small_lod, int large_lod, int aspect, int format)
{
    uint32_t size = 0;
    int lod, w, h;
    for (lod = large_lod; lod <= small_lod; lod++)
    {
        tex_dims(lod, aspect, &w, &h);
        size += w * h * tex_bpp(format);
    }
    return size;
}

typedef struct tex_entry {
    uint32_t address, size;
    int format, large_lod, small_lod, aspect;
    uint32_t palette_hash;
    int texture;
    struct tex_entry *next;
} tex_entry;

static tex_entry *tex_cache;

static void invalidate_textures(uint32_t start, uint32_t end)
{
    tex_entry **p = &tex_cache;
    while (*p != NULL)
    {
        tex_entry *e = *p;
        if ((e->address < end) && (start < e->address + e->size))
        {
            if (e->texture == cur_texture) { cur_texture = 0; texture_dirty = 1; }
            render_glide_texture_destroy(e->texture);
            *p = e->next;
            free(e);
        }
        else
        {
            p = &e->next;
        }
    }
}

static int uses_palette(int format) { return (format == 5) || (format == 14); }

static inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

static void decode(const uint8_t *src, uint32_t *dst, int count, int format)
{
    int i;
    for (i = 0; i < count; i++)
    {
        uint32_t v, r, g, b, a;
        if (tex_bpp(format) == 1) v = src[i]; else v = src[2 * i] | (src[2 * i + 1] << 8);
        switch (format)
        {
            case 0: // RGB_332
                r = ((v >> 5) & 7) * 255 / 7; g = ((v >> 2) & 7) * 255 / 7; b = (v & 3) * 255 / 3;
                dst[i] = rgba(r, g, b, 255);
                break;
            case 2: // ALPHA_8
                dst[i] = rgba(v, v, v, v);
                break;
            case 3: // INTENSITY_8
                dst[i] = rgba(v, v, v, 255);
                break;
            case 4: // ALPHA_INTENSITY_44
                a = (v >> 4) * 17; r = (v & 15) * 17;
                dst[i] = rgba(r, r, r, a);
                break;
            case 5: // P_8
                v = palette[v];
                dst[i] = rgba((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, 255);
                break;
            case 8: // ARGB_8332
                r = ((v >> 5) & 7) * 255 / 7; g = ((v >> 2) & 7) * 255 / 7; b = (v & 3) * 255 / 3;
                dst[i] = rgba(r, g, b, v >> 8);
                break;
            case 10: // RGB_565
                r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31;
                dst[i] = rgba((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
                break;
            case 11: // ARGB_1555
                r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31;
                dst[i] = rgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (v & 0x8000) ? 255 : 0);
                break;
            case 12: // ARGB_4444
                dst[i] = rgba(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (v >> 12) * 17);
                break;
            case 13: // ALPHA_INTENSITY_88
                r = v & 0xFF;
                dst[i] = rgba(r, r, r, v >> 8);
                break;
            case 14: // AP_88
                r = palette[v & 0xFF];
                dst[i] = rgba((r >> 16) & 0xFF, (r >> 8) & 0xFF, r & 0xFF, v >> 8);
                break;
            default:
                dst[i] = rgba(255, 0, 255, 255);
                break;
        }
    }
}

static void select_texture(void)
{
    tex_entry *e;
    int w, h, l;
    uint32_t ph;

    texture_dirty = 0;
    if (!gs.tex_valid)
    {
        cur_texture = 0;
        return;
    }

    ph = uses_palette(gs.tex_format) ? palette_hash : 0;
    for (e = tex_cache; e != NULL; e = e->next)
    {
        if ((e->address == gs.tex_address) && (e->format == gs.tex_format) && (e->large_lod == gs.tex_large_lod) &&
            (e->aspect == gs.tex_aspect) && (e->palette_hash == ph)) break;
    }

    tex_dims(gs.tex_large_lod, gs.tex_aspect, &w, &h);
    if (e == NULL)
    {
        uint32_t *pixels;
        if (gs.tex_address + (uint32_t)(w * h * tex_bpp(gs.tex_format)) > TMU_MEMORY + TMU_SLACK)
        {
            cur_texture = 0;
            return;
        }
        pixels = (uint32_t *)malloc((size_t)w * h * 4);
        decode(tmu_mem + gs.tex_address, pixels, w * h, gs.tex_format);
        e = (tex_entry *)calloc(1, sizeof(tex_entry));
        e->address = gs.tex_address;
        e->size = mipmap_size(gs.tex_small_lod, gs.tex_large_lod, gs.tex_aspect, gs.tex_format);
        e->format = gs.tex_format;
        e->large_lod = gs.tex_large_lod;
        e->small_lod = gs.tex_small_lod;
        e->aspect = gs.tex_aspect;
        e->palette_hash = ph;
        e->texture = render_glide_texture_create(w, h, pixels);
        e->next = tex_cache;
        tex_cache = e;
        free(pixels);
    }

    cur_texture = e->texture;
    // s and t are 0..256 along the longer side
    l = lod_size(gs.tex_large_lod);
    cur_s_scale = (float)l / (256.0f * w);
    cur_t_scale = (float)l / (256.0f * h);
}


/* ------------------------------------------------------------------ */
/* batching                                                            */

static void flush(void)
{
    if (batch_count > 0)
    {
        render_glide_draw(&rstate, batch, batch_count, batch_primitive);
        batch_count = 0;
    }
}

static void state_changed(void)
{
    flush();
    state_dirty = 1;
}

static void update_render_state(void)
{
    if (texture_dirty) select_texture();
    if (!state_dirty) return;
    state_dirty = 0;

    rstate.rgb_src = gs.rgb_src; rstate.rgb_dst = gs.rgb_dst;
    rstate.alpha_src = gs.alpha_src; rstate.alpha_dst = gs.alpha_dst;
    rstate.depth_mode = gs.depth_mode; rstate.depth_func = gs.depth_func; rstate.depth_mask = gs.depth_mask;
    rstate.cc_function = gs.cc[0]; rstate.cc_factor = gs.cc[1]; rstate.cc_local = gs.cc[2]; rstate.cc_other = gs.cc[3]; rstate.cc_invert = gs.cc[4];
    rstate.ac_function = gs.ac[0]; rstate.ac_factor = gs.ac[1]; rstate.ac_local = gs.ac[2]; rstate.ac_other = gs.ac[3]; rstate.ac_invert = gs.ac[4];
    rstate.tc_rgb_function = gs.tc[0]; rstate.tc_rgb_factor = gs.tc[1]; rstate.tc_alpha_function = gs.tc[2]; rstate.tc_alpha_factor = gs.tc[3];
    rstate.tc_rgb_invert = gs.tc[4]; rstate.tc_alpha_invert = gs.tc[5];
    rstate.constant_color = gs.constant_color;
    rstate.chromakey_enable = gs.chromakey_enable;
    rstate.chromakey_value = gs.chromakey_value;
    rstate.fog_mode = gs.fog_mode;
    rstate.fog_color = gs.fog_color;
    memcpy(rstate.fog_table, fog_table, sizeof(fog_table));
    rstate.texture = cur_texture;
    rstate.s_scale = cur_s_scale;
    rstate.t_scale = cur_t_scale;
    rstate.filter_min = gs.filter_min; rstate.filter_mag = gs.filter_mag;
    rstate.clamp_s = gs.clamp_s; rstate.clamp_t = gs.clamp_t;
    rstate.mipmap = gs.mipmap;
}

static void add_vertex(const GrVertex *v)
{
    render_glide_vertex *o;
    if (batch_count >= batch_capacity)
    {
        batch_capacity = batch_capacity ? batch_capacity * 2 : 4096;
        batch = (render_glide_vertex *)realloc(batch, batch_capacity * sizeof(render_glide_vertex));
    }
    o = &batch[batch_count++];
    memcpy(o, v, sizeof(render_glide_vertex));
    // the Voodoo's 12.4 fixed-point vertex registers drop the snap bias's high bits
    if (o->x >= SNAP_BIAS * 0.5f) o->x -= SNAP_BIAS;
    if (o->y >= SNAP_BIAS * 0.5f) o->y -= SNAP_BIAS;
    if (origin_lower_left) o->y = screen_h - o->y;
    // ZGLIDE writes neither ooz nor tmuvtx[0].oow: Glide uses the vertex oow for texturing
    // unless grHints says otherwise (not imported), and depth is W-buffered
    o->tmu_oow = o->oow;
    o->ooz = 0.0f;
}

static void begin_primitive(int primitive)
{
    if (batch_primitive != primitive) flush();
    batch_primitive = primitive;
    if (texture_dirty || state_dirty)
    {
        flush();
        update_render_state();
    }
}


/* ------------------------------------------------------------------ */
/* API: init / hardware                                                */

void CCALL grGlideInit_c(void)
{
    if (tmu_mem == NULL) tmu_mem = (uint8_t *)calloc(1, TMU_MEMORY + TMU_SLACK);
    if (lfb == NULL) lfb = (uint16_t *)malloc(LFB_STRIDE_PIXELS * 1024 * 2);
    if (lfb_orig == NULL) lfb_orig = (uint16_t *)malloc(LFB_STRIDE_PIXELS * 1024 * 2);
    memset(&gs, 0, sizeof(gs));
    gs.rgb_src = 4; gs.rgb_dst = 0; gs.alpha_src = 4; gs.alpha_dst = 0;    // ONE, ZERO
    gs.depth_func = 1;                                                      // LESS
    gs.depth_mask = 1;
    gs.cc[0] = 3; gs.cc[1] = 8; gs.cc[2] = 0; gs.cc[3] = 0;                 // SCALE_OTHER, ONE, ITERATED, ITERATED
    gs.ac[0] = 3; gs.ac[1] = 8; gs.ac[2] = 0; gs.ac[3] = 0;
    gs.tc[0] = 1; gs.tc[1] = 0; gs.tc[2] = 1; gs.tc[3] = 0;                 // LOCAL
    gs.filter_min = gs.filter_mag = 1;
    gs.render_buffer = 1;
    state_dirty = texture_dirty = 1;
    if (winapi_debug) eprintf("grGlideInit\n");
}

void CCALL grGlideShutdown_c(void)
{
    flush();
    if (gl_open) render_glide_close();
    gl_open = 0;
    invalidate_textures(0, 0xFFFFFFFF);
    display_invalidate();
}

uint32_t CCALL grSstQueryBoards_c(int32_t *hwconfig)
{
    if (hwconfig != NULL) hwconfig[0] = 1;
    return FXTRUE;
}

uint32_t CCALL grSstQueryHardware_c(int32_t *hwconfig)
{
    // GrHwConfiguration (148 bytes): num_sst, SSTs[4] { type, VoodooConfig { fbRam, fbiRev, nTexelfx, sliDetect,
    // tmuConfig[GLIDE_NUM_TMU = 2] { tmuRev, tmuRam } } }; ZGLIDE only reads nTexelfx (+0x10)
    if (hwconfig == NULL) return FXFALSE;
    memset(hwconfig, 0, 148);
    hwconfig[0] = 1;
    hwconfig[1] = 0;                    // GR_SSTTYPE_VOODOO
    hwconfig[2] = 4;                    // fbRam (MB)
    hwconfig[3] = 2;                    // fbiRev
    hwconfig[4] = 1;                    // nTexelfx
    hwconfig[5] = 0;                    // sliDetect
    hwconfig[6] = 1;                    // tmuConfig[0].tmuRev
    hwconfig[7] = TMU_MEMORY >> 20;     // tmuConfig[0].tmuRam (MB)
    return FXTRUE;
}

void CCALL grSstSelect_c(int32_t which) {}

static void resolution_size(int res, int *w, int *h)
{
    static const int sizes[][2] = {
        { 320, 200 }, { 320, 240 }, { 400, 256 }, { 512, 384 }, { 640, 200 }, { 640, 350 }, { 640, 400 }, { 640, 480 },
        { 800, 600 }, { 960, 720 }, { 856, 480 }, { 512, 256 }, { 1024, 768 }, { 1280, 1024 }, { 1600, 1200 }, { 400, 300 }
    };
    if ((res >= 0) && (res < 16)) { *w = sizes[res][0]; *h = sizes[res][1]; }
    else { *w = 640; *h = 480; }
}

uint32_t CCALL grSstWinOpen_c(void *hwnd, int32_t resolution, int32_t refresh, int32_t cformat, int32_t origin, int32_t nColBuffers, int32_t nAuxBuffers)
{
    resolution_size(resolution, &screen_w, &screen_h);
    color_format = cformat;
    origin_lower_left = (origin == 1);
    if (winapi_debug) eprintf("grSstWinOpen: %dx%d cformat %d origin %d buffers %d/%d\n", screen_w, screen_h, cformat, origin, nColBuffers, nAuxBuffers);
    if (!display_exists()) return FXFALSE;
    if (!render_glide_open(screen_w, screen_h)) return FXFALSE;
    gl_open = 1;
    state_dirty = texture_dirty = 1;
    return FXTRUE;
}

void CCALL grSstWinClose_c(void)
{
    flush();
    if (gl_open) render_glide_close();
    gl_open = 0;
    display_invalidate();
    if (winapi_debug) eprintf("grSstWinClose\n");
}


/* ------------------------------------------------------------------ */
/* API: state                                                          */

void CCALL grAlphaBlendFunction_c(int32_t rgb_sf, int32_t rgb_df, int32_t alpha_sf, int32_t alpha_df)
{
    state_changed();
    gs.rgb_src = rgb_sf; gs.rgb_dst = rgb_df; gs.alpha_src = alpha_sf; gs.alpha_dst = alpha_df;
}

void CCALL grAlphaCombine_c(int32_t function, int32_t factor, int32_t local, int32_t other, int32_t invert)
{
    state_changed();
    gs.ac[0] = function; gs.ac[1] = factor; gs.ac[2] = local; gs.ac[3] = other; gs.ac[4] = invert;
}

void CCALL grColorCombine_c(int32_t function, int32_t factor, int32_t local, int32_t other, int32_t invert)
{
    state_changed();
    gs.cc[0] = function; gs.cc[1] = factor; gs.cc[2] = local; gs.cc[3] = other; gs.cc[4] = invert;
}

void CCALL grTexCombine_c(int32_t tmu, int32_t rgb_function, int32_t rgb_factor, int32_t alpha_function, int32_t alpha_factor, int32_t rgb_invert, int32_t alpha_invert)
{
    if (tmu != 0) return;
    state_changed();
    gs.tc[0] = rgb_function; gs.tc[1] = rgb_factor; gs.tc[2] = alpha_function; gs.tc[3] = alpha_factor;
    gs.tc[4] = rgb_invert; gs.tc[5] = alpha_invert;
}

void CCALL grChromakeyMode_c(int32_t mode) { state_changed(); gs.chromakey_enable = (mode != 0); }
void CCALL grChromakeyValue_c(uint32_t value) { state_changed(); gs.chromakey_value = to_argb(value) & 0xFFFFFF; }
void CCALL grConstantColorValue_c(uint32_t value) { state_changed(); gs.constant_color = to_argb(value); }
void CCALL grCullMode_c(int32_t mode) { gs.cull_mode = mode; }
void CCALL grDepthBufferFunction_c(int32_t function) { state_changed(); gs.depth_func = function; }
void CCALL grDepthBufferMode_c(int32_t mode) { state_changed(); gs.depth_mode = mode; }
void CCALL grDepthMask_c(int32_t mask) { state_changed(); gs.depth_mask = (mask != 0); }
void CCALL grFogColorValue_c(uint32_t color) { state_changed(); gs.fog_color = to_argb(color); }
void CCALL grFogMode_c(int32_t mode) { state_changed(); gs.fog_mode = mode; }

void CCALL grRenderBuffer_c(int32_t buffer)
{
    flush();
    gs.render_buffer = buffer;
    if ((buffer != 1) && winapi_debug) eprintf("grRenderBuffer: %d (only the back buffer is supported)\n", buffer);
}

uint32_t CCALL grGlideGetState_c(uint32_t *state)
{
    if (state == NULL) return 0;
    state[0] = STATE_MAGIC;
    memcpy(state + 1, &gs, sizeof(gs));
    return 0;
}

uint32_t CCALL grGlideSetState_c(const uint32_t *state)
{
    if ((state == NULL) || (state[0] != STATE_MAGIC)) return 0;
    state_changed();
    memcpy(&gs, state + 1, sizeof(gs));
    texture_dirty = 1;
    return 0;
}


/* ------------------------------------------------------------------ */
/* API: textures                                                       */

uint32_t CCALL grTexMinAddress_c(int32_t tmu) { return 0; }
uint32_t CCALL grTexMaxAddress_c(int32_t tmu) { return TMU_MEMORY; }

uint32_t CCALL grTexCalcMemRequired_c(int32_t small_lod, int32_t large_lod, int32_t aspect, int32_t format)
{
    return (mipmap_size(small_lod, large_lod, aspect, format) + 7) & ~7u;
}

void CCALL grTexDownloadMipMap_c(int32_t tmu, uint32_t start, int32_t even_odd, const GrTexInfo *info)
{
    uint32_t size;
    if ((tmu != 0) || (info == NULL) || (info->data == NULL)) return;
    size = mipmap_size(info->smallLod, info->largeLod, info->aspectRatio, info->format);
    if (start + size > TMU_MEMORY + TMU_SLACK)
    {
        if (winapi_debug) eprintf("grTexDownloadMipMap: 0x%x + %u beyond texture memory\n", start, size);
        return;
    }
    flush();
    invalidate_textures(start, start + size);
    memcpy(tmu_mem + start, info->data, size);
    if (winapi_debug >= 3) eprintf("grTexDownloadMipMap: 0x%06x lod %d-%d aspect %d format %d\n", start, info->largeLod, info->smallLod, info->aspectRatio, info->format);
}

void CCALL grTexDownloadTable_c(int32_t tmu, int32_t type, const uint32_t *data)
{
    uint32_t h;
    int i;
    if ((tmu != 0) || (data == NULL)) return;
    if (type != 2)
    {
        if (winapi_debug) eprintf("grTexDownloadTable: NCC tables are not supported\n");
        return;
    }
    h = 2166136261u;
    for (i = 0; i < 256; i++)
    {
        palette[i] = data[i] & 0xFFFFFF;
        h = (h ^ palette[i]) * 16777619u;
    }
    if (h != palette_hash)
    {
        palette_hash = h;
        if (uses_palette(gs.tex_format))
        {
            flush();
            texture_dirty = 1;
        }
    }
}

void CCALL grTexSource_c(int32_t tmu, uint32_t start, int32_t even_odd, const GrTexInfo *info)
{
    if ((tmu != 0) || (info == NULL)) return;
    if (gs.tex_valid && (gs.tex_address == start) && (gs.tex_format == info->format) && (gs.tex_large_lod == info->largeLod) &&
        (gs.tex_small_lod == info->smallLod) && (gs.tex_aspect == info->aspectRatio)) return;
    flush();
    gs.tex_valid = 1;
    gs.tex_address = start;
    gs.tex_evenodd = even_odd;
    gs.tex_small_lod = info->smallLod;
    gs.tex_large_lod = info->largeLod;
    gs.tex_aspect = info->aspectRatio;
    gs.tex_format = info->format;
    texture_dirty = 1;
    state_dirty = 1;
}

void CCALL grTexClampMode_c(int32_t tmu, int32_t s, int32_t t) { if (tmu == 0) { state_changed(); gs.clamp_s = s; gs.clamp_t = t; } }
void CCALL grTexFilterMode_c(int32_t tmu, int32_t min, int32_t mag) { if (tmu == 0) { state_changed(); gs.filter_min = min; gs.filter_mag = mag; } }
void CCALL grTexMipMapMode_c(int32_t tmu, int32_t mode, int32_t lod_blend) { if (tmu == 0) { state_changed(); gs.mipmap = mode; } }


/* ------------------------------------------------------------------ */
/* API: drawing                                                        */

void CCALL grDrawTriangle_c(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    if (!gl_open) return;
    if (gs.cull_mode != 0)
    {
        float area = (a->x - b->x) * (b->y - c->y) - (b->x - c->x) * (a->y - b->y);
        if ((gs.cull_mode == 1) ? (area < 0.0f) : (area > 0.0f)) return;
    }
    // texture_dirty only needs a new batch if the triangle is textured; keep it simple
    begin_primitive(0);
    add_vertex(a);
    add_vertex(b);
    add_vertex(c);
}

void CCALL grDrawLine_c(const GrVertex *a, const GrVertex *b)
{
    if (!gl_open) return;
    begin_primitive(1);
    add_vertex(a);
    add_vertex(b);
}

void CCALL grDrawPoint_c(const GrVertex *a)
{
    if (!gl_open) return;
    begin_primitive(2);
    add_vertex(a);
}

void CCALL grBufferClear_c(uint32_t color, uint32_t alpha, uint32_t depth)
{
    if (!gl_open) return;
    flush();
    render_glide_clear(to_argb(color), (uint8_t)alpha, (uint16_t)depth, 1, (gs.depth_mode != 0) && gs.depth_mask);
}

void CCALL grBufferSwap_c(int32_t interval)
{
    if (!gl_open) return;
    if (winapi_debug >= 3) eprintf("grBufferSwap %u\n", winapi_get_ticks());
    flush();
    render_glide_swap();
    display_idle();
}


/* ------------------------------------------------------------------ */
/* API: linear frame buffer                                            */

uint32_t CCALL grLfbLock_c(int32_t type, int32_t buffer, int32_t write_mode, int32_t origin, int32_t pixel_pipeline, GrLfbInfo_t *info)
{
    int y;

    if (!gl_open || (info == NULL) || (buffer > 1)) return FXFALSE;
    if (winapi_debug >= 3) eprintf("grLfbLock type %d buffer %d %u\n", type, buffer, winapi_get_ticks());
    flush();

    // the buffer gets the current contents (RGB565) for reads and for writes: on unlock only the pixels
    // the game changed are composited, so the 3D picture keeps its full (scaled) resolution
    render_glide_read_565(buffer, lfb, LFB_STRIDE_PIXELS);
    // the backend returns the picture top-down (GR_ORIGIN_UPPER_LEFT)
    if (origin == 1)
    {
        for (y = 0; y < screen_h / 2; y++)
        {
            uint16_t tmp[LFB_STRIDE_PIXELS];
            memcpy(tmp, lfb + y * LFB_STRIDE_PIXELS, screen_w * 2);
            memcpy(lfb + y * LFB_STRIDE_PIXELS, lfb + (screen_h - 1 - y) * LFB_STRIDE_PIXELS, screen_w * 2);
            memcpy(lfb + (screen_h - 1 - y) * LFB_STRIDE_PIXELS, tmp, screen_w * 2);
        }
    }
    lfb_locked_write = (type & 1);
    if (lfb_locked_write)
    {
        memcpy(lfb_orig, lfb, LFB_STRIDE_PIXELS * screen_h * 2);
        if ((write_mode != 0) && (write_mode != 0xFF) && winapi_debug) eprintf("grLfbLock: write mode %d not supported (565 assumed)\n", write_mode);
        if (pixel_pipeline && winapi_debug) eprintf("grLfbLock: pixel pipeline writes are not supported\n");
    }
    lfb_locked_buffer = buffer;
    lfb_locked_origin = origin;
    info->lfbPtr = lfb;
    info->strideInBytes = LFB_STRIDE_PIXELS * 2;
    info->writeMode = 0;        // GR_LFBWRITEMODE_565
    info->origin = origin;
    return FXTRUE;
}

uint32_t CCALL grLfbUnlock_c(int32_t type, int32_t buffer)
{
    if (!gl_open) return FXFALSE;
    if (((type & 1) != 0) && lfb_locked_write)
    {
        uint32_t *argb = (uint32_t *)malloc((size_t)screen_w * screen_h * 4);
        int x, y, any = 0;
        for (y = 0; y < screen_h; y++)
        {
            int src_y = (lfb_locked_origin == 1) ? (screen_h - 1 - y) : y;
            const uint16_t *row = lfb + src_y * LFB_STRIDE_PIXELS;
            const uint16_t *orig = lfb_orig + src_y * LFB_STRIDE_PIXELS;
            for (x = 0; x < screen_w; x++)
            {
                uint32_t v = row[x];
                if (v == orig[x])
                {
                    argb[y * screen_w + x] = 0;
                }
                else
                {
                    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                    argb[y * screen_w + x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
                    any = 1;
                }
            }
        }
        if (any) render_glide_write_argb(buffer, argb);
        free(argb);
        lfb_locked_write = 0;
    }
    return FXTRUE;
}
