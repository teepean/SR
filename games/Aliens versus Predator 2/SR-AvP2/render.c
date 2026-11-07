/**
 *
 *  The renderer: replaces d3d.ren (LithTech "Talon" render DLL, DirectDraw 7 / Direct3D 7).
 *
 *  The engine loads "d3d.ren", calls its RenderDLLSetup export with its RenderStruct, and from then on draws
 *  through the function table RenderDLLSetup filled in (layout: AVP2-Reconstructed include/renderstruct.h;
 *  the slots RenderDLLSetup sets: src/d3dren/sys/d3d/common_init.cpp). The engine's own functions are the
 *  first slots of the same struct (0x00-0x38).
 *
 *  This stage: the 2D layer. Surfaces are 32-bit (the screen format GetScreenFormat reports), the screen is a
 *  32-bit framebuffer composed on the CPU (blits, optimized 2D with its blend modes) and presented by
 *  SwapBuffers through display.c. The 3D world is Godot's (render3d.cpp); the 2D frame is an overlay over it:
 *  its pixels are premultiplied ARGB, RenderScene makes the scene's rectangle transparent and what's drawn after
 *  it (the HUD) covers the 3D picture (alpha) or adds to it (additive blending keeps the alpha).
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include "platform.h"
#include "ptr32.h"
#include "winapi.h"
#include "display.h"
#include "game.h"
#include "config.h"
#include "host.h"
#include "render3d.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#define LTRENDER_VERSION 3421
#define RENDER_OK 0
#define RENDER_ERROR 1

// asm2c stubs of the slot functions (gen_imports.py, imports.spec "render")
#define STUB(name) extern char ren_##name##_asm2c[];
STUB(Init) STUB(Term) STUB(BindTexture) STUB(UnbindTexture) STUB(RebindLightmaps) STUB(CreateContext) STUB(DeleteContext)
STUB(Clear) STUB(Start3D) STUB(End3D) STUB(IsIn3D) STUB(StartOptimized2D) STUB(EndOptimized2D) STUB(IsInOptimized2D)
STUB(SetOptimized2DBlend) STUB(GetOptimized2DBlend) STUB(SetOptimized2DColor) STUB(GetOptimized2DColor) STUB(RenderScene)
STUB(RenderCommand) STUB(GetHook) STUB(SwapBuffers) STUB(GetInfoFlags) STUB(GetScreenFormat) STUB(CreateSurface)
STUB(DeleteSurface) STUB(GetSurfaceInfo) STUB(LockSurface) STUB(UnlockSurface) STUB(OptimizeSurface) STUB(UnoptimizeSurface)
STUB(LockScreen) STUB(UnlockScreen) STUB(BlitToScreen) STUB(WarpToScreen) STUB(MakeScreenShot) STUB(ReadConsoleVariables)
STUB(BlitFromScreen)
#undef STUB

/* ------------------------------------------------------------------ */
/* the engine's structures (32-bit)                                    */

// RenderStruct offsets
#define RS_WIDTH 0x3c
#define RS_HEIGHT 0x40
#define RS_INITTED 0x44

typedef struct { int32_t left, top, right, bottom; } LTRect32;
typedef struct { float x, y, z; } LTVector32;

#pragma pack(push, 1)
typedef struct {
    uint32_t hBuffer;
    uint32_t options;           // BLIT_TRANSPARENT 1
    uint32_t transparent;       // in the screen format
    PTR32(LTRect32) pSrcRect;
    PTR32(LTRect32) pDestRect;
    float alpha;
    uint32_t pWarpPts;
    int32_t nWarpPts;
} BlitRequest32;

typedef struct {
    uint32_t bHardware;
    char renderDLL[256];
    char internalName[128];
    char description[128];
    uint32_t width, height, bitDepth;
    PTR32(void) pNext;
} RMode32;

typedef struct {
    int32_t rendererVersion;
    RMode32 mode;
    uint32_t hWnd;
} RenderStructInit32;
#pragma pack(pop)

static uint8_t *rs;     // the engine's RenderStruct

static inline void rs_set(uint32_t ofs, const void *fn) { *(uint32_t *)(rs + ofs) = (uint32_t)(uintptr_t)fn; }

/* ------------------------------------------------------------------ */
/* surfaces and the screen                                             */

#define SURFACE_MAGIC 0x52465553    // "SURF"

typedef struct {
    uint32_t magic;
    int32_t width, height, pitch;       // pitch in bytes
    uint32_t *pixels;                   // XRGB8888 (game memory: the engine writes to it after LockSurface)
    int optimized;
    uint32_t transparent;               // optimized surfaces: the transparent color
} surface;

static uint32_t *screen;                // the composed frame (display_pixels)
static int screen_w, screen_h;
static int in_3d, in_optimized2d;
static uint32_t blend_mode, blend_color = 0x00FFFFFF;
static uint32_t frames;

static surface *get_surface(uint32_t h)
{
    surface *s = (surface *)(uintptr_t)h;
    if ((s == NULL) || (s->magic != SURFACE_MAGIC)) return NULL;
    return s;
}

static void clip_rect(LTRect32 *r, int w, int h)
{
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > w) r->right = w;
    if (r->bottom > h) r->bottom = h;
}

/* ------------------------------------------------------------------ */
/* exports                                                             */

void CCALL RenderDLLSetup_c(uint8_t *pStruct)
{
    rs = pStruct;
    r3d_set_render_struct(pStruct);
    rs_set(0x70, ren_Init_asm2c);
    rs_set(0x74, ren_Term_asm2c);
    rs_set(0x78, ren_BindTexture_asm2c);
    rs_set(0x7c, ren_UnbindTexture_asm2c);
    rs_set(0x80, ren_RebindLightmaps_asm2c);
    rs_set(0x84, ren_CreateContext_asm2c);
    rs_set(0x88, ren_DeleteContext_asm2c);
    rs_set(0x8c, ren_Clear_asm2c);
    rs_set(0x90, ren_Start3D_asm2c);
    rs_set(0x94, ren_End3D_asm2c);
    rs_set(0x98, ren_IsIn3D_asm2c);
    rs_set(0x9c, ren_StartOptimized2D_asm2c);
    rs_set(0xa0, ren_EndOptimized2D_asm2c);
    rs_set(0xa4, ren_IsInOptimized2D_asm2c);
    rs_set(0xa8, ren_SetOptimized2DBlend_asm2c);
    rs_set(0xac, ren_GetOptimized2DBlend_asm2c);
    rs_set(0xb0, ren_SetOptimized2DColor_asm2c);
    rs_set(0xb4, ren_GetOptimized2DColor_asm2c);
    rs_set(0xb8, ren_RenderScene_asm2c);
    rs_set(0xbc, ren_RenderCommand_asm2c);
    rs_set(0xc0, ren_GetHook_asm2c);
    rs_set(0xc4, ren_SwapBuffers_asm2c);
    rs_set(0xc8, ren_GetInfoFlags_asm2c);
    rs_set(0xcc, ren_GetScreenFormat_asm2c);
    rs_set(0xd0, ren_CreateSurface_asm2c);
    rs_set(0xd4, ren_DeleteSurface_asm2c);
    rs_set(0xd8, ren_GetSurfaceInfo_asm2c);
    rs_set(0xdc, ren_LockSurface_asm2c);
    rs_set(0xe0, ren_UnlockSurface_asm2c);
    rs_set(0xe4, ren_OptimizeSurface_asm2c);
    rs_set(0xe8, ren_UnoptimizeSurface_asm2c);
    rs_set(0xec, ren_LockScreen_asm2c);
    rs_set(0xf0, ren_UnlockScreen_asm2c);
    rs_set(0xf4, ren_BlitToScreen_asm2c);
    rs_set(0xf8, ren_WarpToScreen_asm2c);
    rs_set(0xfc, ren_MakeScreenShot_asm2c);
    rs_set(0x100, ren_ReadConsoleVariables_asm2c);
    rs_set(0x104, ren_BlitFromScreen_asm2c);
    if (winapi_debug) eprintf("render: RenderDLLSetup\n");
}

// the one device, named like DirectDraw's primary display device in the original
#define RENDER_DEVICE_NAME "display"
#define RENDER_DEVICE_DESC "Primary Display Driver"

// the modes the options menu offers (32 and 16 bits; the screen is 32-bit either way). The device is named
// like the original's DirectDraw primary device: the Graphics page lists only the modes whose internal name
// is the CardDesc console variable ("display"), and with none listed it crashes on leaving the page.
// (The page also drops the modes that aren't 4:3.)
RMode32 * CCALL GetSupportedModes_c(void)
{
    static const int sizes[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 960 },
                                    { 1400, 1050 }, { 1600, 1200 }, { 1920, 1440 }, { 2048, 1536 } };
    RMode32 *head = NULL;
    int i, d;

    for (i = (int)(sizeof(sizes) / sizeof(sizes[0])) - 1; i >= 0; i--)
    {
        for (d = 0; d < 2; d++)
        {
            RMode32 *m = (RMode32 *)game_calloc(1, sizeof(RMode32));
            m->bHardware = 1;
            strcpy(m->internalName, RENDER_DEVICE_NAME);
            strcpy(m->description, RENDER_DEVICE_DESC);
            m->width = sizes[i][0];
            m->height = sizes[i][1];
            m->bitDepth = d ? 16 : 32;
            m->pNext = head;
            head = m;
        }
    }
    return head;
}

void CCALL FreeModeList_c(RMode32 *pModes)
{
    while (pModes != NULL)
    {
        RMode32 *next = (RMode32 *)(void *)pModes->pNext;
        game_free(pModes);
        pModes = next;
    }
}

/* ------------------------------------------------------------------ */
/* init                                                                */

int32_t CCALL ren_Init_c(RenderStructInit32 *pInit)
{
    pInit->rendererVersion = LTRENDER_VERSION;
    screen_w = (int)pInit->mode.width;
    screen_h = (int)pInit->mode.height;
    if ((screen_w <= 0) || (screen_h <= 0)) { screen_w = 640; screen_h = 480; }
    if (!display_create(game.title, screen_w, screen_h))
    {
        eprintf("render: can't create the display\n");
        return RENDER_ERROR;
    }
    display_resize(screen_w, screen_h);
    screen = display_pixels;
    // the device in use goes back into the mode (the engine's current mode: the Graphics page compares it
    // with the listed modes to select the current one)
    memset(pInit->mode.internalName, 0, sizeof(pInit->mode.internalName));
    memset(pInit->mode.description, 0, sizeof(pInit->mode.description));
    strcpy(pInit->mode.internalName, RENDER_DEVICE_NAME);
    strcpy(pInit->mode.description, RENDER_DEVICE_DESC);
    *(uint32_t *)(rs + RS_WIDTH) = screen_w;
    *(uint32_t *)(rs + RS_HEIGHT) = screen_h;
    *(uint32_t *)(rs + RS_INITTED) = 1;
    if (winapi_debug) eprintf("render: Init %dx%dx%u (%s)\n", screen_w, screen_h, pInit->mode.bitDepth, pInit->mode.internalName);
    return RENDER_OK;
}

void CCALL ren_Term_c(void)
{
    if (winapi_debug) eprintf("render: Term\n");
    if (rs != NULL) *(uint32_t *)(rs + RS_INITTED) = 0;
}

uint32_t CCALL ren_GetInfoFlags_c(void) { return 0; }

/* ------------------------------------------------------------------ */
/* DirectDraw stand-ins: the engine's Bink video manager gets d3d.ren's DirectDraw object and back buffer        */
/* through GetHook, creates an offscreen surface for the movie, locks it for BinkCopyToBuffer and blits it to    */
/* the back buffer (stretched or centred, colour-filled borders). Only what it uses is implemented (com.spec).   */

extern uint32_t IDirectDraw7Vtbl_asm2c, IDirectDrawSurface7Vtbl_asm2c;

#define DD_OK 0
#define DDERR_INVALIDPARAMS 0x80070057
#define E_NOINTERFACE_ 0x80004002
#define DDBLT_COLORFILL 0x00000400
#define DD_SURFACE_MAGIC 0x53464444      // "DDFS"

typedef struct {
    uint32_t lpVtbl;
    uint32_t magic;
    uint32_t refs;
    int32_t width, height, pitch;
    uint32_t pixels;                    // XRGB8888 in game memory
    int backbuffer;                     // the screen (render.c's frame)
} dd_surface;

typedef struct { int32_t left, top, right, bottom; } dd_rect;

static uint32_t *dd_object;             // { lpVtbl }
static dd_surface *dd_back;

static dd_surface *dd_get(dd_surface *s)
{
    if ((s == NULL) || (s->magic != DD_SURFACE_MAGIC)) return NULL;
    if (s->backbuffer)
    {
        // the screen may have been reallocated (render mode change)
        s->width = screen_w;
        s->height = screen_h;
        s->pitch = screen_w * 4;
        s->pixels = (uint32_t)(uintptr_t)screen;
    }
    return s;
}

// DDSURFACEDESC2 (124 bytes): the size, flags, height, width, pitch, lpSurface (+0x24), pixel format (+0x48),
// caps (+0x68); 32-bit RGB as GetScreenFormat
static void dd_fill_desc(uint32_t *d, int w, int h, int pitch, uint32_t pixels)
{
    if (d[0] < 124) d[0] = 124;
    d[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (pixels ? 0x800 : 0);
    d[2] = (uint32_t)h;
    d[3] = (uint32_t)w;
    d[4] = (uint32_t)pitch;
    d[0x24 / 4] = pixels;
    d[0x48 / 4] = 32;                   // DDPIXELFORMAT
    d[0x4c / 4] = 0x40;                 // DDPF_RGB
    d[0x50 / 4] = 0;
    d[0x54 / 4] = 32;
    d[0x58 / 4] = 0x00FF0000;
    d[0x5c / 4] = 0x0000FF00;
    d[0x60 / 4] = 0x000000FF;
    d[0x64 / 4] = 0;
    d[0x68 / 4] = 0x40;                 // DDSCAPS_OFFSCREENPLAIN
}

static void dd_surface_rect(dd_surface *s, const dd_rect *r, dd_rect *out)
{
    if (r != NULL) *out = *r;
    else { out->left = 0; out->top = 0; out->right = s->width; out->bottom = s->height; }
}

uint32_t CCALL IDirectDraw7_QueryInterface_c(void *self, void *riid, uint32_t *ppv) { if (ppv != NULL) *ppv = 0; return E_NOINTERFACE_; }
uint32_t CCALL IDirectDraw7_AddRef_c(void *self) { return 1; }
uint32_t CCALL IDirectDraw7_Release_c(void *self) { return 1; }

uint32_t CCALL IDirectDraw7_GetDisplayMode_c(void *self, uint32_t *ddsd)
{
    if (ddsd == NULL) return DDERR_INVALIDPARAMS;
    dd_fill_desc(ddsd, screen_w, screen_h, screen_w * 4, 0);
    return DD_OK;
}

uint32_t CCALL IDirectDraw7_CreateSurface_c(void *self, uint32_t *ddsd, uint32_t *pp, void *outer)
{
    dd_surface *s;
    int w, h;

    if ((ddsd == NULL) || (pp == NULL)) return DDERR_INVALIDPARAMS;
    w = (int)ddsd[3];
    h = (int)ddsd[2];
    if ((w <= 0) || (h <= 0) || (w > 4096) || (h > 4096)) return DDERR_INVALIDPARAMS;
    s = (dd_surface *)game_calloc(1, sizeof(dd_surface));
    if (s == NULL) return DDERR_INVALIDPARAMS;
    s->lpVtbl = (uint32_t)(uintptr_t)&IDirectDrawSurface7Vtbl_asm2c;
    s->magic = DD_SURFACE_MAGIC;
    s->refs = 1;
    s->width = w;
    s->height = h;
    s->pitch = w * 4;
    s->pixels = (uint32_t)(uintptr_t)game_calloc((uint32_t)w * h, 4);
    *pp = (uint32_t)(uintptr_t)s;
    if (winapi_debug) eprintf("render: DirectDraw surface %dx%d\n", w, h);
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface7_QueryInterface_c(void *self, void *riid, uint32_t *ppv) { if (ppv != NULL) *ppv = 0; return E_NOINTERFACE_; }
uint32_t CCALL IDirectDrawSurface7_AddRef_c(dd_surface *self) { return (dd_get(self) != NULL) ? ++self->refs : 0; }

uint32_t CCALL IDirectDrawSurface7_Release_c(dd_surface *self)
{
    dd_surface *s = dd_get(self);
    if (s == NULL) return 0;
    if (s->backbuffer) return 1;
    if (--s->refs != 0) return s->refs;
    s->magic = 0;
    game_free((void *)(uintptr_t)s->pixels);
    game_free(s);
    return 0;
}

uint32_t CCALL IDirectDrawSurface7_IsLost_c(void *self) { return DD_OK; }
uint32_t CCALL IDirectDrawSurface7_Restore_c(void *self) { return DD_OK; }

uint32_t CCALL IDirectDrawSurface7_GetSurfaceDesc_c(dd_surface *self, uint32_t *ddsd)
{
    dd_surface *s = dd_get(self);
    if ((s == NULL) || (ddsd == NULL)) return DDERR_INVALIDPARAMS;
    dd_fill_desc(ddsd, s->width, s->height, s->pitch, 0);
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface7_Lock_c(dd_surface *self, dd_rect *rect, uint32_t *ddsd, uint32_t flags, void *event)
{
    dd_surface *s = dd_get(self);
    uint32_t p;
    if ((s == NULL) || (ddsd == NULL) || (s->pixels == 0)) return DDERR_INVALIDPARAMS;
    p = s->pixels;
    if (rect != NULL) p += rect->top * s->pitch + rect->left * 4;
    dd_fill_desc(ddsd, s->width, s->height, s->pitch, p);
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface7_Unlock_c(void *self, void *rect) { return DD_OK; }

// the back buffer is the 2D frame over the 3D picture: what's blitted to it covers it (alpha 255)
static void dd_blit(dd_surface *d, const dd_rect *dr0, dd_surface *src, const dd_rect *sr0)
{
    dd_rect dr, sr;
    int x, y, dw, dh, sw, sh;
    uint32_t opaque = d->backbuffer ? 0xFF000000u : 0;

    dd_surface_rect(d, dr0, &dr);
    dd_surface_rect(src, sr0, &sr);
    dw = dr.right - dr.left;
    dh = dr.bottom - dr.top;
    sw = sr.right - sr.left;
    sh = sr.bottom - sr.top;
    if ((dw <= 0) || (dh <= 0) || (sw <= 0) || (sh <= 0)) return;
    for (y = 0; y < dh; y++)
    {
        int dy = dr.top + y, sy = sr.top + (int)((int64_t)y * sh / dh);
        if ((dy < 0) || (dy >= d->height) || (sy < 0) || (sy >= src->height)) continue;
        uint32_t *dp = (uint32_t *)(uintptr_t)(d->pixels + dy * d->pitch);
        const uint32_t *sp = (const uint32_t *)(uintptr_t)(src->pixels + sy * src->pitch);
        for (x = 0; x < dw; x++)
        {
            int dx = dr.left + x, sx = sr.left + (int)((int64_t)x * sw / dw);
            if ((dx < 0) || (dx >= d->width) || (sx < 0) || (sx >= src->width)) continue;
            dp[dx] = (sp[sx] & 0x00FFFFFF) | opaque | (opaque ? 0 : (sp[sx] & 0xFF000000));
        }
    }
}

uint32_t CCALL IDirectDrawSurface7_Blt_c(dd_surface *self, dd_rect *dest_rect, dd_surface *src, dd_rect *src_rect, uint32_t flags, uint32_t *fx)
{
    dd_surface *d = dd_get(self);
    if ((d == NULL) || (d->pixels == 0)) return DDERR_INVALIDPARAMS;
    if (flags & DDBLT_COLORFILL)
    {
        dd_rect r;
        int x, y;
        uint32_t c = ((fx != NULL) ? fx[0x50 / 4] : 0) | (d->backbuffer ? 0xFF000000u : 0);
        dd_surface_rect(d, dest_rect, &r);
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > d->width) r.right = d->width;
        if (r.bottom > d->height) r.bottom = d->height;
        for (y = r.top; y < r.bottom; y++)
        {
            uint32_t *p = (uint32_t *)(uintptr_t)(d->pixels + y * d->pitch);
            for (x = r.left; x < r.right; x++) p[x] = c;
        }
        return DD_OK;
    }
    src = dd_get(src);
    if ((src == NULL) || (src->pixels == 0)) return DDERR_INVALIDPARAMS;
    dd_blit(d, dest_rect, src, src_rect);
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface7_BltFast_c(dd_surface *self, uint32_t x, uint32_t y, dd_surface *src, dd_rect *src_rect, uint32_t flags)
{
    dd_surface *d = dd_get(self);
    dd_rect sr, dr;
    src = dd_get(src);
    if ((d == NULL) || (src == NULL) || (d->pixels == 0) || (src->pixels == 0)) return DDERR_INVALIDPARAMS;
    dd_surface_rect(src, src_rect, &sr);
    dr.left = (int32_t)x;
    dr.top = (int32_t)y;
    dr.right = dr.left + (sr.right - sr.left);
    dr.bottom = dr.top + (sr.bottom - sr.top);
    dd_blit(d, &dr, src, &sr);
    return DD_OK;
}

void * CCALL ren_GetHook_c(const char *pName)
{
    if (winapi_debug) eprintf("render: GetHook %s\n", (pName != NULL) ? pName : "(null)");
    if (pName == NULL) return NULL;
    if ((strcmp(pName, "LPDIRECTDRAW") == 0) || (strcmp(pName, "BACKBUFFER") == 0))
    {
        if (dd_object == NULL)
        {
            dd_object = (uint32_t *)game_calloc(1, 4);
            if (dd_object != NULL) dd_object[0] = (uint32_t)(uintptr_t)&IDirectDraw7Vtbl_asm2c;
        }
        if (dd_back == NULL)
        {
            dd_back = (dd_surface *)game_calloc(1, sizeof(dd_surface));
            if (dd_back != NULL)
            {
                dd_back->lpVtbl = (uint32_t)(uintptr_t)&IDirectDrawSurface7Vtbl_asm2c;
                dd_back->magic = DD_SURFACE_MAGIC;
                dd_back->refs = 1;
                dd_back->backbuffer = 1;
            }
        }
        if (screen == NULL) return NULL;
        return (strcmp(pName, "LPDIRECTDRAW") == 0) ? (void *)dd_object : (void *)dd_get(dd_back);
    }
    return NULL;
}

void CCALL ren_RenderCommand_c(int32_t argc, PTR32(char) *argv)
{
    if (winapi_debug) eprintf("render: RenderCommand %s\n", (argc > 0) ? (const char *)argv[0] : "");
}

void CCALL ren_ReadConsoleVariables_c(void) {}

// the screen: 32-bit ARGB
uint32_t CCALL ren_GetScreenFormat_c(uint8_t *pFormat)
{
    static const uint32_t masks[4] = { 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF };   // alpha, red, green, blue
    static const uint32_t first[4] = { 24, 16, 8, 0 };
    int i;
    *(uint32_t *)(pFormat + 0x04) = 3;      // BPP_32
    for (i = 0; i < 4; i++)
    {
        *(uint32_t *)(pFormat + 0x08 + 4 * i) = masks[i];
        *(uint32_t *)(pFormat + 0x18 + 4 * i) = 8;
        *(uint32_t *)(pFormat + 0x28 + 4 * i) = first[i];
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 3D: render3d.cpp (Godot)                                            */

#ifndef HAVE_GODOT
void r3d_set_render_struct(uint8_t *p) {}
uint32_t r3d_create_context(uint32_t world) { static uint32_t dummy[4]; return (uint32_t)(uintptr_t)dummy; }
void r3d_delete_context(uint32_t context) {}
int r3d_render_scene(uint8_t *scene) { return 0; }
void r3d_end_frame(int drawn) {}
void r3d_unbind_texture(void *texture) {}
// x64/canvas-asm.asm's targets (canvases are drawn by render3d.cpp)
uint32_t canvas_DrawPrimitive_c(uint32_t self, uint32_t verts, uint32_t n, uint32_t flags) { return 1; }
uint32_t canvas_SetState_c(uint32_t self, uint32_t state, uint32_t val) { return 1; }
uint32_t canvas_GetState_c(uint32_t self, uint32_t state, uint32_t pval) { return 1; }
uint32_t canvas_SetTexture_c(uint32_t self, uint32_t name) { return 1; }
uint32_t canvas_GetTexelSize_c(uint32_t self, uint32_t pu, uint32_t pv) { return 1; }
#endif

static int scene_drawn;     // RenderScene in this frame

// d3d.ren loads the texture's data when it's bound (d3d_CreateAndLoadTexture -> RenderStruct::GetTexture): that sets
// SharedTexture::m_pCommandLine, which the engine hands to the game right after (IClientShell::OnTextureLoad: the
// vision modes' texture states are registered from its "ViewModes" token)
void CCALL ren_BindTexture_c(void *pTexture, uint32_t bTextureChanged)
{
    if (bTextureChanged) r3d_unbind_texture(pTexture);
    if ((pTexture != NULL) && (rs != NULL))
    {
        uint32_t args[1] = { (uint32_t)(uintptr_t)pTexture };
        call_game(*(uint32_t *)(rs + 0x08), 1, args);
    }
}
void CCALL ren_UnbindTexture_c(void *pTexture) { r3d_unbind_texture(pTexture); }
void CCALL ren_RebindLightmaps_c(void *pContext) {}

// RenderContextInit: the world (MainWorld*)
uint32_t CCALL ren_CreateContext_c(uint32_t *pInit)
{
    return r3d_create_context((pInit != NULL) ? pInit[0] : 0);
}
void CCALL ren_DeleteContext_c(uint32_t hContext) { r3d_delete_context(hContext); }

uint32_t CCALL ren_Start3D_c(void) { in_3d = 1; return 1; }
uint32_t CCALL ren_End3D_c(void) { in_3d = 0; return 1; }
uint32_t CCALL ren_IsIn3D_c(void) { return in_3d; }

int32_t CCALL ren_RenderScene_c(uint8_t *pScene)
{
    int32_t *rect;
    int x, y;

    if (pScene == NULL) return RENDER_OK;
    if (!r3d_render_scene(pScene)) return RENDER_OK;
    scene_drawn = 1;
    // the scene's rectangle shows the 3D picture: transparent in the overlay
    rect = (int32_t *)(pScene + 0xbc);
    if (screen != NULL)
    {
        LTRect32 r = { rect[0], rect[1], rect[2], rect[3] };
        clip_rect(&r, screen_w, screen_h);
        for (y = r.top; y < r.bottom; y++)
            for (x = r.left; x < r.right; x++) screen[y * screen_w + x] = 0;
    }
    return RENDER_OK;
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */

// color: 0-255 floats
void CCALL ren_Clear_c(LTRect32 *pRect, uint32_t flags, LTVector32 *pColor)
{
    LTRect32 r = { 0, 0, screen_w, screen_h };
    uint32_t c = 0;
    int x, y;

    if ((screen == NULL) || !(flags & 1)) return;     // CLEARSCREEN_SCREEN (CLEARSCREEN_RENDER is the z buffer)
    if (pRect != NULL) r = *pRect;
    clip_rect(&r, screen_w, screen_h);
    c = 0xFF000000;
    if (pColor != NULL)
    {
        int cr = (int)pColor->x, cg = (int)pColor->y, cb = (int)pColor->z;
        c |= ((uint32_t)(cr & 0xFF) << 16) | ((uint32_t)(cg & 0xFF) << 8) | (uint32_t)(cb & 0xFF);
    }
    for (y = r.top; y < r.bottom; y++)
        for (x = r.left; x < r.right; x++) screen[y * screen_w + x] = c;
}

// frame pacing: the original renderer waited for the vertical retrace; AVP2_FPS (default 60, 0 = unlimited)
static void limit_frame_rate(void)
{
    static int fps = -1;
    static uint64_t next;
    uint64_t now, freq, period;

    if (fps < 0)
    {
        const char *e = getenv("AVP2_FPS");
        fps = (e != NULL) ? atoi(e) : 60;
        if (fps < 0) fps = 0;
    }
    if (fps == 0) return;
    freq = SDL_GetPerformanceFrequency();
    period = freq / (uint64_t)fps;
    now = SDL_GetPerformanceCounter();
    if ((next == 0) || (now > next + period)) next = now;     // first frame, or fell behind: no catching up
    while (now < next)
    {
        uint64_t left_ms = (next - now) * 1000 / freq;
        if (left_ms > 1) SDL_Delay((uint32_t)(left_ms - 1));
        now = SDL_GetPerformanceCounter();
    }
    next += period;
}

void CCALL ren_SwapBuffers_c(uint32_t flags)
{
    frames++;
    if (winapi_debug && (frames == 1 || frames % 300 == 0)) eprintf("render: frame %u\n", frames);
    r3d_end_frame(scene_drawn);
    scene_drawn = 0;
    display_invalidate();
    display_present(1);
    if (!host_vsync()) limit_frame_rate();
}

static uint32_t locked_screen_buffer;

uint32_t CCALL ren_LockScreen_c(int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t *pData, int32_t *pPitch)
{
    if ((screen == NULL) || (pData == NULL) || (pPitch == NULL)) return 0;
    *pData = (uint32_t)(uintptr_t)(screen + top * screen_w + left);
    *pPitch = screen_w * 4;
    locked_screen_buffer = 1;
    return 1;
}

void CCALL ren_UnlockScreen_c(void) { locked_screen_buffer = 0; }

void CCALL ren_MakeScreenShot_c(const char *pFilename)
{
    SDL_Surface *s;
    if ((screen == NULL) || (pFilename == NULL)) return;
    s = SDL_CreateRGBSurfaceWithFormatFrom(screen, screen_w, screen_h, 32, screen_w * 4, SDL_PIXELFORMAT_XRGB8888);
    if (s != NULL) { SDL_SaveBMP(s, pFilename); SDL_FreeSurface(s); }
}

/* ------------------------------------------------------------------ */
/* surfaces                                                            */

uint32_t CCALL ren_CreateSurface_c(int32_t width, int32_t height)
{
    surface *s;
    if ((width <= 0) || (height <= 0)) return 0;
    s = (surface *)game_calloc(1, sizeof(surface));
    s->magic = SURFACE_MAGIC;
    s->width = width;
    s->height = height;
    s->pitch = width * 4;
    s->pixels = (uint32_t *)game_calloc((uint32_t)width * height, 4);
    return (uint32_t)(uintptr_t)s;
}

void CCALL ren_DeleteSurface_c(uint32_t hSurf)
{
    surface *s = get_surface(hSurf);
    if (s == NULL) return;
    s->magic = 0;
    game_free(s->pixels);
    game_free(s);
}

void CCALL ren_GetSurfaceInfo_c(uint32_t hSurf, uint32_t *pWidth, uint32_t *pHeight, int32_t *pPitch)
{
    surface *s = get_surface(hSurf);
    if (pWidth) *pWidth = s ? s->width : 0;
    if (pHeight) *pHeight = s ? s->height : 0;
    if (pPitch) *pPitch = s ? s->pitch : 0;
}

void * CCALL ren_LockSurface_c(uint32_t hSurf)
{
    surface *s = get_surface(hSurf);
    return s ? s->pixels : NULL;
}

void CCALL ren_UnlockSurface_c(uint32_t hSurf) {}

uint32_t CCALL ren_OptimizeSurface_c(uint32_t hSurf, uint32_t transparentColor)
{
    surface *s = get_surface(hSurf);
    if (s == NULL) return 0;
    s->optimized = 1;
    s->transparent = transparentColor;
    return 1;
}

void CCALL ren_UnoptimizeSurface_c(uint32_t hSurf)
{
    surface *s = get_surface(hSurf);
    if (s != NULL) s->optimized = 0;
}

/* ------------------------------------------------------------------ */
/* optimized 2D                                                        */

uint32_t CCALL ren_StartOptimized2D_c(void) { in_optimized2d = 1; return 1; }
void CCALL ren_EndOptimized2D_c(void) { in_optimized2d = 0; }
uint32_t CCALL ren_IsInOptimized2D_c(void) { return in_optimized2d; }
uint32_t CCALL ren_SetOptimized2DBlend_c(uint32_t blend) { blend_mode = blend; return 1; }
uint32_t CCALL ren_GetOptimized2DBlend_c(uint32_t *pBlend) { if (pBlend) *pBlend = blend_mode; return 1; }
uint32_t CCALL ren_SetOptimized2DColor_c(uint32_t color) { blend_color = color; return 1; }
uint32_t CCALL ren_GetOptimized2DColor_c(uint32_t *pColor) { if (pColor) *pColor = blend_color; return 1; }

static inline uint32_t clamp8(int v) { return (v < 0) ? 0 : ((v > 255) ? 255 : (uint32_t)v); }

// one pixel: src (already color-modulated) over dst with the optimized 2D blend mode. The screen is
// premultiplied ARGB (alpha = how much of the 3D picture below is covered): the modes that can be expressed
// that way are exact over the 3D world (alpha, solid, add, mask & add); multiply and mask only affect 2D.
static inline uint32_t blend_pixel(uint32_t src, uint32_t dst, uint32_t mode, int alpha256)
{
    int sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t da = dst & 0xFF000000;
    switch (mode)
    {
        case 1:     // solid
            return (src & 0xFFFFFF) | 0xFF000000;
        case 2:     // add
            return da | (clamp8(sr + dr) << 16) | (clamp8(sg + dg) << 8) | clamp8(sb + db);
        case 3:     // multiply
            return da | ((uint32_t)(sr * dr / 255) << 16) | ((uint32_t)(sg * dg / 255) << 8) | (uint32_t)(sb * db / 255);
        case 4:     // multiply * 2
            return da | (clamp8(sr * dr / 128) << 16) | (clamp8(sg * dg / 128) << 8) | clamp8(sb * db / 128);
        case 5:     // mask: (1 - src) * dest
            return da | ((uint32_t)((255 - sr) * dr / 255) << 16) | ((uint32_t)((255 - sg) * dg / 255) << 8) | (uint32_t)((255 - sb) * db / 255);
        case 6:     // mask & add: src + (1 - src) * dest
            return da | (clamp8(sr + (255 - sr) * dr / 255) << 16) | (clamp8(sg + (255 - sg) * dg / 255) << 8) | clamp8(sb + (255 - sb) * db / 255);
        default:    // alpha
        {
            uint32_t a = (uint32_t)((int)(dst >> 24) + (((255 - (int)(dst >> 24)) * alpha256) >> 8));
            return (a << 24) | ((uint32_t)(dr + (((sr - dr) * alpha256) >> 8)) << 16) | ((uint32_t)(dg + (((sg - dg) * alpha256) >> 8)) << 8) | (uint32_t)(db + (((sb - db) * alpha256) >> 8));
        }
    }
}

void CCALL ren_BlitToScreen_c(BlitRequest32 *req)
{
    surface *s;
    LTRect32 src, dst;
    int x, y, dw, dh, sw, sh, alpha256, use_key, optimized;
    uint32_t key, mod_r, mod_g, mod_b;

    if ((screen == NULL) || (req == NULL) || ((s = get_surface(req->hBuffer)) == NULL)) return;
    if (req->pSrcRect != NULL) src = *(LTRect32 *)req->pSrcRect; else { src.left = 0; src.top = 0; src.right = s->width; src.bottom = s->height; }
    if (req->pDestRect != NULL) dst = *(LTRect32 *)req->pDestRect; else { dst.left = 0; dst.top = 0; dst.right = src.right - src.left; dst.bottom = src.bottom - src.top; }
    sw = src.right - src.left; sh = src.bottom - src.top;
    dw = dst.right - dst.left; dh = dst.bottom - dst.top;
    if ((sw <= 0) || (sh <= 0) || (dw <= 0) || (dh <= 0)) return;

    optimized = in_optimized2d && s->optimized;
    use_key = (req->options & 1) != 0;
    key = (optimized ? s->transparent : req->transparent) & 0xFFFFFF;
    alpha256 = (int)(req->alpha * 256.0f);
    if (alpha256 < 0) alpha256 = 0; else if (alpha256 > 256) alpha256 = 256;
    mod_r = (blend_color >> 16) & 0xFF; mod_g = (blend_color >> 8) & 0xFF; mod_b = blend_color & 0xFF;

    for (y = 0; y < dh; y++)
    {
        int ty = dst.top + y, sy;
        if ((ty < 0) || (ty >= screen_h)) continue;
        sy = src.top + (int)((int64_t)y * sh / dh);
        if ((sy < 0) || (sy >= s->height)) continue;
        for (x = 0; x < dw; x++)
        {
            int tx = dst.left + x, sx;
            uint32_t p, *d;
            if ((tx < 0) || (tx >= screen_w)) continue;
            sx = src.left + (int)((int64_t)x * sw / dw);
            if ((sx < 0) || (sx >= s->width)) continue;
            p = s->pixels[sy * s->width + sx] & 0xFFFFFF;
            if (use_key && (p == key)) continue;
            d = &screen[ty * screen_w + tx];
            if (optimized)
            {
                if ((mod_r & mod_g & mod_b) != 0xFF)
                    p = (((p >> 16 & 0xFF) * mod_r / 255) << 16) | (((p >> 8 & 0xFF) * mod_g / 255) << 8) | ((p & 0xFF) * mod_b / 255);
                *d = blend_pixel(p, *d, blend_mode, alpha256);
            }
            else if (alpha256 < 256)
            {
                *d = blend_pixel(p, *d, 0, alpha256);
            }
            else *d = p | 0xFF000000;
        }
    }
}

uint32_t CCALL ren_WarpToScreen_c(BlitRequest32 *req)
{
    static int reported;
    if (!reported && winapi_debug) { reported = 1; eprintf("render: WarpToScreen (not supported yet)\n"); }
    return 0;
}

void CCALL ren_BlitFromScreen_c(BlitRequest32 *req)
{
    surface *s;
    LTRect32 src, dst;
    int x, y;

    if ((screen == NULL) || (req == NULL) || ((s = get_surface(req->hBuffer)) == NULL)) return;
    if (req->pSrcRect != NULL) src = *(LTRect32 *)req->pSrcRect; else { src.left = 0; src.top = 0; src.right = screen_w; src.bottom = screen_h; }
    if (req->pDestRect != NULL) dst = *(LTRect32 *)req->pDestRect; else { dst.left = 0; dst.top = 0; dst.right = s->width; dst.bottom = s->height; }
    for (y = 0; y < dst.bottom - dst.top; y++)
    {
        int sy = src.top + y, ty = dst.top + y;
        if ((sy < 0) || (sy >= screen_h) || (ty < 0) || (ty >= s->height)) continue;
        for (x = 0; x < dst.right - dst.left; x++)
        {
            int sx = src.left + x, tx = dst.left + x;
            if ((sx < 0) || (sx >= screen_w) || (tx < 0) || (tx >= s->width)) continue;
            s->pixels[ty * s->width + tx] = screen[sy * screen_w + sx];
        }
    }
}

EXTERN_C_END
