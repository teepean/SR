/**
 *
 *  DirectDraw emulation (IDirectDraw/IDirectDraw2, IDirectDrawSurface, IDirectDrawPalette, IDirectDrawClipper).
 *
 *  Surfaces are plain memory (8 or 16 bpp). The primary surface is the screen: whenever it changes
 *  (Unlock, Blt, Flip, palette change) it's converted into the display framebuffer (display.c).
 *  Used by the game's 2D screens in Glide mode (8-bit 640x480) and by its DirectDraw software renderer.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "winapi.h"
#include "display.h"
#include "ptr32.h"
#include "Game-Memory.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#define DD_OK 0
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_NOTFOUND 0x887600FFu
#define DDERR_OUTOFMEMORY 0x8007000Eu
#define DDERR_NOPALETTEATTACHED 0x8876023Cu
#define DDERR_NOCLIPPERATTACHED 0x88760208u
#define DDERR_NOCOLORKEY 0x887600D7u
#define E_NOINTERFACE 0x80004002u

#define DDSD_CAPS 0x00000001
#define DDSD_HEIGHT 0x00000002
#define DDSD_WIDTH 0x00000004
#define DDSD_PITCH 0x00000008
#define DDSD_BACKBUFFERCOUNT 0x00000020
#define DDSD_LPSURFACE 0x00000800
#define DDSD_PIXELFORMAT 0x00001000
#define DDSD_CKSRCBLT 0x00010000

#define DDSCAPS_BACKBUFFER 0x00000004
#define DDSCAPS_COMPLEX 0x00000008
#define DDSCAPS_FLIP 0x00000010
#define DDSCAPS_FRONTBUFFER 0x00000020
#define DDSCAPS_OFFSCREENPLAIN 0x00000040
#define DDSCAPS_PALETTE 0x00000100
#define DDSCAPS_PRIMARYSURFACE 0x00000200
#define DDSCAPS_SYSTEMMEMORY 0x00000800
#define DDSCAPS_VIDEOMEMORY 0x00004000
#define DDSCAPS_VISIBLE 0x00008000
#define DDSCAPS_LOCALVIDMEM 0x10000000

#define DDPF_PALETTEINDEXED8 0x00000020
#define DDPF_RGB 0x00000040

#define DDBLT_COLORFILL 0x00000400
#define DDBLT_KEYDEST 0x00002000
#define DDBLT_KEYSRC 0x00008000
#define DDBLT_KEYSRCOVERRIDE 0x00010000
#define DDBLTFAST_SRCCOLORKEY 0x00000001
#define DDBLTFAST_DESTCOLORKEY 0x00000002

#define DDCKEY_SRCBLT 0x00000008

#define DDPCAPS_8BIT 0x00000004

// IID Data1 values
#define IID_IDirectDraw_1 0x6C14DB80u
#define IID_IDirectDraw2_1 0xB3A6F3E0u
#define IID_IDirectDrawSurface_1 0x6C14DB81u
#define IID_IDirectDrawSurface2_1 0x57805885u

extern uint32_t IDirectDrawVtbl_asm2c;
extern uint32_t IDirectDraw2Vtbl_asm2c;
extern uint32_t IDirectDrawSurfaceVtbl_asm2c;
extern uint32_t IDirectDrawPaletteVtbl_asm2c;
extern uint32_t IDirectDrawClipperVtbl_asm2c;

typedef struct { int32_t left, top, right, bottom; } rect_t;

typedef struct dd_palette {
    PTR32(void) lpVtbl;
    uint32_t refs;
    uint8_t entries[256][4];    // PALETTEENTRY: r, g, b, flags
} dd_palette;

typedef struct dd_clipper {
    PTR32(void) lpVtbl;
    uint32_t refs;
    void *hwnd;
} dd_clipper;

typedef struct dd_surface {
    PTR32(void) lpVtbl;
    uint32_t refs;
    uint32_t caps;
    int width, height, bpp, pitch;
    uint8_t *pixels;
    struct dd_surface *back;        // attached back buffer (flipping chain)
    struct dd_surface *front;       // for a back buffer: the front surface
    dd_palette *palette;
    dd_clipper *clipper;
    int has_colorkey;
    uint32_t colorkey;
} dd_surface;

typedef struct dd_device {
    PTR32(void) lpVtbl;                   // IDirectDraw
    uint32_t refs;
    struct { PTR32(void) lpVtbl; struct dd_device *owner; } dd2;  // IDirectDraw2 view
    int width, height, bpp;
    dd_surface *primary;
} dd_device;

static dd_device *the_dd;

static void fill_pixel_format(uint32_t *pf, int bpp);


/* ------------------------------------------------------------------ */
/* presenting the primary surface                                      */

static void present_primary(void)
{
    dd_surface *p;
    int x, y, w, h;

    if ((the_dd == NULL) || ((p = the_dd->primary) == NULL)) return;
    if (!display_exists()) return;
    if ((display_width != p->width) || (display_height != p->height)) display_resize(p->width, p->height);

    w = p->width;
    h = p->height;
    if (p->bpp == 8)
    {
        uint32_t lut[256];
        for (x = 0; x < 256; x++)
        {
            if (p->palette != NULL) lut[x] = (p->palette->entries[x][0] << 16) | (p->palette->entries[x][1] << 8) | p->palette->entries[x][2];
            else lut[x] = x * 0x010101;
        }
        for (y = 0; y < h; y++)
        {
            const uint8_t *src = p->pixels + (size_t)y * p->pitch;
            uint32_t *dst = display_pixels + (size_t)y * display_width;
            for (x = 0; x < w; x++) dst[x] = lut[src[x]];
        }
    }
    else if (p->bpp == 16)
    {
        for (y = 0; y < h; y++)
        {
            const uint16_t *src = (const uint16_t *)(p->pixels + (size_t)y * p->pitch);
            uint32_t *dst = display_pixels + (size_t)y * display_width;
            for (x = 0; x < w; x++)
            {
                uint32_t v = src[x], r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                dst[x] = (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            }
        }
    }
    display_invalidate();
}

static int is_screen(const dd_surface *s)
{
    return (the_dd != NULL) && (s == the_dd->primary);
}


/* ------------------------------------------------------------------ */
/* IDirectDraw                                                         */

uint32_t CCALL DirectDrawCreate_c(void *lpGUID, PTR32(dd_device) *lplpDD, void *pUnkOuter)
{
    dd_device *dd;
    if (lplpDD == NULL) return DDERR_INVALIDPARAMS;
    dd = (dd_device *)game_calloc(1, sizeof(dd_device));
    dd->lpVtbl = &IDirectDrawVtbl_asm2c;
    dd->refs = 1;
    dd->dd2.lpVtbl = &IDirectDraw2Vtbl_asm2c;
    dd->dd2.owner = dd;
    dd->width = 640;
    dd->height = 480;
    dd->bpp = 8;
    the_dd = dd;
    *lplpDD = dd;
    if (winapi_debug) eprintf("DirectDrawCreate: %p\n", dd);
    return DD_OK;
}

uint32_t CCALL IDirectDraw_QueryInterface_c(dd_device *lpThis, uint32_t *riid, PTR32(void) *ppvObj)
{
    if (ppvObj == NULL) return DDERR_INVALIDPARAMS;
    *ppvObj = NULL;
    if (riid == NULL) return E_NOINTERFACE;
    if (riid[0] == IID_IDirectDraw2_1) { lpThis->refs++; *ppvObj = &lpThis->dd2; return DD_OK; }
    if (riid[0] == IID_IDirectDraw_1) { lpThis->refs++; *ppvObj = lpThis; return DD_OK; }
    if (winapi_debug) eprintf("IDirectDraw::QueryInterface %08x (not supported)\n", riid[0]);
    return E_NOINTERFACE;
}

uint32_t CCALL IDirectDraw_AddRef_c(dd_device *lpThis) { return ++lpThis->refs; }

uint32_t CCALL IDirectDraw_Release_c(dd_device *lpThis)
{
    if (lpThis->refs > 1) return --lpThis->refs;
    if (the_dd == lpThis) the_dd = NULL;
    lpThis->lpVtbl = NULL;
    game_free(lpThis);
    return 0;
}

uint32_t CCALL IDirectDraw_Compact_c(dd_device *lpThis) { return DD_OK; }

uint32_t CCALL IDirectDraw_CreateClipper_c(dd_device *lpThis, uint32_t dwFlags, PTR32(dd_clipper) *lplpClipper, void *pUnkOuter)
{
    dd_clipper *c;
    if (lplpClipper == NULL) return DDERR_INVALIDPARAMS;
    c = (dd_clipper *)game_calloc(1, sizeof(dd_clipper));
    c->lpVtbl = &IDirectDrawClipperVtbl_asm2c;
    c->refs = 1;
    *lplpClipper = c;
    return DD_OK;
}

uint32_t CCALL IDirectDraw_CreatePalette_c(dd_device *lpThis, uint32_t dwFlags, const uint8_t *lpColorTable, PTR32(dd_palette) *lplpPalette, void *pUnkOuter)
{
    dd_palette *p;
    if (lplpPalette == NULL) return DDERR_INVALIDPARAMS;
    p = (dd_palette *)game_calloc(1, sizeof(dd_palette));
    p->lpVtbl = &IDirectDrawPaletteVtbl_asm2c;
    p->refs = 1;
    if (lpColorTable != NULL) memcpy(p->entries, lpColorTable, (dwFlags & DDPCAPS_8BIT) ? 1024 : 1024);
    *lplpPalette = p;
    return DD_OK;
}

static dd_surface *new_surface(uint32_t caps, int width, int height, int bpp)
{
    dd_surface *s = (dd_surface *)game_calloc(1, sizeof(dd_surface));
    s->lpVtbl = &IDirectDrawSurfaceVtbl_asm2c;
    s->refs = 1;
    s->caps = caps;
    s->width = width;
    s->height = height;
    s->bpp = bpp;
    s->pitch = ((width * bpp / 8) + 3) & ~3;
    s->pixels = (uint8_t *)game_calloc(1, (size_t)s->pitch * height + 16);
    return s;
}

uint32_t CCALL IDirectDraw_CreateSurface_c(dd_device *lpThis, const uint32_t *desc, PTR32(dd_surface) *lplpSurface, void *pUnkOuter)
{
    // DDSURFACEDESC: dwSize 0, dwFlags 4, dwHeight 8, dwWidth 12, lPitch 16, dwBackBufferCount 20, ...,
    // ddckCKSrcBlt 64, ddpfPixelFormat 72 (dwSize, dwFlags, dwFourCC, dwRGBBitCount 84, ...), ddsCaps 104
    uint32_t flags, caps;
    int w, h, bpp;
    dd_surface *s;

    if ((desc == NULL) || (lplpSurface == NULL)) return DDERR_INVALIDPARAMS;
    flags = desc[1];
    caps = (flags & DDSD_CAPS) ? desc[26] : 0;

    if (caps & DDSCAPS_PRIMARYSURFACE)
    {
        w = lpThis->width;
        h = lpThis->height;
        bpp = lpThis->bpp;
    }
    else
    {
        w = (flags & DDSD_WIDTH) ? (int)desc[3] : lpThis->width;
        h = (flags & DDSD_HEIGHT) ? (int)desc[2] : lpThis->height;
        bpp = (flags & DDSD_PIXELFORMAT) ? (int)desc[21] : lpThis->bpp;
    }

    s = new_surface(caps | ((caps & DDSCAPS_PRIMARYSURFACE) ? (DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER) : 0), w, h, bpp);
    if (flags & DDSD_CKSRCBLT)
    {
        s->has_colorkey = 1;
        s->colorkey = desc[16];
    }

    if (caps & DDSCAPS_PRIMARYSURFACE)
    {
        lpThis->primary = s;
        if ((flags & DDSD_BACKBUFFERCOUNT) && (desc[5] > 0))
        {
            s->back = new_surface(DDSCAPS_BACKBUFFER | (caps & (DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY)), w, h, bpp);
            s->back->front = s;
        }
    }

    if (winapi_debug) eprintf("CreateSurface: caps 0x%x %dx%dx%d%s -> %p\n", caps, w, h, bpp, (s->back != NULL) ? " +back buffer" : "", s);
    *lplpSurface = s;
    return DD_OK;
}

uint32_t CCALL IDirectDraw_DuplicateSurface_c(dd_device *lpThis, dd_surface *src, PTR32(dd_surface) *dst) { return DDERR_UNSUPPORTED; }


uint32_t CCALL IDirectDraw_EnumDisplayModes_c(dd_device *lpThis, uint32_t dwFlags, void *desc, uint32_t context, uint32_t callback)
{
    // the game needs 640x480x8 (its tables also allow 800x600 and 1024x768, but everything is 640x480)
    static const int modes[][3] = { { 640, 480, 8 }, { 640, 480, 16 } };
    // the game gets a pointer to the description: low memory, not the C stack
    static uint32_t d[27];
    int i;

    if (callback == 0) return DDERR_INVALIDPARAMS;
    for (i = 0; i < 2; i++)
    {
        memset(d, 0, sizeof(d));
        d[0] = 108;
        d[1] = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
        d[2] = modes[i][1];
        d[3] = modes[i][0];
        d[4] = modes[i][0] * modes[i][2] / 8;
        d[6] = 60;
        fill_pixel_format(d + 18, modes[i][2]);
        {
            uint32_t args[2] = { (uint32_t)(uintptr_t) d, context };
            if (call_game(callback, 2, args) == 0) break;   // DDENUMRET_CANCEL (stdcall callback)
        }
    }
    return DD_OK;
}

uint32_t CCALL IDirectDraw_EnumSurfaces_c(dd_device *lpThis, uint32_t dwFlags, void *desc, void *context, void *callback) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDraw_FlipToGDISurface_c(dd_device *lpThis) { return DD_OK; }

uint32_t CCALL IDirectDraw_GetCaps_c(dd_device *lpThis, uint32_t *driver_caps, uint32_t *hel_caps)
{
    // DDCAPS: dwSize first; report no hardware features (all blits are software anyway)
    if (driver_caps != NULL) memset(driver_caps + 1, 0, ((driver_caps[0] >= 8) && (driver_caps[0] <= 1024) ? driver_caps[0] : 316) - 4);
    if (hel_caps != NULL) memset(hel_caps + 1, 0, ((hel_caps[0] >= 8) && (hel_caps[0] <= 1024) ? hel_caps[0] : 316) - 4);
    return DD_OK;
}

static void fill_pixel_format(uint32_t *pf, int bpp)
{
    memset(pf, 0, 32);
    pf[0] = 32;
    if (bpp == 8)
    {
        pf[1] = DDPF_PALETTEINDEXED8 | DDPF_RGB;
        pf[3] = 8;
    }
    else
    {
        pf[1] = DDPF_RGB;
        pf[3] = bpp;
        if (bpp == 16) { pf[4] = 0xF800; pf[5] = 0x07E0; pf[6] = 0x001F; }
        else { pf[4] = 0xFF0000; pf[5] = 0xFF00; pf[6] = 0xFF; }
    }
}

uint32_t CCALL IDirectDraw_GetDisplayMode_c(dd_device *lpThis, uint32_t *desc)
{
    if (desc == NULL) return DDERR_INVALIDPARAMS;
    memset(desc + 1, 0, 104);
    desc[0] = 108;
    desc[1] = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc[2] = lpThis->height;
    desc[3] = lpThis->width;
    desc[4] = lpThis->width * lpThis->bpp / 8;
    desc[6] = 60;
    fill_pixel_format(desc + 18, lpThis->bpp);
    return DD_OK;
}

uint32_t CCALL IDirectDraw_GetFourCCCodes_c(dd_device *lpThis, uint32_t *n, uint32_t *codes) { if (n) *n = 0; return DD_OK; }
uint32_t CCALL IDirectDraw_GetGDISurface_c(dd_device *lpThis, PTR32(dd_surface) *s) { if (s == NULL) return DDERR_INVALIDPARAMS; *s = lpThis->primary; if (*s) (*s)->refs++; return (*s != NULL) ? DD_OK : DDERR_NOTFOUND; }
uint32_t CCALL IDirectDraw_GetMonitorFrequency_c(dd_device *lpThis, uint32_t *f) { if (f) *f = 60; return DD_OK; }
uint32_t CCALL IDirectDraw_GetScanLine_c(dd_device *lpThis, uint32_t *l) { if (l) *l = 0; return DD_OK; }
uint32_t CCALL IDirectDraw_GetVerticalBlankStatus_c(dd_device *lpThis, uint32_t *b) { if (b) *b = 1; return DD_OK; }
uint32_t CCALL IDirectDraw_Initialize_c(dd_device *lpThis, void *guid) { return DD_OK; }
uint32_t CCALL IDirectDraw_RestoreDisplayMode_c(dd_device *lpThis) { return DD_OK; }
uint32_t CCALL IDirectDraw_SetCooperativeLevel_c(dd_device *lpThis, void *hwnd, uint32_t dwFlags) { return DD_OK; }

uint32_t CCALL IDirectDraw_SetDisplayMode_c(dd_device *lpThis, uint32_t w, uint32_t h, uint32_t bpp)
{
    if (winapi_debug) eprintf("SetDisplayMode: %ux%ux%u\n", w, h, bpp);
    if ((bpp != 8) && (bpp != 16)) return DDERR_UNSUPPORTED;
    lpThis->width = w;
    lpThis->height = h;
    lpThis->bpp = bpp;
    return DD_OK;
}

uint32_t CCALL IDirectDraw_WaitForVerticalBlank_c(dd_device *lpThis, uint32_t dwFlags, void *hEvent) { return DD_OK; }

/* IDirectDraw2: same object */
#define DD2(p) ((p)->owner)
typedef struct { PTR32(void) lpVtbl; dd_device *owner; } dd2_view;
uint32_t CCALL IDirectDraw2_QueryInterface_c(dd2_view *p, uint32_t *riid, PTR32(void) *ppv) { return IDirectDraw_QueryInterface_c(DD2(p), riid, ppv); }
uint32_t CCALL IDirectDraw2_AddRef_c(dd2_view *p) { return IDirectDraw_AddRef_c(DD2(p)); }
uint32_t CCALL IDirectDraw2_Release_c(dd2_view *p) { return IDirectDraw_Release_c(DD2(p)); }
uint32_t CCALL IDirectDraw2_Compact_c(dd2_view *p) { return DD_OK; }
uint32_t CCALL IDirectDraw2_CreateClipper_c(dd2_view *p, uint32_t f, PTR32(dd_clipper) *c, void *u) { return IDirectDraw_CreateClipper_c(DD2(p), f, c, u); }
uint32_t CCALL IDirectDraw2_CreatePalette_c(dd2_view *p, uint32_t f, const uint8_t *t, PTR32(dd_palette) *pal, void *u) { return IDirectDraw_CreatePalette_c(DD2(p), f, t, pal, u); }
uint32_t CCALL IDirectDraw2_CreateSurface_c(dd2_view *p, const uint32_t *d, PTR32(dd_surface) *s, void *u) { return IDirectDraw_CreateSurface_c(DD2(p), d, s, u); }
uint32_t CCALL IDirectDraw2_DuplicateSurface_c(dd2_view *p, dd_surface *s, PTR32(dd_surface) *d) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDraw2_EnumDisplayModes_c(dd2_view *p, uint32_t f, void *d, uint32_t c, uint32_t cb) { return IDirectDraw_EnumDisplayModes_c(DD2(p), f, d, c, cb); }
uint32_t CCALL IDirectDraw2_EnumSurfaces_c(dd2_view *p, uint32_t f, void *d, void *c, void *cb) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDraw2_FlipToGDISurface_c(dd2_view *p) { return DD_OK; }
uint32_t CCALL IDirectDraw2_GetCaps_c(dd2_view *p, uint32_t *a, uint32_t *b) { return IDirectDraw_GetCaps_c(DD2(p), a, b); }
uint32_t CCALL IDirectDraw2_GetDisplayMode_c(dd2_view *p, uint32_t *d) { return IDirectDraw_GetDisplayMode_c(DD2(p), d); }
uint32_t CCALL IDirectDraw2_GetFourCCCodes_c(dd2_view *p, uint32_t *n, uint32_t *c) { if (n) *n = 0; return DD_OK; }
uint32_t CCALL IDirectDraw2_GetGDISurface_c(dd2_view *p, PTR32(dd_surface) *s) { return IDirectDraw_GetGDISurface_c(DD2(p), s); }
uint32_t CCALL IDirectDraw2_GetMonitorFrequency_c(dd2_view *p, uint32_t *f) { if (f) *f = 60; return DD_OK; }
uint32_t CCALL IDirectDraw2_GetScanLine_c(dd2_view *p, uint32_t *l) { if (l) *l = 0; return DD_OK; }
uint32_t CCALL IDirectDraw2_GetVerticalBlankStatus_c(dd2_view *p, uint32_t *b) { if (b) *b = 1; return DD_OK; }
uint32_t CCALL IDirectDraw2_Initialize_c(dd2_view *p, void *g) { return DD_OK; }
uint32_t CCALL IDirectDraw2_RestoreDisplayMode_c(dd2_view *p) { return DD_OK; }
uint32_t CCALL IDirectDraw2_SetCooperativeLevel_c(dd2_view *p, void *h, uint32_t f) { return DD_OK; }
uint32_t CCALL IDirectDraw2_SetDisplayMode_c(dd2_view *p, uint32_t w, uint32_t h, uint32_t bpp, uint32_t refresh, uint32_t flags) { return IDirectDraw_SetDisplayMode_c(DD2(p), w, h, bpp); }
uint32_t CCALL IDirectDraw2_WaitForVerticalBlank_c(dd2_view *p, uint32_t f, void *e) { return DD_OK; }
uint32_t CCALL IDirectDraw2_GetAvailableVidMem_c(dd2_view *p, uint32_t *caps, uint32_t *total, uint32_t *free_)
{
    if (total) *total = 8 * 1024 * 1024;
    if (free_) *free_ = 8 * 1024 * 1024;
    return DD_OK;
}


/* ------------------------------------------------------------------ */
/* IDirectDrawSurface                                                  */

uint32_t CCALL IDirectDrawSurface_QueryInterface_c(dd_surface *lpThis, uint32_t *riid, PTR32(void) *ppvObj)
{
    if (ppvObj == NULL) return DDERR_INVALIDPARAMS;
    *ppvObj = NULL;
    if ((riid != NULL) && ((riid[0] == IID_IDirectDrawSurface_1) || (riid[0] == IID_IDirectDrawSurface2_1)))
    {
        // Surface2's extra methods (GetDDInterface, PageLock, PageUnlock) aren't used
        lpThis->refs++;
        *ppvObj = lpThis;
        return DD_OK;
    }
    if (winapi_debug) eprintf("IDirectDrawSurface::QueryInterface %08x (not supported)\n", (riid != NULL) ? riid[0] : 0);
    return E_NOINTERFACE;
}

uint32_t CCALL IDirectDrawSurface_AddRef_c(dd_surface *lpThis) { return ++lpThis->refs; }

static void free_surface(dd_surface *s)
{
    if ((the_dd != NULL) && (the_dd->primary == s)) the_dd->primary = NULL;
    game_free(s->pixels);
    s->lpVtbl = NULL;
    game_free(s);
}

uint32_t CCALL IDirectDrawSurface_Release_c(dd_surface *lpThis)
{
    if (lpThis->refs > 1) return --lpThis->refs;
    if (lpThis->front != NULL)
    {
        // a back buffer is owned by its front surface
        return 0;
    }
    if (lpThis->back != NULL) free_surface(lpThis->back);
    free_surface(lpThis);
    return 0;
}

uint32_t CCALL IDirectDrawSurface_AddAttachedSurface_c(dd_surface *lpThis, dd_surface *s) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDrawSurface_AddOverlayDirtyRect_c(dd_surface *lpThis, void *r) { return DDERR_UNSUPPORTED; }

static void clip_rect(rect_t *r, int w, int h)
{
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > w) r->right = w;
    if (r->bottom > h) r->bottom = h;
}

static void after_write(dd_surface *s)
{
    if (is_screen(s)) present_primary();
}

uint32_t CCALL IDirectDrawSurface_Blt_c(dd_surface *lpThis, const rect_t *dst_rect, dd_surface *src, const rect_t *src_rect, uint32_t dwFlags, const uint32_t *fx)
{
    rect_t dr, sr;
    int bytes = lpThis->bpp / 8;
    int x, y;

    if (dst_rect != NULL) dr = *dst_rect; else { dr.left = dr.top = 0; dr.right = lpThis->width; dr.bottom = lpThis->height; }

    if (dwFlags & DDBLT_COLORFILL)
    {
        // DDBLTFX: dwSize 0, ..., dwFillColor at offset 80
        uint32_t color = (fx != NULL) ? fx[20] : 0;
        clip_rect(&dr, lpThis->width, lpThis->height);
        for (y = dr.top; y < dr.bottom; y++)
        {
            uint8_t *row = lpThis->pixels + (size_t)y * lpThis->pitch;
            for (x = dr.left; x < dr.right; x++)
            {
                if (bytes == 1) row[x] = (uint8_t)color;
                else if (bytes == 2) ((uint16_t *)row)[x] = (uint16_t)color;
                else ((uint32_t *)row)[x] = color;
            }
        }
        after_write(lpThis);
        return DD_OK;
    }

    if (src == NULL) return DDERR_INVALIDPARAMS;
    if (src_rect != NULL) sr = *src_rect; else { sr.left = sr.top = 0; sr.right = src->width; sr.bottom = src->height; }
    if ((sr.right <= sr.left) || (sr.bottom <= sr.top) || (dr.right <= dr.left) || (dr.bottom <= dr.top)) return DD_OK;
    if (src->bpp != lpThis->bpp) return DDERR_UNSUPPORTED;

    {
        int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
        int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
        int keyed = (dwFlags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) != 0;
        uint32_t key = (dwFlags & DDBLT_KEYSRCOVERRIDE) && (fx != NULL) ? fx[23] : src->colorkey;   // DDBLTFX.ddckSrcColorkey at 92
        if ((dwFlags & DDBLT_KEYSRC) && !src->has_colorkey) keyed = 0;
        // copy through a temporary if source and destination are the same surface
        uint8_t *tmp = NULL;
        const uint8_t *sp = src->pixels;
        int spitch = src->pitch;
        if (src == lpThis)
        {
            tmp = (uint8_t *)malloc((size_t)src->pitch * src->height);
            memcpy(tmp, src->pixels, (size_t)src->pitch * src->height);
            sp = tmp;
        }
        for (y = 0; y < dh; y++)
        {
            int ty = dr.top + y, sy = sr.top + (int)((int64_t)y * sh / dh);
            uint8_t *drow;
            const uint8_t *srow;
            if ((ty < 0) || (ty >= lpThis->height) || (sy < 0) || (sy >= src->height)) continue;
            drow = lpThis->pixels + (size_t)ty * lpThis->pitch;
            srow = sp + (size_t)sy * spitch;
            for (x = 0; x < dw; x++)
            {
                int tx = dr.left + x, sx = sr.left + (int)((int64_t)x * sw / dw);
                uint32_t v;
                if ((tx < 0) || (tx >= lpThis->width) || (sx < 0) || (sx >= src->width)) continue;
                if (bytes == 1) v = srow[sx]; else if (bytes == 2) v = ((const uint16_t *)srow)[sx]; else v = ((const uint32_t *)srow)[sx];
                if (keyed && (v == key)) continue;
                if (bytes == 1) drow[tx] = (uint8_t)v; else if (bytes == 2) ((uint16_t *)drow)[tx] = (uint16_t)v; else ((uint32_t *)drow)[tx] = v;
            }
        }
        free(tmp);
    }
    after_write(lpThis);
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface_BltBatch_c(dd_surface *lpThis, void *a, uint32_t b, uint32_t c) { return DDERR_UNSUPPORTED; }

uint32_t CCALL IDirectDrawSurface_BltFast_c(dd_surface *lpThis, uint32_t x, uint32_t y, dd_surface *src, const rect_t *src_rect, uint32_t dwTrans)
{
    rect_t sr, dr;
    if (src == NULL) return DDERR_INVALIDPARAMS;
    if (src_rect != NULL) sr = *src_rect; else { sr.left = sr.top = 0; sr.right = src->width; sr.bottom = src->height; }
    dr.left = x; dr.top = y; dr.right = x + (sr.right - sr.left); dr.bottom = y + (sr.bottom - sr.top);
    return IDirectDrawSurface_Blt_c(lpThis, &dr, src, &sr, (dwTrans & DDBLTFAST_SRCCOLORKEY) ? DDBLT_KEYSRC : 0, NULL);
}

uint32_t CCALL IDirectDrawSurface_DeleteAttachedSurface_c(dd_surface *lpThis, uint32_t f, dd_surface *s) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDrawSurface_EnumAttachedSurfaces_c(dd_surface *lpThis, void *c, void *cb) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDrawSurface_EnumOverlayZOrders_c(dd_surface *lpThis, uint32_t f, void *c, void *cb) { return DDERR_UNSUPPORTED; }

uint32_t CCALL IDirectDrawSurface_Flip_c(dd_surface *lpThis, dd_surface *target, uint32_t dwFlags)
{
    // copies instead of swapping buffers: the game and the shell keep lpSurface pointers across locks
    dd_surface *back = (target != NULL) ? target : lpThis->back;
    if (back == NULL) return DDERR_NOTFOUND;
    if ((back->pitch == lpThis->pitch) && (back->height == lpThis->height)) memcpy(lpThis->pixels, back->pixels, (size_t)lpThis->pitch * lpThis->height);
    after_write(lpThis);
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface_GetAttachedSurface_c(dd_surface *lpThis, const uint32_t *caps, PTR32(dd_surface) *s)
{
    if (s == NULL) return DDERR_INVALIDPARAMS;
    *s = NULL;
    if ((lpThis->back != NULL) && (caps != NULL) && (caps[0] & DDSCAPS_BACKBUFFER))
    {
        lpThis->back->refs++;
        *s = lpThis->back;
        return DD_OK;
    }
    return DDERR_NOTFOUND;
}

uint32_t CCALL IDirectDrawSurface_GetBltStatus_c(dd_surface *lpThis, uint32_t f) { return DD_OK; }
uint32_t CCALL IDirectDrawSurface_GetCaps_c(dd_surface *lpThis, uint32_t *caps) { if (caps) caps[0] = lpThis->caps; return DD_OK; }
uint32_t CCALL IDirectDrawSurface_GetClipper_c(dd_surface *lpThis, PTR32(dd_clipper) *c) { if (c == NULL) return DDERR_INVALIDPARAMS; *c = lpThis->clipper; if (*c == NULL) return DDERR_NOCLIPPERATTACHED; (*c)->refs++; return DD_OK; }

uint32_t CCALL IDirectDrawSurface_GetColorKey_c(dd_surface *lpThis, uint32_t f, uint32_t *key)
{
    if (!lpThis->has_colorkey) return DDERR_NOCOLORKEY;
    if (key) { key[0] = key[1] = lpThis->colorkey; }
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface_GetDC_c(dd_surface *lpThis, PTR32(void) *hdc)
{
    if (winapi_debug) eprintf("IDirectDrawSurface::GetDC: not supported\n");
    return DDERR_UNSUPPORTED;
}

uint32_t CCALL IDirectDrawSurface_GetFlipStatus_c(dd_surface *lpThis, uint32_t f) { return DD_OK; }
uint32_t CCALL IDirectDrawSurface_GetOverlayPosition_c(dd_surface *lpThis, int32_t *x, int32_t *y) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDrawSurface_GetPalette_c(dd_surface *lpThis, PTR32(dd_palette) *p) { if (p == NULL) return DDERR_INVALIDPARAMS; *p = lpThis->palette; if (*p == NULL) return DDERR_NOPALETTEATTACHED; (*p)->refs++; return DD_OK; }
uint32_t CCALL IDirectDrawSurface_GetPixelFormat_c(dd_surface *lpThis, uint32_t *pf) { if (pf) fill_pixel_format(pf, lpThis->bpp); return DD_OK; }

static void fill_surface_desc(dd_surface *s, uint32_t *desc)
{
    memset(desc + 1, 0, 104);
    desc[0] = 108;
    desc[1] = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_LPSURFACE;
    desc[2] = s->height;
    desc[3] = s->width;
    desc[4] = s->pitch;
    desc[9] = (uint32_t)(uintptr_t)s->pixels;
    if (s->has_colorkey) { desc[1] |= DDSD_CKSRCBLT; desc[16] = desc[17] = s->colorkey; }
    fill_pixel_format(desc + 18, s->bpp);
    desc[26] = s->caps;
}

uint32_t CCALL IDirectDrawSurface_GetSurfaceDesc_c(dd_surface *lpThis, uint32_t *desc)
{
    if (desc == NULL) return DDERR_INVALIDPARAMS;
    fill_surface_desc(lpThis, desc);
    desc[1] &= ~DDSD_LPSURFACE;
    desc[9] = 0;
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface_Initialize_c(dd_surface *lpThis, void *dd, void *desc) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDrawSurface_IsLost_c(dd_surface *lpThis) { return DD_OK; }

uint32_t CCALL IDirectDrawSurface_Lock_c(dd_surface *lpThis, const rect_t *rect, uint32_t *desc, uint32_t dwFlags, void *hEvent)
{
    if (desc == NULL) return DDERR_INVALIDPARAMS;
    fill_surface_desc(lpThis, desc);
    if (rect != NULL) desc[9] += rect->top * lpThis->pitch + rect->left * (lpThis->bpp / 8);
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface_ReleaseDC_c(dd_surface *lpThis, void *hdc) { return DD_OK; }
uint32_t CCALL IDirectDrawSurface_Restore_c(dd_surface *lpThis) { return DD_OK; }

uint32_t CCALL IDirectDrawSurface_SetClipper_c(dd_surface *lpThis, dd_clipper *c)
{
    if (c != NULL) c->refs++;
    lpThis->clipper = c;
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface_SetColorKey_c(dd_surface *lpThis, uint32_t dwFlags, const uint32_t *key)
{
    if (key == NULL) { lpThis->has_colorkey = 0; return DD_OK; }
    if (dwFlags & DDCKEY_SRCBLT)
    {
        lpThis->has_colorkey = 1;
        lpThis->colorkey = key[0];
    }
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface_SetOverlayPosition_c(dd_surface *lpThis, int32_t x, int32_t y) { return DDERR_UNSUPPORTED; }

uint32_t CCALL IDirectDrawSurface_SetPalette_c(dd_surface *lpThis, dd_palette *p)
{
    if (p != NULL) p->refs++;
    lpThis->palette = p;
    if (lpThis->back != NULL) lpThis->back->palette = p;
    after_write(lpThis);
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface_Unlock_c(dd_surface *lpThis, void *ptr)
{
    after_write(lpThis);
    return DD_OK;
}

uint32_t CCALL IDirectDrawSurface_UpdateOverlay_c(dd_surface *lpThis, void *a, dd_surface *b, void *c, uint32_t d, void *e) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDrawSurface_UpdateOverlayDisplay_c(dd_surface *lpThis, uint32_t f) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDrawSurface_UpdateOverlayZOrder_c(dd_surface *lpThis, uint32_t f, dd_surface *s) { return DDERR_UNSUPPORTED; }


/* ------------------------------------------------------------------ */
/* IDirectDrawPalette                                                  */

uint32_t CCALL IDirectDrawPalette_QueryInterface_c(dd_palette *lpThis, uint32_t *riid, PTR32(void) *ppv) { if (ppv) *ppv = NULL; return E_NOINTERFACE; }
uint32_t CCALL IDirectDrawPalette_AddRef_c(dd_palette *lpThis) { return ++lpThis->refs; }

uint32_t CCALL IDirectDrawPalette_Release_c(dd_palette *lpThis)
{
    if (lpThis->refs > 1) return --lpThis->refs;
    // surfaces may still point to it (they don't hold references in this emulation beyond SetPalette)
    return 0;
}

uint32_t CCALL IDirectDrawPalette_GetCaps_c(dd_palette *lpThis, uint32_t *caps) { if (caps) *caps = DDPCAPS_8BIT; return DD_OK; }

uint32_t CCALL IDirectDrawPalette_GetEntries_c(dd_palette *lpThis, uint32_t f, uint32_t base, uint32_t n, uint8_t *entries)
{
    if ((entries == NULL) || (base >= 256)) return DDERR_INVALIDPARAMS;
    if (base + n > 256) n = 256 - base;
    memcpy(entries, lpThis->entries[base], n * 4);
    return DD_OK;
}

uint32_t CCALL IDirectDrawPalette_Initialize_c(dd_palette *lpThis, void *dd, uint32_t f, void *t) { return DDERR_UNSUPPORTED; }

uint32_t CCALL IDirectDrawPalette_SetEntries_c(dd_palette *lpThis, uint32_t f, uint32_t base, uint32_t n, const uint8_t *entries)
{
    if ((entries == NULL) || (base >= 256)) return DDERR_INVALIDPARAMS;
    if (base + n > 256) n = 256 - base;
    memcpy(lpThis->entries[base], entries, n * 4);
    if ((the_dd != NULL) && (the_dd->primary != NULL) && (the_dd->primary->palette == lpThis)) present_primary();
    return DD_OK;
}


/* ------------------------------------------------------------------ */
/* IDirectDrawClipper                                                  */

uint32_t CCALL IDirectDrawClipper_QueryInterface_c(dd_clipper *lpThis, uint32_t *riid, PTR32(void) *ppv) { if (ppv) *ppv = NULL; return E_NOINTERFACE; }
uint32_t CCALL IDirectDrawClipper_AddRef_c(dd_clipper *lpThis) { return ++lpThis->refs; }
uint32_t CCALL IDirectDrawClipper_Release_c(dd_clipper *lpThis) { if (lpThis->refs > 1) return --lpThis->refs; return 0; }
uint32_t CCALL IDirectDrawClipper_GetClipList_c(dd_clipper *lpThis, void *r, void *l, uint32_t *s) { return DDERR_UNSUPPORTED; }
uint32_t CCALL IDirectDrawClipper_GetHWnd_c(dd_clipper *lpThis, PTR32(void) *h) { if (h) *h = lpThis->hwnd; return DD_OK; }
uint32_t CCALL IDirectDrawClipper_Initialize_c(dd_clipper *lpThis, void *dd, uint32_t f) { return DD_OK; }
uint32_t CCALL IDirectDrawClipper_IsClipListChanged_c(dd_clipper *lpThis, uint32_t *b) { if (b) *b = 0; return DD_OK; }
uint32_t CCALL IDirectDrawClipper_SetClipList_c(dd_clipper *lpThis, void *l, uint32_t f) { return DD_OK; }
uint32_t CCALL IDirectDrawClipper_SetHWnd_c(dd_clipper *lpThis, uint32_t f, void *h) { lpThis->hwnd = h; return DD_OK; }

EXTERN_C_END
