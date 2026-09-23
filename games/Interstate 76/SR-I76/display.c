/**
 *
 *  Game display: the client area of the game window as a 32-bit framebuffer, presented with SDL.
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
#include "render.h"
#include "platform.h"
#include "winapi.h"

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

int display_width, display_height;
uint32_t *display_pixels;

static SDL_Window *window;
static int renderer_ok;
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

    if (!SDL_WasInit(SDL_INIT_VIDEO))
    {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        {
            eprintf("Error: SDL video: %s\n", SDL_GetError());
            return 0;
        }
    }

    display_width = width;
    display_height = height;
    display_pixels = (uint32_t *) calloc((size_t)width * height, sizeof(uint32_t));

    scale = config_get_int("window_scale", config_get_int("scale", 2));
    if (scale < 1) scale = 1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width * scale, height * scale, SDL_WINDOW_RESIZABLE | render_window_flags() | (config_get_int("fullscreen", 0) ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (window == NULL)
    {
        eprintf("Error: SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }

    if (!render_init(window))
    {
        eprintf("Error: can't initialize the renderer\n");
        return 0;
    }
    renderer_ok = 1;

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
    if (renderer_ok) render_shutdown();
    if (window != NULL) SDL_DestroyWindow(window);
    renderer_ok = 0;
    window = NULL;
}

// tears SDL down explicitly and exits without running atexit handlers:
// SDL_Quit from atexit (called from inside game code) crashed in the video driver
void app_exit(int code)
{
    fflush(NULL);
    display_destroy();
    SDL_Quit();
    _exit(code);
}

// Frame limiter: called instead of GetTickCount by the game's per-frame timer (sub_49C920,
// instruction_replacements.sci). The game logic is frame-rate dependent (physics, AI, weapons, sound)
// and was tuned for ~20 FPS; I76_FPS=<n> sets the limit (default 20, 0 = unlimited).
uint32_t CCALL i76_frame_tick_c(void)
{
    static int fps = -1;
    static double period, last;
    double now;

    if (fps < 0)
    {
        fps = config_get_int("fps", 20);
        if (fps > 0) period = 1000.0 / fps;
    }

    if (fps > 0)
    {
        double target;

        now = SDL_GetPerformanceCounter() * 1000.0 / SDL_GetPerformanceFrequency();
        if (last == 0.0) last = now - period;
        target = last + period;
        if (now < target)
        {
            if (target - now >= 2.0) SDL_Delay((uint32_t)(target - now - 1.0));
            do
            {
                now = SDL_GetPerformanceCounter() * 1000.0 / SDL_GetPerformanceFrequency();
            } while (now < target);
        }
        // don't try to catch up after a stall (loading etc.)
        last = (now - target > period) ? now : target;
    }

    if (winapi_debug)
    {
        static uint32_t count, start;
        uint32_t t = SDL_GetTicks();
        if (count++ == 0) start = t;
        if (t - start >= 5000)
        {
            eprintf("frame rate: %.1f FPS\n", (count - 1) * 1000.0 / (t - start));
            count = 0;
        }
    }

    return winapi_get_ticks();
}

void display_set_title(const char *title)
{
    if ((window != NULL) && (title != NULL)) SDL_SetWindowTitle(window, title);
}

// debugging: I76_DUMP_FRAMES=<existing dir> saves the presented picture (GDI or Glide) every second
static void dump_frame(void)
{
    static int index;
    char name[1024];
    uint32_t *pixels;
    int w, h;
    SDL_Surface *s;

    pixels = render_read_last(&w, &h);
    if (pixels == NULL) return;
    s = SDL_CreateRGBSurfaceWithFormatFrom(pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_XRGB8888);
    if (s != NULL)
    {
        snprintf(name, sizeof(name), "%s/frame%04d.bmp", getenv("I76_DUMP_FRAMES"), index++);
        SDL_SaveBMP(s, name);
        SDL_FreeSurface(s);
    }
    free(pixels);
}

void display_idle(void)
{
    static uint32_t last;
    uint32_t now = SDL_GetTicks();
    if ((getenv("I76_DUMP_FRAMES") != NULL) && renderer_ok && (now - last >= 1000))
    {
        last = now;
        dirty = 1;
        display_present(1);
        dump_frame();
    }
}

void display_invalidate(void)
{
    dirty = 1;
    // present at most every 8 ms from here, the message loop flushes the rest
    display_present(0);
}

void display_present(int force)
{
    uint32_t now;

    if (!renderer_ok) return;
    // while the Glide screen is open it owns the display (like a Voodoo's VGA pass-through)
    if (render_glide_is_open()) { dirty = 0; render_glide_refresh(force); return; }
    if (!dirty && !force) return;

    now = SDL_GetTicks();
    if (!force && (now - last_present < 8)) return;

    render_present_2d(display_pixels, display_width, display_height);

    last_present = now;
    dirty = 0;
}

// window size in points vs. drawable size in pixels (HiDPI)
static void window_scale(float *sx, float *sy)
{
    int ww, wh, dw, dh;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    *sx = (ww > 0) ? (float)dw / ww : 1.0f;
    *sy = (wh > 0) ? (float)dh / wh : 1.0f;
}

void display_window_to_client(int wx, int wy, int *cx, int *cy)
{
    int vx, vy, vw, vh;
    float sx, sy;

    if (!renderer_ok || (vw = 0, render_viewport(display_width, display_height, &vx, &vy, &vw, &vh), vw <= 0) || (vh <= 0))
    {
        *cx = wx;
        *cy = wy;
        return;
    }
    window_scale(&sx, &sy);
    *cx = (int)((wx * sx - vx) * display_width / vw);
    *cy = (int)((wy * sy - vy) * display_height / vh);
}

void display_warp_window(int wx, int wy)
{
    if (window != NULL) SDL_WarpMouseInWindow(window, wx, wy);
}

void display_warp_mouse(int cx, int cy)
{
    int vx, vy, vw, vh;
    float sx, sy;

    if (!renderer_ok) return;
    render_viewport(display_width, display_height, &vx, &vy, &vw, &vh);
    if ((display_width <= 0) || (display_height <= 0)) return;
    window_scale(&sx, &sy);
    SDL_WarpMouseInWindow(window, (int)((vx + (float)cx * vw / display_width) / sx), (int)((vy + (float)cy * vh / display_height) / sy));
}
