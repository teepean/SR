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

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

int display_width, display_height;
uint32_t *display_pixels;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int dirty;
static uint32_t last_present;

int display_exists(void)
{
    return window != NULL;
}

static int create_texture(void)
{
    if (texture != NULL) SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, display_width, display_height);
    if (texture == NULL)
    {
        eprintf("Error: SDL_CreateTexture: %s\n", SDL_GetError());
        return 0;
    }
    SDL_RenderSetLogicalSize(renderer, display_width, display_height);
    return 1;
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

    scale = (getenv("I76_SCALE") != NULL) ? atoi(getenv("I76_SCALE")) : 2;
    if (scale < 1) scale = 1;

    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width * scale, height * scale, SDL_WINDOW_RESIZABLE);
    if (window == NULL)
    {
        eprintf("Error: SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == NULL) renderer = SDL_CreateRenderer(window, -1, 0);
    if (renderer == NULL)
    {
        eprintf("Error: SDL_CreateRenderer: %s\n", SDL_GetError());
        return 0;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (!create_texture()) return 0;

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
    if (renderer != NULL) create_texture();
    dirty = 1;
}

void display_destroy(void)
{
    if (texture != NULL) SDL_DestroyTexture(texture);
    if (renderer != NULL) SDL_DestroyRenderer(renderer);
    if (window != NULL) SDL_DestroyWindow(window);
    texture = NULL;
    renderer = NULL;
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

void display_set_title(const char *title)
{
    if ((window != NULL) && (title != NULL)) SDL_SetWindowTitle(window, title);
}

void display_idle(void)
{
    static uint32_t last;
    uint32_t now = SDL_GetTicks();
    if ((getenv("I76_DUMP_FRAMES") != NULL) && (now - last >= 1000))
    {
        last = now;
        dirty = 1;
        display_present(1);
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

    if (renderer == NULL) return;
    if (!dirty && !force) return;

    now = SDL_GetTicks();
    if (!force && (now - last_present < 8)) return;

    // debugging: I76_DUMP_FRAMES=<dir> saves the framebuffer every second
    {
        static const char *dump_dir;
        static int dump_checked, dump_index;
        static uint32_t last_dump;

        if (!dump_checked)
        {
            dump_checked = 1;
            dump_dir = getenv("I76_DUMP_FRAMES");
        }
        static int dump_pending;
        dump_pending = 1;
        if ((dump_dir != NULL) && dump_pending && (now - last_dump >= 1000))
        {
            char name[1024];
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(display_pixels, display_width, display_height, 32, display_width * 4, SDL_PIXELFORMAT_XRGB8888);
            last_dump = now;
            if (s != NULL)
            {
                snprintf(name, sizeof(name), "%s/frame%04d.bmp", dump_dir, dump_index++);
                SDL_SaveBMP(s, name);
                SDL_FreeSurface(s);
            }
            dump_pending = 0;
        }
    }

    SDL_UpdateTexture(texture, NULL, display_pixels, display_width * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    last_present = now;
    dirty = 0;
}

void display_window_to_client(int wx, int wy, int *cx, int *cy)
{
    // with SDL_RenderSetLogicalSize, mouse events are already in logical coordinates
    *cx = wx;
    *cy = wy;
}

void display_warp_mouse(int cx, int cy)
{
    int ww, wh;
    float sx, sy;
    SDL_Rect vp;

    if ((window == NULL) || (renderer == NULL)) return;

    SDL_GetWindowSize(window, &ww, &wh);
    SDL_RenderGetViewport(renderer, &vp);
    SDL_RenderGetScale(renderer, &sx, &sy);
    SDL_WarpMouseInWindow(window, (int)((vp.x + cx) * sx), (int)((vp.y + cy) * sy));
}
