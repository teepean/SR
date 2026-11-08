/**
 *
 *  The window system under the Win32 emulation: one game window, its events, mouse and cursor.
 *
 *  Two backends: Godot (host_godot.cpp - LibGodot owns the window; the 3D renderer draws into it) and SDL
 *  (host_sdl.c - a plain SDL window, also usable with SDL_VIDEODRIVER=offscreen for tests).
 *  AVP2_HOST=sdl|godot selects one (default: godot when it can be loaded, else sdl).
 *
 *  Events are SDL_Event structures in both backends (user32 and dinput read them): SDL_KEYDOWN/UP (scancode,
 *  mod, repeat), SDL_MOUSEMOTION (x, y in window coordinates, xrel, yrel), SDL_MOUSEBUTTONDOWN/UP, SDL_MOUSEWHEEL,
 *  SDL_WINDOWEVENT (FOCUS_GAINED/LOST, EXPOSED), SDL_QUIT.
 *
 */

#if !defined(_HOST_H_INCLUDED_)
#define _HOST_H_INCLUDED_

#include <stdint.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// backend selection (before the window is created); returns 1 for Godot
int host_select(void);
int host_is_godot(void);

// the game window; the client area shows a w x h framebuffer (scaled to the window)
int host_create_window(const char *title, int w, int h);
void host_destroy_window(void);
int host_window_exists(void);
void host_set_title(const char *title);
// framebuffer size changed (the window keeps its size unless it was the framebuffer's)
void host_resize(int w, int h);
// shows the framebuffer (XRGB8888); with Godot this runs one Godot frame
void host_present(const uint32_t *pixels, int w, int h);
// keeps the window responsive while the game draws nothing (loading)
void host_pump(void);
// 1 if presenting waits for the display (no frame limiter needed)
int host_vsync(void);

int host_poll_event(SDL_Event *ev);
// indexed by SDL_Scancode
const uint8_t *host_keyboard_state(void);
// SDL_BUTTON masks; window coordinates
uint32_t host_mouse_state(int *x, int *y);
void host_window_size(int *w, int *h);
// the game's frame (game_w x game_h) in the window: x, y, w, h in window pixels (ghost_frame_rect)
void host_frame_rect(int game_w, int game_h, float r[4]);
// how the 3D view fills the window: SR-AvP2.cfg aspect
enum { HOST_ASPECT_STRETCH, HOST_ASPECT_KEEP, HOST_ASPECT_WIDE };
int host_aspect_mode(void);
void host_warp_mouse(int wx, int wy);
void host_set_relative_mouse(int on);
void host_show_cursor(int show);
void host_toggle_fullscreen(void);
void host_desktop_size(int *w, int *h);

// cursors: ARGB8888 images, or SDL_SystemCursor ids
void *host_create_cursor(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void *host_system_cursor(int id);
void host_set_cursor(void *cursor);

void host_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* _HOST_H_INCLUDED_ */
