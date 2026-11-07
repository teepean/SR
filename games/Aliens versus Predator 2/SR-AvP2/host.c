/**
 *
 *  The window system (host.h): the SDL backend, and the dispatch to the Godot backend (host_godot.cpp).
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL.h>
#include "platform.h"
#include "config.h"
#include "host.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

// host_godot.cpp (only in builds with Godot)
#ifdef HAVE_GODOT
int ghost_load(void);
int ghost_create_window(const char *title, int w, int h);
void ghost_destroy_window(void);
int ghost_window_exists(void);
void ghost_set_title(const char *title);
void ghost_resize(int w, int h);
void ghost_present(const uint32_t *pixels, int w, int h);
void ghost_pump(void);
int ghost_poll_event(SDL_Event *ev);
const uint8_t *ghost_keyboard_state(void);
uint32_t ghost_mouse_state(int *x, int *y);
void ghost_window_size(int *w, int *h);
void ghost_warp_mouse(int wx, int wy);
void ghost_set_relative_mouse(int on);
void ghost_show_cursor(int show);
void ghost_toggle_fullscreen(void);
void ghost_desktop_size(int *w, int *h);
void *ghost_create_cursor(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void *ghost_system_cursor(int id);
void ghost_set_cursor(void *cursor);
void ghost_shutdown(void);
#define GODOT(call) do { if (use_godot) { call; } } while (0)
#define GODOT_RET(call) do { if (use_godot) return call; } while (0)
#else
#define GODOT(call) do { } while (0)
#define GODOT_RET(call) do { } while (0)
#endif

static int use_godot = -1;

int host_select(void)
{
    if (use_godot >= 0) return use_godot;
    use_godot = 0;
#ifdef HAVE_GODOT
    {
        const char *h = getenv("AVP2_HOST");
        if (h == NULL) h = config_get("host");
        if ((h == NULL) || (strcasecmp(h, "sdl") != 0))
        {
            use_godot = ghost_load();
            if (!use_godot) eprintf("host: Godot isn't available, using SDL\n");
        }
    }
#endif
    return use_godot;
}

int host_is_godot(void) { return use_godot > 0; }

/* ------------------------------------------------------------------ */
/* SDL                                                                 */

static SDL_Window *window;

int host_create_window(const char *title, int w, int h)
{
    int scale;

    host_select();
    GODOT_RET(ghost_create_window(title, w, h));
    if (window != NULL) return 1;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0))
    {
        eprintf("Error: SDL video: %s\n", SDL_GetError());
        return 0;
    }
    scale = config_get_int("window_scale", 1);
    if (scale < 1) scale = 1;
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w * scale, h * scale,
                              SDL_WINDOW_RESIZABLE | (config_get_int("fullscreen", 0) ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (window == NULL)
    {
        eprintf("Error: SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }
    return 1;
}

void host_destroy_window(void)
{
    GODOT(ghost_destroy_window(); return);
    if (window != NULL) SDL_DestroyWindow(window);
    window = NULL;
}

int host_window_exists(void)
{
    GODOT_RET(ghost_window_exists());
    return window != NULL;
}

void host_set_title(const char *title)
{
    GODOT(ghost_set_title(title); return);
    if ((window != NULL) && (title != NULL)) SDL_SetWindowTitle(window, title);
}

void host_resize(int w, int h)
{
    int scale;

    GODOT(ghost_resize(w, h); return);
    if ((window == NULL) || (SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) return;
    scale = config_get_int("window_scale", 1);
    if (scale < 1) scale = 1;
    SDL_SetWindowSize(window, w * scale, h * scale);
}

void host_present(const uint32_t *pixels, int w, int h)
{
    SDL_Surface *dst, *src;

    GODOT(ghost_present(pixels, w, h); return);
    if ((window == NULL) || (pixels == NULL)) return;
    dst = SDL_GetWindowSurface(window);
    src = SDL_CreateRGBSurfaceWithFormatFrom((void *)pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_XRGB8888);
    if ((dst != NULL) && (src != NULL))
    {
        SDL_BlitScaled(src, NULL, dst, NULL);
        SDL_UpdateWindowSurface(window);
    }
    if (src != NULL) SDL_FreeSurface(src);
}

void host_pump(void)
{
    GODOT(ghost_pump(); return);
}

int host_vsync(void)
{
    return host_is_godot();
}

int host_poll_event(SDL_Event *ev)
{
    GODOT_RET(ghost_poll_event(ev));
    return SDL_PollEvent(ev);
}

const uint8_t *host_keyboard_state(void)
{
    GODOT_RET(ghost_keyboard_state());
    return SDL_GetKeyboardState(NULL);
}

uint32_t host_mouse_state(int *x, int *y)
{
    GODOT_RET(ghost_mouse_state(x, y));
    return SDL_GetMouseState(x, y);
}

void host_window_size(int *w, int *h)
{
    GODOT(ghost_window_size(w, h); return);
    if (window != NULL) SDL_GetWindowSize(window, w, h);
    else { *w = 0; *h = 0; }
}

void host_warp_mouse(int wx, int wy)
{
    GODOT(ghost_warp_mouse(wx, wy); return);
    if (window != NULL) SDL_WarpMouseInWindow(window, wx, wy);
}

void host_set_relative_mouse(int on)
{
    GODOT(ghost_set_relative_mouse(on); return);
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
}

void host_show_cursor(int show)
{
    GODOT(ghost_show_cursor(show); return);
    SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
}

void host_toggle_fullscreen(void)
{
    GODOT(ghost_toggle_fullscreen(); return);
    if (window != NULL) SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

void host_desktop_size(int *w, int *h)
{
    SDL_DisplayMode mode;

    GODOT(ghost_desktop_size(w, h); return);
    *w = 1920;
    *h = 1080;
    if (SDL_WasInit(SDL_INIT_VIDEO) && (SDL_GetDesktopDisplayMode(0, &mode) == 0))
    {
        *w = mode.w;
        *h = mode.h;
    }
}

void *host_create_cursor(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    SDL_Surface *s;
    SDL_Cursor *c;

    GODOT_RET(ghost_create_cursor(argb, w, h, hot_x, hot_y));
    s = SDL_CreateRGBSurfaceWithFormatFrom((void *)argb, w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s == NULL) return NULL;
    c = SDL_CreateColorCursor(s, hot_x, hot_y);
    SDL_FreeSurface(s);
    return c;
}

void *host_system_cursor(int id)
{
    GODOT_RET(ghost_system_cursor(id));
    return SDL_CreateSystemCursor((SDL_SystemCursor)id);
}

void host_set_cursor(void *cursor)
{
    GODOT(ghost_set_cursor(cursor); return);
    if (cursor != NULL) SDL_SetCursor((SDL_Cursor *)cursor);
}

void host_shutdown(void)
{
    GODOT(ghost_shutdown());
    if (window != NULL) SDL_DestroyWindow(window);
    window = NULL;
    SDL_Quit();
}

EXTERN_C_END
