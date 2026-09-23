/**
 *
 *  Rendering interface (render.c dispatches to OpenGL 3.3: render_gl.c or Direct3D 11: render_d3d11.c).
 *
 *  The backend owns the window's graphics context and presents either
 *   - the GDI framebuffer (display.c): render_present_2d, or
 *   - the Glide screen (glide.c): the render_glide_* functions draw into an offscreen
 *     "back buffer" at the Glide resolution times the scale factor; render_glide_swap presents it.
 *  Glide semantics (state tracking, texture memory, combine modes) live in glide.c; the backend
 *  gets ready-to-use state (render_glide_state) and raw Glide vertices.
 *
 */

#if !defined(_RENDER_H_INCLUDED_)
#define _RENDER_H_INCLUDED_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

// window flags the backend needs (e.g. SDL_WINDOW_OPENGL)
uint32_t render_window_flags(void);
int render_init(struct SDL_Window *window);
void render_shutdown(void);

// size of the window's drawable area in pixels
void render_drawable_size(int *w, int *h);
// letterboxed viewport of a w x h picture in the window (window pixels)
void render_viewport(int w, int h, int *vx, int *vy, int *vw, int *vh);
// selected backend ("OpenGL", "Direct3D 11"); render_fallback switches to OpenGL (returns 0 if already used)
const char *render_backend_name(void);
int render_fallback(void);

// presents a XRGB8888 picture scaled to the window
void render_present_2d(const uint32_t *pixels, int w, int h);

// reads the last presented picture (XRGB8888, *w x *h, malloc'ed) - for frame dumps
uint32_t *render_read_last(int *w, int *h);


/* ---- Glide ---- */

// vertex as passed to grDrawTriangle (Glide 2.x GrVertex, TMU0 only)
typedef struct {
    float x, y, z;
    float r, g, b;
    float ooz;
    float a;
    float oow;
    float sow, tow, tmu_oow;
} render_glide_vertex;

typedef struct {
    // blending (Glide GR_BLEND_* values), enabled if not (ONE, ZERO)
    int rgb_src, rgb_dst, alpha_src, alpha_dst;
    // depth: Glide GR_DEPTHBUFFER_* mode, GR_CMP_* function, write mask
    int depth_mode, depth_func, depth_mask;
    // color/alpha combine (grColorCombine / grAlphaCombine)
    int cc_function, cc_factor, cc_local, cc_other, cc_invert;
    int ac_function, ac_factor, ac_local, ac_other, ac_invert;
    // texture combine for TMU0 (grTexCombine)
    int tc_rgb_function, tc_rgb_factor, tc_alpha_function, tc_alpha_factor, tc_rgb_invert, tc_alpha_invert;
    uint32_t constant_color;        // ARGB
    int chromakey_enable;
    uint32_t chromakey_value;       // ARGB (alpha ignored)
    int fog_mode;                   // GR_FOG_*
    uint32_t fog_color;             // ARGB
    float fog_table[64];            // 0..1, used for GR_FOG_WITH_TABLE
    // texture: backend handle (0 = none) and how s/t (0..256 range) map to 0..1
    int texture;
    float s_scale, t_scale;
    int filter_min, filter_mag;     // GR_TEXTUREFILTER_*
    int clamp_s, clamp_t;           // GR_TEXTURECLAMP_*
    int mipmap;                     // GR_MIPMAP_*
} render_glide_state;

// opens the Glide screen (width x height Glide pixels)
int render_glide_open(int width, int height);
void render_glide_close(void);
int render_glide_is_open(void);

int render_glide_texture_create(int w, int h, const uint32_t *rgba);   // RGBA8 (r in the lowest byte)
void render_glide_texture_destroy(int texture);

void render_glide_draw(const render_glide_state *state, const render_glide_vertex *vertices, int count, int primitive); // 0 = triangles, 1 = lines, 2 = points
void render_glide_clear(uint32_t color, uint8_t alpha, uint16_t depth, int color_mask, int depth_mask);
void render_glide_swap(void);
// re-presents the front buffer if the game hasn't swapped for a while (or always with force)
void render_glide_refresh(int force);

// linear frame buffer emulation: buffer 0 = front, 1 = back
// reads the buffer at Glide resolution as RGB565
void render_glide_read_565(int buffer, uint16_t *dst, int stride_pixels);
// draws an ARGB8888 picture (Glide resolution) over the buffer; pixels with alpha 0 are left unchanged
void render_glide_write_argb(int buffer, const uint32_t *src);

#ifdef __cplusplus
}
#endif

#endif /* _RENDER_H_INCLUDED_ */
