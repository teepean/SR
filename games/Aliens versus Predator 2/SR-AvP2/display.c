/**
 *
 *  Game display: the client area of the game window as a 32-bit framebuffer (GDI drawing and the renderer's 2D),
 *  presented by the window system backend (host.h: Godot or SDL).
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <SDL.h>
#include "sysmem.h"
#include "display.h"
#include "config.h"
#include "platform.h"
#include "winapi.h"
#include "host.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

int display_width, display_height;
uint32_t *display_pixels;

static int created;
static int dirty;
static uint32_t last_present;

int display_exists(void)
{
    return created && host_window_exists();
}

// the frame is in the low 2 GB: the engine gets it as a 32-bit pointer (RenderStruct::LockScreen)
static uint32_t *frame_alloc(int width, int height)
{
    return (uint32_t *) sys_map_low((size_t)width * height * sizeof(uint32_t), -1, 0);
}

static void frame_free(uint32_t *pixels, int width, int height)
{
    if (pixels != NULL) sys_unmap(pixels, (size_t)width * height * sizeof(uint32_t));
}

int display_create(const char *title, int width, int height)
{
    if (created)
    {
        display_resize(width, height);
        display_set_title(title);
        return 1;
    }
    display_width = width;
    display_height = height;
    display_pixels = frame_alloc(width, height);
    if (!host_create_window(title, width, height)) return 0;
    created = 1;
    display_present(1);
    return 1;
}

void display_resize(int width, int height)
{
    if ((width == display_width) && (height == display_height)) return;

    frame_free(display_pixels, display_width, display_height);
    display_width = width;
    display_height = height;
    display_pixels = frame_alloc(width, height);
    host_resize(width, height);
    dirty = 1;
}

void display_destroy(void)
{
    if (created) host_destroy_window();
    created = 0;
}

// tears the window system down explicitly and exits without running atexit handlers
void app_exit(int code)
{
    fflush(NULL);
    display_destroy();
    host_shutdown();
    _exit(code);
}

void display_set_title(const char *title)
{
    if (created && (title != NULL)) host_set_title(title);
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
    if ((getenv("AVP2_DUMP_FRAMES") != NULL) && created && (now - last >= 1000))
    {
        last = now;
        dump_frame();
    }
    display_present(0);
    if (created) host_pump();
}

void display_invalidate(void)
{
    dirty = 1;
    display_present(0);
}

void display_present(int force)
{
    uint32_t now;

    if (!created || (display_pixels == NULL)) return;
    if (!dirty && !force) return;
    now = SDL_GetTicks();
    if (!force && (now - last_present < 8)) return;

    host_present(display_pixels, display_width, display_height);

    last_present = now;
    dirty = 0;
}

// window pixels -> the game's (through the frame's rectangle in the window: host_frame_rect; clamped to the frame)
void display_window_to_client(int wx, int wy, int *cx, int *cy)
{
    float r[4];

    if (!created) { *cx = wx; *cy = wy; return; }
    host_frame_rect(display_width, display_height, r);
    *cx = (r[2] > 0.0f) ? (int)floorf((wx - r[0]) * display_width / r[2]) : wx;
    *cy = (r[3] > 0.0f) ? (int)floorf((wy - r[1]) * display_height / r[3]) : wy;
    if (*cx < 0) *cx = 0; else if ((display_width > 0) && (*cx >= display_width)) *cx = display_width - 1;
    if (*cy < 0) *cy = 0; else if ((display_height > 0) && (*cy >= display_height)) *cy = display_height - 1;
}

void display_warp_window(int wx, int wy)
{
    if (created) host_warp_mouse(wx, wy);
}

void display_warp_mouse(int cx, int cy)
{
    float r[4];

    if (!created || (display_width <= 0) || (display_height <= 0)) return;
    host_frame_rect(display_width, display_height, r);
    host_warp_mouse((int)(r[0] + cx * r[2] / display_width), (int)(r[1] + cy * r[3] / display_height));
}

EXTERN_C_END
