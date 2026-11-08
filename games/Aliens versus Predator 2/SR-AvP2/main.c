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
#include <fcntl.h>
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
#include "asmsyms.h"
#include "blob.h"

EXTERN_C_BEGIN

void winapi_user32_init(void);
void winapi_set_command_line(const char *cmdline);

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

/* recompiled lithtech.exe: the C runtime's entry point (called through call_game) */

void CCALL Raw_UnexpectedHandler(int which);

#ifdef __cplusplus
}
#endif


int winapi_debug;
uint8_t *lithtech_image;     // lithtech.exe's headers and resources: GetModuleHandle(NULL)

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
    // (Windows: an absolute directory with a drive letter stays absolute - vfs_resolve keeps it)
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

// SR-AvP2.cfg game_resolution = WxH (e.g. 1920x1080: a wide screen; the game's display menu offers only 4:3 modes):
// the render mode in autoexec.cfg (SCREENWIDTH/SCREENHEIGHT and the game's GameScreenWidth/GameScreenHeight), set
// before the engine reads it. camera.c widens the field of view for it (Hor+).
static void apply_game_resolution(void)
{
    static const char *const keys[4] = { "SCREENWIDTH", "SCREENHEIGHT", "GameScreenWidth", "GameScreenHeight" };
    const char *res = config_get("game_resolution");
    char host[1024], values[4][16], *text = NULL, *out;
    int w, h, i, found[4] = { 0, 0, 0, 0 };
    long len = 0;
    size_t pos = 0, cap;
    FILE *f;

    if ((res == NULL) || (sscanf(res, "%dx%d", &w, &h) != 2) || (w < 320) || (h < 200) || (w > 16384) || (h > 16384)) return;
    snprintf(values[0], sizeof(values[0]), "%d", w);
    snprintf(values[1], sizeof(values[1]), "%d", h);
    snprintf(values[2], sizeof(values[2]), "%d", w);
    snprintf(values[3], sizeof(values[3]), "%d", h);

    if (!vfs_resolve("autoexec.cfg", host, sizeof(host))) snprintf(host, sizeof(host), "autoexec.cfg");
    f = fopen(host, "rb");
    if (f != NULL)
    {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        text = (char *) malloc(len + 1);
        if ((text == NULL) || (fread(text, 1, len, f) != (size_t) len)) len = 0;
        if (text != NULL) text[len] = 0;
        fclose(f);
    }
    cap = (size_t) len + 512;
    out = (char *) malloc(cap);
    if (out == NULL) { free(text); return; }

    // the lines ("KEY" "value"), the four replaced
    for (const char *p = (text != NULL) ? text : ""; *p; )
    {
        const char *e = strchr(p, '\n');
        size_t n = (e != NULL) ? (size_t)(e - p + 1) : strlen(p);
        int replaced = 0;
        for (i = 0; i < 4; i++)
        {
            size_t kl = strlen(keys[i]);
            if ((n > kl + 2) && (p[0] == '"') && (strncasecmp(p + 1, keys[i], kl) == 0) && (p[kl + 1] == '"'))
            {
                pos += (size_t) snprintf(out + pos, cap - pos, "\"%s\" \"%s\"\r\n", keys[i], values[i]);
                found[i] = replaced = 1;
                break;
            }
        }
        if (!replaced)
        {
            memcpy(out + pos, p, n);
            pos += n;
        }
        p += n;
    }
    for (i = 0; i < 4; i++)
        if (!found[i]) pos += (size_t) snprintf(out + pos, cap - pos, "\"%s\" \"%s\"\r\n", keys[i], values[i]);

    f = fopen(host, "wb");
    if (f != NULL)
    {
        fwrite(out, 1, pos, f);
        fclose(f);
        if (winapi_debug) eprintf("game resolution %dx%d (autoexec.cfg)\n", w, h);
    }
    free(out);
    free(text);
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
#ifdef _WIN32
    // files are binary by default (open, fopen): as everywhere else
    _set_fmode(_O_BINARY);
#endif
    // log file (stderr of a Windows GUI program goes nowhere)
    if (getenv("AVP2_LOG") != NULL)
    {
        if (freopen(getenv("AVP2_LOG"), "w", stderr) != NULL) setvbuf(stderr, NULL, _IONBF, 0);
    }
    sys_install_crash_handler();
#ifdef SR_BLOB
    // the recompiled code: its image at its fixed address, before anything else takes low memory
    if (blob_load()) return 1;
#endif
#ifdef PTROFS_64BIT
    // the game's 4 GB window: from just below the program (the recompiled code's data is in it); the llasm data's
    // addresses are set now (ptr_initialize_pointers)
    if (initialize_pointer_offset())
    {
        eprintf("Error: can't find a place for the game's memory below the program\n");
        return 1;
    }
    if (getenv("AVP2_DEBUG") != NULL) eprintf("pointer offset 0x%llx\n", (unsigned long long)pointer_offset);
#endif
    // everything the recompiled code can see must be below 2 GB
    if (x86_init_malloc())
    {
        eprintf("Error: can't initialize the low memory allocator\n");
        sys_dump_low_map();
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
    lithtech_image = pe_image_load("lithtech.exe", 1);
#ifdef PTROFS_64BIT
    if (lithtech_image == NULL)
#else
    if (lithtech_image != (uint8_t *)(uintptr_t)0x400000)
#endif
    {
        eprintf("Error: can't map lithtech.exe at 0x400000 - start the game in its directory\n");
        sys_dump_low_map();
        return 1;
    }

    // the window system: Godot (its window opens now) or SDL
    host_select();

    msvcrt_init();
    winapi_user32_init();
    winapi_gdi32_init();

    apply_game_resolution();
    prepare_command_line(argc, argv);
    winapi_set_command_line(command_line);
    register_archives();

    // the engine's DirectMusic manager -> music.c
    music_install();

    app_exit(call_game(H2G(lithtech_start), 0, NULL));
    return 0;
}

EXTERN_C_END
