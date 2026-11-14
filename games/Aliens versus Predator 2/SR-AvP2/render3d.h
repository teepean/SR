/**
 *
 *  The 3D world on Godot (render3d.cpp), for render.c (the d3d.ren replacement). Without Godot (SDL host, or a
 *  build without godot-cpp) these do nothing and RenderScene draws nothing.
 *
 */

#if !defined(_RENDER3D_H_INCLUDED_)
#define _RENDER3D_H_INCLUDED_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// the engine's RenderStruct (its callbacks: GetTexture, ...)
void r3d_set_render_struct(uint8_t *rs);
// a world was bound (MainWorld*, game address); returns the context handle
uint32_t r3d_create_context(uint32_t world);
void r3d_delete_context(uint32_t context);
// draws the scene (SceneDesc*); 0 if nothing was drawn (no Godot, or not the main thread)
int r3d_render_scene(uint8_t *scene);
// after each frame: whether RenderScene drew into it (the 3D picture is hidden otherwise)
void r3d_end_frame(int drawn);
// a texture changed or is released (SharedTexture*)
void r3d_unbind_texture(void *texture);
// the game's screen (render mode) size: the scene rectangle (SceneDesc m_Rect) is in its pixels
void r3d_set_screen_size(int w, int h);
// the renderer starts (after the window): the shaders are made and compiled now (shader preloading)
void r3d_init(void);
// Godot shuts down: the renderer's background work stops first
void r3d_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* _RENDER3D_H_INCLUDED_ */
