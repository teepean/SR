/**
 *
 *  Rendering: selects the backend (SR-I76.cfg graphics_api = opengl | d3d11) and forwards render.h calls to it.
 *  Default: Direct3D 11 on Windows, OpenGL elsewhere.
 *
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <SDL.h>
#include "render_backend.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

static const render_backend *backend;

static const render_backend *default_backend(void)
{
    const char *api = config_get("graphics_api");
#ifdef _WIN32
    if ((api != NULL) && ((strcasecmp(api, "opengl") == 0) || (strcasecmp(api, "gl") == 0))) return &render_backend_gl;
    return &render_backend_d3d11;
#else
    (void) api;
    return &render_backend_gl;
#endif
}

static const render_backend *get(void)
{
    if (backend == NULL) backend = default_backend();
    return backend;
}

const char *render_backend_name(void) { return get()->name; }

int render_fallback(void)
{
    // the selected backend couldn't be initialized: use OpenGL instead (the window has to be recreated)
    if (get() == &render_backend_gl) return 0;
    fprintf(stderr, "render: %s not available, using OpenGL\n", get()->name);
    backend = &render_backend_gl;
    return 1;
}

uint32_t render_window_flags(void) { return get()->window_flags(); }
int render_init(struct SDL_Window *window) { return get()->init(window); }
void render_shutdown(void) { get()->shutdown(); }
void render_drawable_size(int *w, int *h) { get()->drawable_size(w, h); }

void render_viewport(int w, int h, int *vx, int *vy, int *vw, int *vh)
{
    int ww, wh;
    get()->drawable_size(&ww, &wh);
    if ((w <= 0) || (h <= 0) || (ww <= 0) || (wh <= 0))
    {
        *vx = *vy = 0; *vw = ww; *vh = wh;
        return;
    }
    if ((int64_t)ww * h > (int64_t)wh * w)
    {
        *vh = wh;
        *vw = (int)((int64_t)wh * w / h);
    }
    else
    {
        *vw = ww;
        *vh = (int)((int64_t)ww * h / w);
    }
    *vx = (ww - *vw) / 2;
    *vy = (wh - *vh) / 2;
}

void render_present_2d(const uint32_t *pixels, int w, int h) { get()->present_2d(pixels, w, h); }
uint32_t *render_read_last(int *w, int *h) { return get()->read_last(w, h); }
int render_glide_open(int width, int height) { return get()->glide_open(width, height); }
void render_glide_close(void) { get()->glide_close(); }
int render_glide_is_open(void) { return get()->glide_is_open(); }
int render_glide_texture_create(int w, int h, const uint32_t *rgba) { return get()->glide_texture_create(w, h, rgba); }
void render_glide_texture_destroy(int texture) { get()->glide_texture_destroy(texture); }
void render_glide_draw(const render_glide_state *state, const render_glide_vertex *vertices, int count, int primitive) { get()->glide_draw(state, vertices, count, primitive); }
void render_glide_clear(uint32_t color, uint8_t alpha, uint16_t depth, int color_mask, int depth_mask) { get()->glide_clear(color, alpha, depth, color_mask, depth_mask); }
void render_glide_swap(void) { get()->glide_swap(); }
void render_glide_refresh(int force) { get()->glide_refresh(force); }
void render_glide_read_565(int buffer, uint16_t *dst, int stride_pixels) { get()->glide_read_565(buffer, dst, stride_pixels); }
void render_glide_write_argb(int buffer, const uint32_t *src) { get()->glide_write_argb(buffer, src); }

#ifdef __cplusplus
}
#endif
