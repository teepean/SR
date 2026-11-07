/**
 *
 *  Game display: the client area of the game window as a 32-bit framebuffer, presented with SDL.
 *
 */

#if !defined(_DISPLAY_H_INCLUDED_)
#define _DISPLAY_H_INCLUDED_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// window client area (framebuffer) size
extern int display_width, display_height;
// framebuffer in XRGB8888, display_width * display_height
extern uint32_t *display_pixels;

int display_create(const char *title, int width, int height);
void display_resize(int width, int height);
void display_destroy(void);
void app_exit(int code);
int display_exists(void);

// marks the framebuffer as changed; it's presented by display_present (rate limited)
void display_invalidate(void);
void display_present(int force);
// called from the message loop
void display_idle(void);

// converts window coordinates (SDL events) to client coordinates of the framebuffer
void display_window_to_client(int wx, int wy, int *cx, int *cy);
// moves the mouse cursor to client coordinates
void display_warp_mouse(int cx, int cy);
// moves the mouse cursor to window coordinates (input scripts)
void display_warp_window(int wx, int wy);
void display_set_title(const char *title);

#ifdef __cplusplus
}
#endif

#endif /* _DISPLAY_H_INCLUDED_ */
