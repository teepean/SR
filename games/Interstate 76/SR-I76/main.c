/**
 *
 *  Interstate '76 (recompiled) - program entry
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL.h>
#include "platform.h"
#include "msvcrt.h"
#include "winapi.h"
#include "winapi-gdi32.h"
#include "config.h"

#include "display.h"

EXTERN_C_BEGIN

void winapi_user32_init(void);

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

/* recompiled i76.exe */
extern int CCALL WinMain_(void *hInstance, void *hPrevInstance, char *lpCmdLine, int nCmdShow);

/* C++ static constructor table of i76.exe (__xc_a .. __xc_z) */
extern void (CCALL *i76_xc_a[])(void);
extern void (CCALL *i76_xc_z[])(void);

void CCALL Raw_UnexpectedHandler(int which);

#ifdef __cplusplus
}
#endif


int winapi_debug;

void CCALL Raw_UnexpectedHandler(int which)
{
    eprintf("Error: Win32 exception handler called (%s) - exceptions are not supported\n", (which == 1) ? "__CxxFrameHandler" : "_except_handler3");
    exit(1);
}

static void run_static_constructors(void)
{
    void (CCALL **p)(void);

    for (p = i76_xc_a; p < i76_xc_z; p++)
    {
        if (*p != NULL)
        {
            (*p)();
        }
    }
}

static char command_line[256];

static void prepare_command_line(int argc, char *argv[])
{
    int i, hardware = -1;
    const char *renderer;

    for (i = 1; i < argc; i++)
    {
        if ((strcasecmp(argv[i], "/glide") == 0) || (strcasecmp(argv[i], "-glide") == 0)) hardware = 1;
        if ((strcasecmp(argv[i], "/gdi") == 0) || (strcasecmp(argv[i], "-gdi") == 0)) hardware = 0;
    }
    if (hardware == -1)
    {
        // renderer from SR-I76.cfg (default: glide)
        renderer = config_get("renderer");
        hardware = (renderer == NULL) || (strcasecmp(renderer, "software") != 0);
    }

    // the software renderer presents through GDI (windowed); with /glide, /gdi must not be given
    // (it switches video playback to a DirectDraw path)
    strcpy(command_line, hardware ? "/glide" : "/gdi");
    for (i = 1; i < argc; i++)
    {
        if ((strcasecmp(argv[i] + 1, "glide") == 0) || (strcasecmp(argv[i] + 1, "gdi") == 0)) continue;
        if (strlen(command_line) + strlen(argv[i]) + 2 >= sizeof(command_line)) break;
        if (command_line[0] != 0) strcat(command_line, " ");
        strcat(command_line, argv[i]);
    }
}

int main(int argc, char *argv[])
{
    if (sizeof(void *) != 4)
    {
        eprintf("Error: The program wasn't compiled correctly for 32 bits\n");
        return 1;
    }

    // terminate immediately on SIGTERM/SIGINT (SDL would turn them into a quit event)
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

#if defined(__linux__)
    // prefer X11 (XWayland on Wayland desktops): with the 32-bit NVIDIA driver, Wayland EGL isn't available and
    // Mesa's fallback path aborted in libwayland after a mission (SR-I76.cfg: video_driver = x11 | wayland | auto)
    if (getenv("SDL_VIDEODRIVER") == NULL)
    {
        const char *vd = config_get("video_driver");
        if ((vd == NULL) || (strcasecmp(vd, "auto") != 0)) SDL_SetHint(SDL_HINT_VIDEODRIVER, (vd != NULL) ? vd : "x11,wayland");
    }
#endif

    if (SDL_Init(SDL_INIT_NOPARACHUTE))
    {
        eprintf("Error: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }


    winapi_debug = (getenv("I76_DEBUG") != NULL) ? atoi(getenv("I76_DEBUG")) : 0;
    joystick_startup();

    msvcrt_init();
    winapi_user32_init();
    winapi_gdi32_init();

    prepare_command_line(argc, argv);

    run_static_constructors();

    app_exit(WinMain_((void *)0x400000, NULL, command_line, 5)); // 5 = SW_SHOW
    return 0;
}

EXTERN_C_END
