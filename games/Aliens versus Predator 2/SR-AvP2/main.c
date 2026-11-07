/**
 *
 *  Aliens versus Predator 2 (recompiled) - program entry
 *
 *  The engine (lithtech.exe) is entered through its own C runtime startup (the PE entry point, aliased
 *  lithtech_start by SRW-lithtech/x64/global_aliases.sci): it reads the command line (GetCommandLineA), runs
 *  the C++ static constructors and calls WinMain, as on Windows.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "platform.h"
#include "sysmem.h"
#include "msvcrt.h"
#include "winapi.h"
#include "winapi-gdi32.h"
#include "config.h"
#include "Game-Memory.h"
#include "ptr32.h"
#include "display.h"
#include "peimage.h"

EXTERN_C_BEGIN

void winapi_user32_init(void);
void winapi_set_command_line(const char *cmdline);

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

/* recompiled lithtech.exe: the C runtime's entry point (called through call_game) */
extern char lithtech_start[];

void CCALL Raw_UnexpectedHandler(int which);

#ifdef __cplusplus
}
#endif


int winapi_debug;

void CCALL Raw_UnexpectedHandler(int which)
{
    static const char *const what[] = { "?", "__CxxFrameHandler", "_except_handler3", "a software interrupt",
                                        "an access to an absolute low address" };
    eprintf("Error: recompiled code reached %s - not supported\n", what[(which >= 1 && which <= 4) ? which : 0]);
    exit(1);
}

static char command_line[1024];

// AVP2.exe (the launcher) starts the engine with the game's resource archives on its command line; without
// -rez / -cmdfile arguments the same command line is built here (the archives that exist, in the launcher's order)
static void add_arg(const char *arg)
{
    if (strlen(command_line) + strlen(arg) + 4 >= sizeof(command_line)) return;
    strcat(command_line, " ");
    if (strchr(arg, ' ') != NULL)
    {
        strcat(command_line, "\"");
        strcat(command_line, arg);
        strcat(command_line, "\"");
    }
    else strcat(command_line, arg);
}

static int file_exists(const char *name)
{
    FILE *f = fopen(name, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

static void prepare_command_line(int argc, char *argv[])
{
    static const char *const rez[] = { "AVP2.REZ", "SOUNDS.REZ", "ALIEN.REZ", "MARINE.REZ", "PREDATOR.REZ", "MULTI.REZ",
                                       "AVP2DLL.REZ", "AVP2L.REZ", "custom", "AVP2P.REZ" };
    int i, launcher = 1;

    // the program name first, as Windows passes it
    strcpy(command_line, "lithtech.exe");
    for (i = 1; i < argc; i++)
    {
        if ((strcasecmp(argv[i], "-rez") == 0) || (strcasecmp(argv[i], "-cmdfile") == 0)) launcher = 0;
    }
    if (launcher)
    {
        add_arg("-windowtitle");
        add_arg(config_get("window_title") != NULL ? config_get("window_title") : "Aliens versus Predator 2");
        for (i = 0; i < (int)(sizeof(rez) / sizeof(rez[0])); i++)
        {
            // case-insensitive game directory: the archives are found by vfs_resolve's lookup too, but only add
            // the ones that exist (custom is a directory and always added)
            if ((strcmp(rez[i], "custom") == 0) || file_exists(rez[i]))
            {
                add_arg("-rez");
                add_arg(rez[i]);
            }
        }
    }
    for (i = 1; i < argc; i++) add_arg(argv[i]);
}

int main(int argc, char *argv[])
{
    // log file (stderr of a Windows GUI program goes nowhere)
    if (getenv("AVP2_LOG") != NULL)
    {
        if (freopen(getenv("AVP2_LOG"), "w", stderr) != NULL) setvbuf(stderr, NULL, _IONBF, 0);
    }
    sys_install_crash_handler();
    // everything the recompiled code can see must be below 2 GB
    if (x86_init_malloc())
    {
        eprintf("Error: can't initialize the low memory allocator\n");
        return 1;
    }
    if (sizeof(PTR32(void)) != 4)
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

    winapi_debug = (getenv("AVP2_DEBUG") != NULL) ? atoi(getenv("AVP2_DEBUG")) : 0;

    // lithtech.exe's image (headers, resources) at its image base: the C runtime and the resource functions read
    // it through GetModuleHandle(NULL), as on Windows (the code is the recompiled one)
    if (pe_image_load("lithtech.exe", 1) != (uint8_t *)(uintptr_t)0x400000)
    {
        eprintf("Error: can't map lithtech.exe at 0x400000 - start the game in its directory\n");
        return 1;
    }

    msvcrt_init();
    winapi_user32_init();
    winapi_gdi32_init();

    prepare_command_line(argc, argv);
    winapi_set_command_line(command_line);

    app_exit(call_game((uint32_t)(uintptr_t) lithtech_start, 0, NULL));
    return 0;
}

EXTERN_C_END
