/**
 *
 *  Rendering backends (render.c selects one): OpenGL 3.3 (render_gl.c), Direct3D 11 (render_d3d11.c, Windows),
 *  Vulkan (render_vk.c, Linux).
 *
 */

#if !defined(_RENDER_BACKEND_H_INCLUDED_)
#define _RENDER_BACKEND_H_INCLUDED_

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint32_t (*window_flags)(void);
    int (*init)(struct SDL_Window *window);
    void (*shutdown)(void);
    void (*drawable_size)(int *w, int *h);
    void (*present_2d)(const uint32_t *pixels, int w, int h);
    uint32_t *(*read_last)(int *w, int *h);
    int (*glide_open)(int width, int height);
    void (*glide_close)(void);
    int (*glide_is_open)(void);
    int (*glide_texture_create)(int w, int h, const uint32_t *rgba);
    void (*glide_texture_destroy)(int texture);
    void (*glide_draw)(const render_glide_state *state, const render_glide_vertex *vertices, int count, int primitive);
    void (*glide_clear)(uint32_t color, uint8_t alpha, uint16_t depth, int color_mask, int depth_mask);
    void (*glide_swap)(void);
    void (*glide_refresh)(int force);
    void (*glide_read_565)(int buffer, uint16_t *dst, int stride_pixels);
    void (*glide_write_argb)(int buffer, const uint32_t *src);
} render_backend;

// shared helpers (render.c)
int render_glide_target_auto(void);
void render_glide_target_size(int glide_w, int glide_h, int *tw, int *th);
void render_post_settings(int *fxaa, float *sharpen, float *gamma);

extern const render_backend render_backend_gl;
#ifdef _WIN32
extern const render_backend render_backend_d3d11;
#elif defined(HAVE_VULKAN)
extern const render_backend render_backend_vk;
#endif

#ifdef __cplusplus
}
#endif

#endif /* _RENDER_BACKEND_H_INCLUDED_ */
