/**
 *
 *  GDI32 emulation.
 *
 *  The display is a true color device (display.c framebuffer, XRGB8888). 8-bit bitmaps are
 *  converted to RGB when they are drawn to the display, using their color table (DIB_RGB_COLORS)
 *  or the logical palette selected into the device context (DIB_PAL_COLORS).
 *
 *  Text is rendered with FreeType.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "platform.h"
#include "display.h"
#include "vfs.h"
#include "winapi.h"
#include "winapi-gdi32.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)


/* ------------------------------------------------------------------ */
/* objects                                                             */

enum {
    GDI_DC = 0x47440001,
    GDI_BITMAP,
    GDI_PALETTE,
    GDI_FONT,
    GDI_BRUSH,
    GDI_PEN
};

typedef struct {
    uint32_t type;
    int32_t width, height;
    int32_t bpp;
    int32_t pitch;          // bytes between rows (top to bottom), may be negative
    uint8_t *bits;          // first (top) row
    uint8_t *alloc;         // allocated memory (NULL if not owned)
    uint32_t colors[256];   // color table as XRGB
    int32_t ncolors;
    int is_screen;
} gdi_bitmap;

typedef struct {
    uint32_t type;
    int32_t n;
    uint8_t entries[256][4]; // peRed, peGreen, peBlue, peFlags
} gdi_palette;

typedef struct {
    uint32_t type;
    int32_t height;         // LOGFONT lfHeight
    int32_t weight;
    char face[32];
    FT_Face ft;
    int32_t ascent, descent, pixel_size;
} gdi_font;

typedef struct {
    uint32_t type;
    uint32_t color;
    int null;
} gdi_brush;

typedef struct {
    uint32_t type;
    void *hwnd;             // window DC
    gdi_bitmap *bitmap;     // selected bitmap (memory DC)
    gdi_palette *palette;
    gdi_font *font;
    gdi_brush *brush;
    gdi_brush *pen;
    uint32_t text_color, bk_color;
    int32_t bk_mode;        // 1 = TRANSPARENT, 2 = OPAQUE
} gdi_dc;

static gdi_bitmap screen_bitmap;
static gdi_bitmap default_bitmap;
static gdi_palette default_palette;
static gdi_palette *system_palette;     // last palette realized on the display
static gdi_font default_font;
static gdi_brush stock_brushes[6];      // WHITE, LTGRAY, GRAY, DKGRAY, BLACK, NULL
static gdi_brush stock_pens[3];         // WHITE, BLACK, NULL

static uint32_t obj_type(const void *h)
{
    if (h == NULL) return 0;
    return *(const uint32_t *)h;
}

static gdi_bitmap *dc_bitmap(gdi_dc *dc)
{
    if (dc->hwnd != NULL)
    {
        // window DC: the display framebuffer
        screen_bitmap.type = GDI_BITMAP;
        screen_bitmap.width = display_width;
        screen_bitmap.height = display_height;
        screen_bitmap.bpp = 32;
        screen_bitmap.pitch = display_width * 4;
        screen_bitmap.bits = (uint8_t *) display_pixels;
        screen_bitmap.is_screen = 1;
        return (display_pixels != NULL) ? &screen_bitmap : NULL;
    }
    return dc->bitmap;
}

static uint32_t palette_color(const gdi_palette *pal, int index)
{
    const uint8_t *e;
    if (pal == NULL) pal = &default_palette;
    if (index >= pal->n) index = 0;
    e = pal->entries[index & 0xff];
    return ((uint32_t)e[0] << 16) | ((uint32_t)e[1] << 8) | e[2];
}

// COLORREF -> XRGB
static uint32_t colorref_to_rgb(uint32_t cr, const gdi_palette *pal)
{
    if ((cr & 0xff000000) == 0x01000000) return palette_color(pal, cr & 0xffff);   // PALETTEINDEX
    return ((cr & 0xff) << 16) | (cr & 0xff00) | ((cr >> 16) & 0xff);
}

// nearest color index in a color table
static int nearest_index(const gdi_bitmap *bm, uint32_t rgb)
{
    int i, best = 0;
    int bestd = 0x7fffffff;
    int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;

    for (i = 0; i < bm->ncolors; i++)
    {
        uint32_t c = bm->colors[i];
        int dr = r - (int)((c >> 16) & 0xff), dg = g - (int)((c >> 8) & 0xff), db = b - (int)(c & 0xff);
        int d = dr * dr + dg * dg + db * db;
        if (d < bestd)
        {
            bestd = d;
            best = i;
            if (d == 0) break;
        }
    }
    return best;
}

// color value to store into a bitmap of the given format
static uint32_t color_to_pixel(const gdi_bitmap *bm, uint32_t cr, const gdi_palette *pal)
{
    uint32_t rgb;

    if ((bm->bpp == 8) && ((cr & 0xff000000) == 0x01000000)) return cr & 0xff; // PALETTEINDEX on 8-bit bitmap
    rgb = colorref_to_rgb(cr, pal);
    switch (bm->bpp)
    {
        case 8: return (uint32_t) nearest_index(bm, rgb);
        case 16: return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
        default: return rgb;
    }
}

static inline uint32_t get_rgb(const gdi_bitmap *bm, int x, int y)
{
    const uint8_t *row = bm->bits + (intptr_t)y * bm->pitch;
    uint32_t v;
    switch (bm->bpp)
    {
        case 8: return bm->colors[row[x]];
        case 16:
            v = ((const uint16_t *)row)[x];
            return ((v & 0xf800) << 8) | ((v & 0xe000) << 3) | ((v & 0x07e0) << 5) | ((v & 0x0600) >> 1) | ((v & 0x001f) << 3) | ((v & 0x001c) >> 2);
        case 24: return ((uint32_t)row[x * 3 + 2] << 16) | ((uint32_t)row[x * 3 + 1] << 8) | row[x * 3];
        case 32: return ((const uint32_t *)row)[x] & 0xffffff;
    }
    return 0;
}

static inline void put_pixel(gdi_bitmap *bm, int x, int y, uint32_t pixel)
{
    uint8_t *row = bm->bits + (intptr_t)y * bm->pitch;
    switch (bm->bpp)
    {
        case 8: row[x] = (uint8_t) pixel; break;
        case 16: ((uint16_t *)row)[x] = (uint16_t) pixel; break;
        case 24: row[x * 3] = pixel & 0xff; row[x * 3 + 1] = (pixel >> 8) & 0xff; row[x * 3 + 2] = (pixel >> 16) & 0xff; break;
        case 32: ((uint32_t *)row)[x] = pixel; break;
    }
}

static inline uint32_t rgb_to_pixel(const gdi_bitmap *bm, uint32_t rgb)
{
    switch (bm->bpp)
    {
        case 8: return (uint32_t) nearest_index(bm, rgb);
        case 16: return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
        default: return rgb;
    }
}

static void dc_changed(gdi_dc *dc)
{
    if (dc->hwnd != NULL) display_invalidate();
}


/* ------------------------------------------------------------------ */
/* blitting                                                            */

// copies (with nearest-neighbor scaling) src rect to dst rect; clips to both bitmaps
static void blit(gdi_bitmap *dst, int dx, int dy, int dw, int dh, const gdi_bitmap *src, int sx, int sy, int sw, int sh)
{
    int x, y;

    if ((dw <= 0) || (dh <= 0) || (sw <= 0) || (sh <= 0)) return;

    if ((dw == sw) && (dh == sh))
    {
        // clip
        if (dx < 0) { sx -= dx; dw += dx; dx = 0; }
        if (dy < 0) { sy -= dy; dh += dy; dy = 0; }
        if (sx < 0) { dx -= sx; dw += sx; sx = 0; }
        if (sy < 0) { dy -= sy; dh += sy; sy = 0; }
        if (dx + dw > dst->width) dw = dst->width - dx;
        if (dy + dh > dst->height) dh = dst->height - dy;
        if (sx + dw > src->width) dw = src->width - sx;
        if (sy + dh > src->height) dh = src->height - sy;
        if ((dw <= 0) || (dh <= 0)) return;

        if (src->bpp == dst->bpp && (src->bpp != 8 || dst->is_screen == 0))
        {
            int bytes = (src->bpp + 7) / 8;
            for (y = 0; y < dh; y++)
            {
                memmove(dst->bits + (intptr_t)(dy + y) * dst->pitch + dx * bytes, src->bits + (intptr_t)(sy + y) * src->pitch + sx * bytes, dw * bytes);
            }
            return;
        }

        if (dst->bpp == 32 && src->bpp == 8)
        {
            for (y = 0; y < dh; y++)
            {
                const uint8_t *s = src->bits + (intptr_t)(sy + y) * src->pitch + sx;
                uint32_t *d = (uint32_t *)(dst->bits + (intptr_t)(dy + y) * dst->pitch) + dx;
                for (x = 0; x < dw; x++) d[x] = src->colors[s[x]];
            }
            return;
        }

        for (y = 0; y < dh; y++)
        {
            for (x = 0; x < dw; x++) put_pixel(dst, dx + x, dy + y, rgb_to_pixel(dst, get_rgb(src, sx + x, sy + y)));
        }
        return;
    }

    // scaling
    for (y = 0; y < dh; y++)
    {
        int ty = dy + y, fy;
        if (ty < 0 || ty >= dst->height) continue;
        fy = sy + (int)((int64_t)y * sh / dh);
        if (fy < 0 || fy >= src->height) continue;
        for (x = 0; x < dw; x++)
        {
            int tx = dx + x, fx;
            if (tx < 0 || tx >= dst->width) continue;
            fx = sx + (int)((int64_t)x * sw / dw);
            if (fx < 0 || fx >= src->width) continue;
            if (src->bpp == 8 && dst->bpp == 8 && !dst->is_screen)
            {
                dst->bits[(intptr_t)ty * dst->pitch + tx] = src->bits[(intptr_t)fy * src->pitch + fx];
            }
            else
            {
                put_pixel(dst, tx, ty, rgb_to_pixel(dst, get_rgb(src, fx, fy)));
            }
        }
    }
}

#define SRCCOPY 0x00CC0020
#define BLACKNESS 0x00000042
#define WHITENESS 0x00FF0062

static void fill_rect(gdi_bitmap *bm, int x0, int y0, int x1, int y1, uint32_t pixel)
{
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bm->width) x1 = bm->width;
    if (y1 > bm->height) y1 = bm->height;
    for (y = y0; y < y1; y++)
    {
        for (x = x0; x < x1; x++) put_pixel(bm, x, y, pixel);
    }
}

uint32_t CCALL BitBlt_c(gdi_dc *hdc, int32_t x, int32_t y, int32_t cx, int32_t cy, gdi_dc *hdcSrc, int32_t x1, int32_t y1, uint32_t rop)
{
    gdi_bitmap *dst, *src;

    if (obj_type(hdc) != GDI_DC) return 0;
    dst = dc_bitmap(hdc);
    if (dst == NULL) return 0;

    if (rop == BLACKNESS || rop == WHITENESS)
    {
        fill_rect(dst, x, y, x + cx, y + cy, rgb_to_pixel(dst, (rop == WHITENESS) ? 0xffffff : 0));
        dc_changed(hdc);
        return 1;
    }

    if (obj_type(hdcSrc) != GDI_DC) return 0;
    src = dc_bitmap(hdcSrc);
    if (src == NULL) return 0;
    if (rop != SRCCOPY && winapi_debug) eprintf("BitBlt: unsupported rop 0x%x\n", rop);

    blit(dst, x, y, cx, cy, src, x1, y1, cx, cy);
    dc_changed(hdc);
    // Win95 returns the number of copied scan lines (i76shell checks BitBlt(...) == height)
    return (cy > 0) ? (uint32_t) cy : 1;
}

uint32_t CCALL StretchBlt_c(gdi_dc *hdcDest, int32_t xDest, int32_t yDest, int32_t wDest, int32_t hDest, gdi_dc *hdcSrc, int32_t xSrc, int32_t ySrc, int32_t wSrc, int32_t hSrc, uint32_t rop)
{
    gdi_bitmap *dst, *src;

    if ((obj_type(hdcDest) != GDI_DC) || (obj_type(hdcSrc) != GDI_DC)) return 0;
    dst = dc_bitmap(hdcDest);
    src = dc_bitmap(hdcSrc);
    if ((dst == NULL) || (src == NULL)) return 0;

    blit(dst, xDest, yDest, wDest, hDest, src, xSrc, ySrc, wSrc, hSrc);
    dc_changed(hdcDest);
    return (hDest > 0) ? (uint32_t) hDest : 1;
}

typedef struct {
    uint32_t biSize;
    int32_t biWidth;
    int32_t biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t biXPelsPerMeter;
    int32_t biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} bitmapinfoheader;

// fills bitmap description (not the bits) from BITMAPINFO; usage 0 = DIB_RGB_COLORS, 1 = DIB_PAL_COLORS
static int bitmap_from_info(gdi_bitmap *bm, const bitmapinfoheader *bi, uint32_t usage, const gdi_palette *pal)
{
    const uint8_t *table;
    int i, n;

    memset(bm, 0, sizeof(gdi_bitmap));
    bm->type = GDI_BITMAP;
    bm->width = bi->biWidth;
    bm->height = (bi->biHeight < 0) ? -bi->biHeight : bi->biHeight;
    bm->bpp = bi->biBitCount;
    if ((bm->bpp != 8) && (bm->bpp != 16) && (bm->bpp != 24) && (bm->bpp != 32))
    {
        eprintf("GDI: unsupported bitmap depth %d\n", bm->bpp);
        return 0;
    }
    if (bi->biCompression != 0 && !(bi->biCompression == 3 && bm->bpp >= 16))
    {
        eprintf("GDI: unsupported bitmap compression %u\n", bi->biCompression);
        return 0;
    }
    bm->pitch = ((bm->width * bm->bpp + 31) / 32) * 4;

    if (bm->bpp == 8)
    {
        n = (bi->biClrUsed != 0) ? (int)bi->biClrUsed : 256;
        if (n > 256) n = 256;
        bm->ncolors = n;
        table = (const uint8_t *)bi + bi->biSize;
        for (i = 0; i < n; i++)
        {
            if (usage == 1)
            {
                bm->colors[i] = palette_color(pal, ((const uint16_t *)table)[i]);
            }
            else
            {
                bm->colors[i] = ((uint32_t)table[i * 4 + 2] << 16) | ((uint32_t)table[i * 4 + 1] << 8) | table[i * 4];
            }
        }
    }
    return 1;
}

static void set_bits(gdi_bitmap *bm, uint8_t *bits, int bottom_up)
{
    if (bottom_up)
    {
        bm->bits = bits + (intptr_t)(bm->height - 1) * bm->pitch;
        bm->pitch = -bm->pitch;
    }
    else
    {
        bm->bits = bits;
    }
}

int32_t CCALL SetDIBitsToDevice_c(gdi_dc *hdc, int32_t xDest, int32_t yDest, uint32_t w, uint32_t h, int32_t xSrc, int32_t ySrc, uint32_t StartScan, uint32_t cLines, const void *lpvBits, const bitmapinfoheader *lpbmi, uint32_t ColorUse)
{
    gdi_bitmap src, *dst;

    if ((obj_type(hdc) != GDI_DC) || (lpvBits == NULL) || (lpbmi == NULL)) return 0;
    dst = dc_bitmap(hdc);
    if (dst == NULL) return 0;
    if (!bitmap_from_info(&src, lpbmi, ColorUse, hdc->palette)) return 0;

    // only complete bitmaps are supported (StartScan = 0, cLines = height)
    set_bits(&src, (uint8_t *) lpvBits, lpbmi->biHeight > 0);
    if (lpbmi->biHeight > 0)
    {
        // bottom-up: ySrc is measured from the bottom
        ySrc = src.height - ySrc - (int32_t)h;
    }

    blit(dst, xDest, yDest, (int)w, (int)h, &src, xSrc, ySrc, (int)w, (int)h);
    dc_changed(hdc);
    return (int32_t) cLines;
}

void * CCALL CreateDIBSection_c(gdi_dc *hdc, const bitmapinfoheader *pbmi, uint32_t usage, void **ppvBits, void *hSection, uint32_t offset)
{
    gdi_bitmap *bm;
    size_t size;

    if (pbmi == NULL) return NULL;
    bm = (gdi_bitmap *) calloc(1, sizeof(gdi_bitmap));
    if (!bitmap_from_info(bm, pbmi, usage, (obj_type(hdc) == GDI_DC) ? hdc->palette : NULL))
    {
        free(bm);
        return NULL;
    }

    size = (size_t) bm->pitch * bm->height;
    bm->alloc = (uint8_t *) calloc(1, size ? size : 4);
    set_bits(bm, bm->alloc, pbmi->biHeight > 0);
    if (ppvBits != NULL) *ppvBits = bm->alloc;

    if (winapi_debug) eprintf("CreateDIBSection: %dx%dx%d usage %u\n", bm->width, bm->height, bm->bpp, usage);
    return bm;
}

typedef struct { uint8_t rgbBlue, rgbGreen, rgbRed, rgbReserved; } rgbquad;

uint32_t CCALL SetDIBColorTable_c(gdi_dc *hdc, uint32_t iStart, uint32_t cEntries, const rgbquad *prgbq)
{
    gdi_bitmap *bm;
    uint32_t i;

    if ((obj_type(hdc) != GDI_DC) || (prgbq == NULL)) return 0;
    bm = hdc->bitmap;
    if ((bm == NULL) || (bm->bpp != 8)) return 0;

    for (i = 0; (i < cEntries) && (iStart + i < 256); i++)
    {
        bm->colors[iStart + i] = ((uint32_t)prgbq[i].rgbRed << 16) | ((uint32_t)prgbq[i].rgbGreen << 8) | prgbq[i].rgbBlue;
    }
    return i;
}


void gdi_blit_rgb(void *hdc, int x, int y, int w, int h, const uint32_t *pixels, int pitch)
{
    gdi_bitmap src, *dst;

    if (obj_type(hdc) != GDI_DC) return;
    dst = dc_bitmap((gdi_dc *) hdc);
    if (dst == NULL) return;

    memset(&src, 0, sizeof(src));
    src.type = GDI_BITMAP;
    src.width = w;
    src.height = h;
    src.bpp = 32;
    src.pitch = pitch * 4;
    src.bits = (uint8_t *) pixels;
    blit(dst, x, y, w, h, &src, 0, 0, w, h);
    dc_changed((gdi_dc *) hdc);
}


/* ------------------------------------------------------------------ */
/* device contexts                                                     */

static gdi_dc *new_dc(void)
{
    gdi_dc *dc = (gdi_dc *) calloc(1, sizeof(gdi_dc));
    dc->type = GDI_DC;
    dc->bitmap = &default_bitmap;
    dc->palette = &default_palette;
    dc->font = &default_font;
    dc->brush = &stock_brushes[0];
    dc->pen = &stock_pens[1];
    dc->text_color = 0;
    dc->bk_color = 0xffffff;
    dc->bk_mode = 2;
    return dc;
}

void * CCALL GetDC_c(void *hWnd)
{
    gdi_dc *dc = new_dc();
    dc->hwnd = (hWnd != NULL) ? hWnd : (void *)1;
    if (system_palette != NULL) dc->palette = system_palette;
    return dc;
}

int32_t CCALL ReleaseDC_c(void *hWnd, void *hDC)
{
    if (obj_type(hDC) != GDI_DC) return 0;
    ((gdi_dc *)hDC)->type = 0;
    free(hDC);
    return 1;
}

void * CCALL CreateCompatibleDC_c(gdi_dc *hdc)
{
    return new_dc();
}

uint32_t CCALL DeleteDC_c(void *hdc)
{
    if (obj_type(hdc) != GDI_DC) return 0;
    ((gdi_dc *)hdc)->type = 0;
    free(hdc);
    return 1;
}

void * CCALL SelectObject_c(gdi_dc *hdc, void *h)
{
    void *old;

    if (obj_type(hdc) != GDI_DC) return NULL;
    switch (obj_type(h))
    {
        case GDI_BITMAP:
            old = hdc->bitmap;
            hdc->bitmap = (gdi_bitmap *) h;
            return old;
        case GDI_FONT:
            old = hdc->font;
            hdc->font = (gdi_font *) h;
            return old;
        case GDI_BRUSH:
            old = hdc->brush;
            hdc->brush = (gdi_brush *) h;
            return old;
        case GDI_PEN:
            old = hdc->pen;
            hdc->pen = (gdi_brush *) h;
            return old;
    }
    return NULL;
}

uint32_t CCALL DeleteObject_c(void *h)
{
    switch (obj_type(h))
    {
        case GDI_BITMAP:
        {
            gdi_bitmap *bm = (gdi_bitmap *) h;
            if ((bm == &default_bitmap) || bm->is_screen) return 1;
            bm->type = 0;
            free(bm->alloc);
            free(bm);
            return 1;
        }
        case GDI_PALETTE:
            if (h == &default_palette) return 1;
            if (h == system_palette) system_palette = NULL;
            ((gdi_palette *)h)->type = 0;
            free(h);
            return 1;
        case GDI_FONT:
            if (h == &default_font) return 1;
            ((gdi_font *)h)->type = 0;
            free(h);
            return 1;
        case GDI_BRUSH:
        case GDI_PEN:
            return 1;   // only stock objects exist
    }
    return 0;
}

void * CCALL GetStockObject_c(int32_t i)
{
    switch (i)
    {
        case 0: case 1: case 2: case 3: case 4: case 5: return &stock_brushes[i];
        case 6: return &stock_pens[0];
        case 7: return &stock_pens[1];
        case 8: return &stock_pens[2];
        case 10: case 12: case 13: case 16: case 17: return &default_font;
        case 15: return &default_palette;
    }
    return NULL;
}

int32_t CCALL GetDeviceCaps_c(gdi_dc *hdc, int32_t index)
{
    switch (index)
    {
        case 8: return display_width;       // HORZRES
        case 10: return display_height;     // VERTRES
        case 12: return 32;                 // BITSPIXEL
        case 14: return 1;                  // PLANES
        case 24: return -1;                 // NUMCOLORS (true color)
        case 38: return 0x7e99;             // RASTERCAPS (no RC_PALETTE)
        case 88: case 90: return 96;        // LOGPIXELSX/Y
        case 104: return 0;                 // SIZEPALETTE
        case 106: return 0;                 // NUMRESERVED
        case 108: return 24;                // COLORRES
    }
    return 0;
}

uint32_t CCALL GdiFlush_c(void)
{
    return 1;
}


/* ------------------------------------------------------------------ */
/* palettes                                                            */

typedef struct {
    uint16_t palVersion;
    uint16_t palNumEntries;
    uint8_t palPalEntry[256][4];
} logpalette;

void * CCALL CreatePalette_c(const logpalette *plpal)
{
    gdi_palette *p;
    int n;

    if (plpal == NULL) return NULL;
    p = (gdi_palette *) calloc(1, sizeof(gdi_palette));
    p->type = GDI_PALETTE;
    n = plpal->palNumEntries;
    if (n > 256) n = 256;
    p->n = n;
    memcpy(p->entries, plpal->palPalEntry, n * 4);
    return p;
}

uint32_t CCALL SetPaletteEntries_c(gdi_palette *hpal, uint32_t iStart, uint32_t cEntries, const uint8_t *pPalEntries)
{
    uint32_t i;
    if ((obj_type(hpal) != GDI_PALETTE) || (pPalEntries == NULL)) return 0;
    for (i = 0; (i < cEntries) && (iStart + i < 256); i++)
    {
        memcpy(hpal->entries[iStart + i], pPalEntries + i * 4, 4);
    }
    if ((int32_t)(iStart + i) > hpal->n) hpal->n = iStart + i;
    return i;
}

void * CCALL SelectPalette_c(gdi_dc *hdc, gdi_palette *hPal, uint32_t bForceBkgd)
{
    void *old;
    if ((obj_type(hdc) != GDI_DC) || (obj_type(hPal) != GDI_PALETTE)) return NULL;
    old = hdc->palette;
    hdc->palette = hPal;
    return old;
}

uint32_t CCALL RealizePalette_c(gdi_dc *hdc)
{
    if (obj_type(hdc) != GDI_DC) return (uint32_t)-1;
    if ((hdc->hwnd != NULL) && (hdc->palette != &default_palette)) system_palette = hdc->palette;
    return (uint32_t) hdc->palette->n;
}

uint32_t CCALL GetSystemPaletteEntries_c(gdi_dc *hdc, uint32_t iStart, uint32_t cEntries, uint8_t *pPalEntries)
{
    const gdi_palette *p = (system_palette != NULL) ? system_palette : &default_palette;
    uint32_t i;

    if (pPalEntries == NULL) return 256;
    for (i = 0; (i < cEntries) && (iStart + i < 256); i++)
    {
        memcpy(pPalEntries + i * 4, p->entries[iStart + i], 4);
        pPalEntries[i * 4 + 3] = 0;
    }
    return i;
}

uint32_t CCALL SetSystemPaletteUse_c(gdi_dc *hdc, uint32_t use)
{
    return 1; // SYSPAL_STATIC
}


/* ------------------------------------------------------------------ */
/* drawing                                                             */

uint32_t CCALL Rectangle_c(gdi_dc *hdc, int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    gdi_bitmap *bm;

    if (obj_type(hdc) != GDI_DC) return 0;
    bm = dc_bitmap(hdc);
    if (bm == NULL) return 0;

    if (!hdc->brush->null)
    {
        fill_rect(bm, left + 1, top + 1, right - 1, bottom - 1, color_to_pixel(bm, hdc->brush->color, hdc->palette));
    }
    if (!hdc->pen->null)
    {
        uint32_t p = color_to_pixel(bm, hdc->pen->color, hdc->palette);
        fill_rect(bm, left, top, right, top + 1, p);
        fill_rect(bm, left, bottom - 1, right, bottom, p);
        fill_rect(bm, left, top, left + 1, bottom, p);
        fill_rect(bm, right - 1, top, right, bottom, p);
    }
    dc_changed(hdc);
    return 1;
}

uint32_t CCALL SetTextColor_c(gdi_dc *hdc, uint32_t color)
{
    uint32_t old;
    if (obj_type(hdc) != GDI_DC) return 0xffffffff;
    old = hdc->text_color;
    hdc->text_color = color;
    return old;
}

uint32_t CCALL SetBkColor_c(gdi_dc *hdc, uint32_t color)
{
    uint32_t old;
    if (obj_type(hdc) != GDI_DC) return 0xffffffff;
    old = hdc->bk_color;
    hdc->bk_color = color;
    return old;
}

int32_t CCALL SetBkMode_c(gdi_dc *hdc, int32_t mode)
{
    int32_t old;
    if (obj_type(hdc) != GDI_DC) return 0;
    old = hdc->bk_mode;
    hdc->bk_mode = mode;
    return old;
}


/* ------------------------------------------------------------------ */
/* fonts                                                               */

static FT_Library ft_library;
static int ft_initialized;

typedef struct {
    char face[32];
    char path[512];
} font_file;

#define MAX_FONT_FILES 16
static font_file font_files[MAX_FONT_FILES];
static int num_font_files;

static void register_font_file(const char *face, const char *path)
{
    int i;
    for (i = 0; i < num_font_files; i++)
    {
        if (0 == strcasecmp(font_files[i].face, face)) break;
    }
    if (i == MAX_FONT_FILES) return;
    if (i == num_font_files) num_font_files++;
    strncpy(font_files[i].face, face, sizeof(font_files[i].face) - 1);
    strncpy(font_files[i].path, path, sizeof(font_files[i].path) - 1);
}

static const char *find_font_file(const char *face, int bold)
{
    static const char *sans[] = {
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL
    };
    static const char *sans_bold[] = {
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        NULL
    };
    static const char *mono[] = {
        "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        NULL
    };
    const char **list;
    int i;

    for (i = 0; i < num_font_files; i++)
    {
        if (0 == strcasecmp(font_files[i].face, face)) return font_files[i].path;
    }

    if (strcasestr(face, "courier") != NULL) list = mono;
    else if (bold || strcasestr(face, "bold") != NULL) list = sans_bold;
    else list = sans;

    for (i = 0; list[i] != NULL; i++)
    {
        if (access(list[i], R_OK) == 0) return list[i];
    }
    return NULL;
}

typedef struct {
    int32_t lfHeight;
    int32_t lfWidth;
    int32_t lfEscapement;
    int32_t lfOrientation;
    int32_t lfWeight;
    uint8_t lfItalic;
    uint8_t lfUnderline;
    uint8_t lfStrikeOut;
    uint8_t lfCharSet;
    uint8_t lfOutPrecision;
    uint8_t lfClipPrecision;
    uint8_t lfQuality;
    uint8_t lfPitchAndFamily;
    char lfFaceName[32];
} logfonta;

static int load_font(gdi_font *f)
{
    const char *path;
    int height;

    if (f->ft != NULL) return 1;
    if (!ft_initialized)
    {
        if (FT_Init_FreeType(&ft_library) != 0) return 0;
        ft_initialized = 1;
    }

    path = find_font_file(f->face, f->weight >= 600);
    if ((path == NULL) || (FT_New_Face(ft_library, path, 0, &f->ft) != 0))
    {
        eprintf("GDI: font not found: %s\n", f->face);
        f->ft = NULL;
        return 0;
    }

    height = f->height;
    if (height == 0) height = -12;
    if (height < 0)
    {
        // character height (em size)
        f->pixel_size = -height;
    }
    else
    {
        // cell height: em size scaled so that ascent + descent == height
        int units = f->ft->ascender - f->ft->descender;
        if (units <= 0) units = f->ft->units_per_EM;
        f->pixel_size = (height * f->ft->units_per_EM + units / 2) / units;
    }
    if (f->pixel_size < 1) f->pixel_size = 1;
    FT_Set_Pixel_Sizes(f->ft, 0, f->pixel_size);
    f->ascent = (int32_t)((f->ft->size->metrics.ascender + 63) >> 6);
    f->descent = (int32_t)((-f->ft->size->metrics.descender + 63) >> 6);

    if (winapi_debug) eprintf("GDI: font %s height %d -> %s, %d px (ascent %d descent %d)\n", f->face, f->height, path, f->pixel_size, f->ascent, f->descent);
    return 1;
}

void * CCALL CreateFontIndirectA_c(const logfonta *lplf)
{
    gdi_font *f;

    if (lplf == NULL) return NULL;
    f = (gdi_font *) calloc(1, sizeof(gdi_font));
    f->type = GDI_FONT;
    f->height = lplf->lfHeight;
    f->weight = lplf->lfWeight;
    strncpy(f->face, lplf->lfFaceName, sizeof(f->face) - 1);
    if (winapi_debug) eprintf("CreateFontIndirectA: %s height %d weight %d\n", f->face, f->height, f->weight);
    return f;
}

uint32_t CCALL SetMapperFlags_c(gdi_dc *hdc, uint32_t flags) { return 0; }

// .fot files created by CreateScalableFontResourceA contain the path of the TrueType file
uint32_t CCALL CreateScalableFontResourceA_c(uint32_t fdwHidden, const char *lpszFont, const char *lpszFile, const char *lpszPath)
{
    char path[1024];
    FILE *f;

    if ((lpszFont == NULL) || (lpszFile == NULL)) return 0;
    vfs_resolve(lpszFont, path, sizeof(path));
    f = fopen(path, "wt");
    if (f == NULL) return 0;
    fprintf(f, "%s\n", lpszFile);
    fclose(f);
    return 1;
}

int32_t CCALL AddFontResourceA_c(const char *lpFileName)
{
    char path[1024], ttf[1024], ttfpath[1024];
    FILE *f;
    FT_Face face;
    size_t len;

    if (lpFileName == NULL) return 0;
    if (!vfs_resolve(lpFileName, path, sizeof(path))) return 0;

    len = strlen(path);
    if ((len > 4) && (0 == strcasecmp(path + len - 4, ".fot")))
    {
        f = fopen(path, "rt");
        if (f == NULL) return 0;
        if (fgets(ttf, sizeof(ttf), f) == NULL) ttf[0] = 0;
        fclose(f);
        len = strlen(ttf);
        while (len && (ttf[len - 1] == '\n' || ttf[len - 1] == '\r')) ttf[--len] = 0;
        if (!vfs_resolve(ttf, ttfpath, sizeof(ttfpath))) return 0;
    }
    else
    {
        strcpy(ttfpath, path);
    }

    if (!ft_initialized)
    {
        if (FT_Init_FreeType(&ft_library) != 0) return 0;
        ft_initialized = 1;
    }
    if (FT_New_Face(ft_library, ttfpath, 0, &face) != 0) return 0;
    if (winapi_debug) eprintf("AddFontResourceA: %s -> %s (%s)\n", lpFileName, ttfpath, face->family_name);
    register_font_file(face->family_name, ttfpath);
    FT_Done_Face(face);
    return 1;
}

uint32_t CCALL RemoveFontResourceA_c(const char *lpFileName) { return 1; }

typedef struct { int32_t cx, cy; } win_size;

static int glyph_advance(gdi_font *f, unsigned char c)
{
    if (FT_Load_Char(f->ft, c, FT_LOAD_DEFAULT | FT_LOAD_TARGET_MONO) != 0) return 0;
    return (int)((f->ft->glyph->advance.x + 32) >> 6);
}

static int text_width(gdi_font *f, const char *s, int n)
{
    int i, w = 0;
    for (i = 0; i < n; i++) w += glyph_advance(f, (unsigned char)s[i]);
    return w;
}

uint32_t CCALL GetTextExtentPoint32A_c(gdi_dc *hdc, const char *lpString, int32_t c, win_size *psizl)
{
    if ((obj_type(hdc) != GDI_DC) || (psizl == NULL)) return 0;
    if (!load_font(hdc->font))
    {
        psizl->cx = c * 8;
        psizl->cy = 16;
        return 1;
    }
    psizl->cx = (lpString != NULL) ? text_width(hdc->font, lpString, c) : 0;
    psizl->cy = hdc->font->ascent + hdc->font->descent;
    return 1;
}

uint32_t CCALL GetTextExtentExPointA_c(gdi_dc *hdc, const char *lpszString, int32_t cchString, int32_t nMaxExtent, int32_t *lpnFit, int32_t *lpnDx, win_size *lpSize)
{
    int i, w = 0, fit = 0;

    if ((obj_type(hdc) != GDI_DC) || !load_font(hdc->font)) return 0;
    for (i = 0; i < cchString; i++)
    {
        w += glyph_advance(hdc->font, (unsigned char)lpszString[i]);
        if (w <= nMaxExtent) fit = i + 1;
        if (lpnDx != NULL) lpnDx[i] = w;
    }
    if (lpnFit != NULL) *lpnFit = fit;
    if (lpSize != NULL)
    {
        lpSize->cx = w;
        lpSize->cy = hdc->font->ascent + hdc->font->descent;
    }
    return 1;
}

uint32_t CCALL TextOutA_c(gdi_dc *hdc, int32_t x, int32_t y, const char *lpString, int32_t c)
{
    gdi_bitmap *bm;
    gdi_font *f;
    uint32_t fg;
    int i, pen_x;

    if ((obj_type(hdc) != GDI_DC) || (lpString == NULL)) return 0;
    bm = dc_bitmap(hdc);
    if ((bm == NULL) || !load_font(hdc->font)) return 0;
    f = hdc->font;

    fg = color_to_pixel(bm, hdc->text_color, hdc->palette);

    if (hdc->bk_mode == 2)
    {
        fill_rect(bm, x, y, x + text_width(f, lpString, c), y + f->ascent + f->descent, color_to_pixel(bm, hdc->bk_color, hdc->palette));
    }

    pen_x = x;
    for (i = 0; i < c; i++)
    {
        FT_GlyphSlot g;
        int gx, gy, bx, by;

        if (FT_Load_Char(f->ft, (unsigned char)lpString[i], FT_LOAD_RENDER | FT_LOAD_TARGET_MONO) != 0) continue;
        g = f->ft->glyph;
        bx = pen_x + g->bitmap_left;
        by = y + f->ascent - g->bitmap_top;

        for (gy = 0; gy < (int)g->bitmap.rows; gy++)
        {
            int py = by + gy;
            const uint8_t *row;
            if (py < 0 || py >= bm->height) continue;
            row = g->bitmap.buffer + gy * g->bitmap.pitch;
            for (gx = 0; gx < (int)g->bitmap.width; gx++)
            {
                int px = bx + gx;
                int on;
                if (px < 0 || px >= bm->width) continue;
                if (g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) on = (row[gx >> 3] >> (7 - (gx & 7))) & 1;
                else on = row[gx] >= 128;
                if (on) put_pixel(bm, px, py, fg);
            }
        }
        pen_x += (int)((g->advance.x + 32) >> 6);
    }

    dc_changed(hdc);
    return 1;
}


/* ------------------------------------------------------------------ */

void winapi_gdi32_init(void)
{
    int i;
    static const uint32_t brush_colors[6] = { 0xffffff, 0xc0c0c0, 0x808080, 0x404040, 0x000000, 0 };

    default_bitmap.type = GDI_BITMAP;
    default_bitmap.width = 1;
    default_bitmap.height = 1;
    default_bitmap.bpp = 32;
    default_bitmap.pitch = 4;
    default_bitmap.bits = (uint8_t *) calloc(1, 4);

    // default palette: the 20 static colors of Windows at the start and the end
    default_palette.type = GDI_PALETTE;
    default_palette.n = 256;
    {
        static const uint8_t first[10][3] = { {0,0,0},{128,0,0},{0,128,0},{128,128,0},{0,0,128},{128,0,128},{0,128,128},{192,192,192},{192,220,192},{166,202,240} };
        static const uint8_t last[10][3] = { {255,251,240},{160,160,164},{128,128,128},{255,0,0},{0,255,0},{255,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,255} };
        for (i = 0; i < 10; i++)
        {
            memcpy(default_palette.entries[i], first[i], 3);
            memcpy(default_palette.entries[246 + i], last[i], 3);
        }
    }

    default_font.type = GDI_FONT;
    default_font.height = -12;
    strcpy(default_font.face, "System");

    for (i = 0; i < 6; i++)
    {
        stock_brushes[i].type = GDI_BRUSH;
        stock_brushes[i].color = ((brush_colors[i] & 0xff) << 16) | (brush_colors[i] & 0xff00) | ((brush_colors[i] >> 16) & 0xff);
        stock_brushes[i].null = (i == 5);
    }
    for (i = 0; i < 3; i++)
    {
        stock_pens[i].type = GDI_PEN;
        stock_pens[i].color = (i == 0) ? 0xffffff : 0;
        stock_pens[i].null = (i == 2);
    }
}

EXTERN_C_END
