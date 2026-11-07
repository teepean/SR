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
#include <unistd.h>
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
#include "host.h"
#include "peimage.h"
#include "vfs.h"
#include "game.h"
#include "rezfs.h"
#include "music.h"

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
    char host[1024];
    return vfs_resolve(name, host, sizeof(host));
}

// "avp2:<name>": <name> in AvP2's install (avp2_dir) as a path the game can use: relative to the game directory
// (the virtual file system maps every path into it; an absolute directory becomes ../.. up to the root)
static void avp2_path(const char *name, char *path, size_t size)
{
    const char *dir = config_get("avp2_dir");
    char cwd[1024];
    size_t len;
    const char *p;

    path[0] = 0;
    if ((dir == NULL) || (dir[0] == 0))
    {
        snprintf(path, size, "%s", name);
        return;
    }
    if (dir[0] == '/')
    {
        if (getcwd(cwd, sizeof(cwd)) == NULL) cwd[0] = 0;
        for (p = cwd; *p; p++)
        {
            if ((*p == '/') && (p[1] != 0) && (p[1] != '/')) strncat(path, "..\\", size - strlen(path) - 1);
        }
        dir++;
    }
    len = strlen(path);
    snprintf(path + len, size - len, "%s\\%s", dir, name);
    for (p = path; *p; p++) if (*p == '/') *(char *)p = '\\';
}

static void prepare_command_line(int argc, char *argv[])
{
    char path[1024];
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
        add_arg(config_get("window_title") != NULL ? config_get("window_title") : game.title);
        for (i = 0; game.rez[i] != NULL; i++)
        {
            if (strncmp(game.rez[i], "avp2:", 5) == 0) avp2_path(game.rez[i] + 5, path, sizeof(path));
            else if (strncmp(game.rez[i], "avp2?:", 6) == 0) avp2_path(game.rez[i] + 6, path, sizeof(path));
            else snprintf(path, sizeof(path), "%s", game.rez[i]);
            // case-insensitive game directory: only add the archives that exist (custom is a directory and
            // always added)
            if ((strcmp(path, "custom") == 0) || file_exists(path))
            {
                add_arg("-rez");
                add_arg(path);
            }
            else if (strncmp(game.rez[i], "avp2:", 5) == 0)
            {
                eprintf("Warning: %s not found (setting avp2_dir: Aliens versus Predator 2's directory)\n", path);
            }
        }
    }
    for (i = 1; i < argc; i++) add_arg(argv[i]);
    if (winapi_debug) eprintf("command line: %s\n", command_line);
}

// the archives of the final command line, for the runtime's own reads (rezfs.c)
static void register_archives(void)
{
    const char *p = command_line;
    char tok[1024];
    int next_is_rez = 0;

    while (*p)
    {
        int n = 0, quoted = 0;
        while (*p == ' ') p++;
        if (*p == 0) break;
        if (*p == '"') { quoted = 1; p++; }
        while (*p && (quoted ? (*p != '"') : (*p != ' ')))
        {
            if (n < (int)sizeof(tok) - 1) tok[n++] = *p;
            p++;
        }
        if (quoted && (*p == '"')) p++;
        tok[n] = 0;
        if (next_is_rez) rezfs_add(tok);
        next_is_rez = (strcasecmp(tok, "-rez") == 0);
    }
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

    // the window system: Godot (its window opens now) or SDL
    host_select();

    msvcrt_init();
    winapi_user32_init();
    winapi_gdi32_init();

    prepare_command_line(argc, argv);
    winapi_set_command_line(command_line);
    register_archives();

    // the engine's DirectMusic manager -> music.c
    music_install();

    app_exit(call_game((uint32_t)(uintptr_t) lithtech_start, 0, NULL));
    return 0;
}

EXTERN_C_END
