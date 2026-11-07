/**
 *
 *  Game display: the client area of the game window as a 32-bit framebuffer (GDI drawing), presented with
 *  SDL's window surface. Temporary: Godot (LibGodot) takes the window over with the renderer.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL.h>
#include "display.h"
#include "config.h"
#include "platform.h"
#include "winapi.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

int display_width, display_height;
uint32_t *display_pixels;

static SDL_Window *window;
static int dirty;
static uint32_t last_present;

int display_exists(void)
{
    return window != NULL;
}

int display_create(const char *title, int width, int height)
{
    int scale;

    if (window != NULL)
    {
        display_resize(width, height);
        display_set_title(title);
        return 1;
    }
    if (!SDL_WasInit(SDL_INIT_VIDEO) && (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0))
    {
        eprintf("Error: SDL video: %s\n", SDL_GetError());
        return 0;
    }

    display_width = width;
    display_height = height;
    display_pixels = (uint32_t *) calloc((size_t)width * height, sizeof(uint32_t));

    scale = config_get_int("window_scale", 1);
    if (scale < 1) scale = 1;
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width * scale, height * scale,
                              SDL_WINDOW_RESIZABLE | (config_get_int("fullscreen", 0) ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (window == NULL)
    {
        eprintf("Error: SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }
    display_present(1);
    return 1;
}

void display_resize(int width, int height)
{
    if ((width == display_width) && (height == display_height)) return;

    free(display_pixels);
    display_width = width;
    display_height = height;
    display_pixels = (uint32_t *) calloc((size_t)width * height, sizeof(uint32_t));
    dirty = 1;
}

void display_destroy(void)
{
    if (window != NULL) SDL_DestroyWindow(window);
    window = NULL;
}

// tears SDL down explicitly and exits without running atexit handlers
void app_exit(int code)
{
    fflush(NULL);
    display_destroy();
    SDL_Quit();
    _exit(code);
}

void display_set_title(const char *title)
{
    if ((window != NULL) && (title != NULL)) SDL_SetWindowTitle(window, title);
}

// debugging: AVP2_DUMP_FRAMES=<existing dir> saves the framebuffer every second
static void dump_frame(void)
{
    static int index;
    char name[1024];
    SDL_Surface *s;

    if (display_pixels == NULL) return;
    s = SDL_CreateRGBSurfaceWithFormatFrom(display_pixels, display_width, display_height, 32, display_width * 4, SDL_PIXELFORMAT_XRGB8888);
    if (s != NULL)
    {
        snprintf(name, sizeof(name), "%s/frame%04d.bmp", getenv("AVP2_DUMP_FRAMES"), index++);
        SDL_SaveBMP(s, name);
        SDL_FreeSurface(s);
    }
}

void display_idle(void)
{
    static uint32_t last;
    uint32_t now = SDL_GetTicks();

    heap_check_all();
    if ((getenv("AVP2_DUMP_FRAMES") != NULL) && (window != NULL) && (now - last >= 1000))
    {
        last = now;
        dump_frame();
    }
    display_present(0);
}

void display_invalidate(void)
{
    dirty = 1;
    display_present(0);
}

void display_present(int force)
{
    uint32_t now;
    SDL_Surface *dst, *src;

    if ((window == NULL) || (display_pixels == NULL)) return;
    if (!dirty && !force) return;
    now = SDL_GetTicks();
    if (!force && (now - last_present < 8)) return;

    dst = SDL_GetWindowSurface(window);
    src = SDL_CreateRGBSurfaceWithFormatFrom(display_pixels, display_width, display_height, 32, display_width * 4, SDL_PIXELFORMAT_XRGB8888);
    if ((dst != NULL) && (src != NULL))
    {
        SDL_BlitScaled(src, NULL, dst, NULL);
        SDL_UpdateWindowSurface(window);
    }
    if (src != NULL) SDL_FreeSurface(src);

    last_present = now;
    dirty = 0;
}

void display_window_to_client(int wx, int wy, int *cx, int *cy)
{
    int ww, wh;

    if (window == NULL) { *cx = wx; *cy = wy; return; }
    SDL_GetWindowSize(window, &ww, &wh);
    *cx = (ww > 0) ? wx * display_width / ww : wx;
    *cy = (wh > 0) ? wy * display_height / wh : wy;
}

void display_warp_window(int wx, int wy)
{
    if (window != NULL) SDL_WarpMouseInWindow(window, wx, wy);
}

void display_warp_mouse(int cx, int cy)
{
    int ww, wh;

    if ((window == NULL) || (display_width <= 0) || (display_height <= 0)) return;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_WarpMouseInWindow(window, cx * ww / display_width, cy * wh / display_height);
}

EXTERN_C_END
