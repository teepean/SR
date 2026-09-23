/**
 *
 *  Interstate '76 (recompiled) - program entry
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "platform.h"
#include "msvcrt.h"
#include "winapi.h"
#include "winapi-gdi32.h"

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
    int i;

    // the software renderer presents through GDI (windowed) unless another renderer is selected
    strcpy(command_line, "/gdi");
    for (i = 1; i < argc; i++)
    {
        if (strlen(command_line) + strlen(argv[i]) + 2 >= sizeof(command_line)) break;
        strcat(command_line, " ");
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

    if (SDL_Init(SDL_INIT_NOPARACHUTE))
    {
        eprintf("Error: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    atexit(SDL_Quit);


    winapi_debug = (getenv("I76_DEBUG") != NULL) ? atoi(getenv("I76_DEBUG")) : 0;

    msvcrt_init();
    winapi_user32_init();
    winapi_gdi32_init();

    prepare_command_line(argc, argv);

    run_static_constructors();

    return WinMain_((void *)0x400000, NULL, command_line, 5); // 5 = SW_SHOW
}
