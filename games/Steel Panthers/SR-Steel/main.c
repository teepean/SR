/*
 * Steel Panthers (SSI 1995) - native Linux runtime for the statically recompiled STEEL.EXE v1.1.
 *
 * usage: steelpanthers [--game-dir DIR] [--cd FILE.CUE] [--debug]
 *   The game folder defaults to the current directory, the CD image to the first .cue file in it.
 */
#define _GNU_SOURCE
#include "sp.h"
#include <SDL3/SDL.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>

extern uint8_t stack_start[];
extern uint8_t clib_InitRtns[], clib_CMain[];

_stack *g_stack;
static _stack the_stack;
int sp_debug;
volatile uint32_t Game_TimerTick;
uint32_t Game_TimerRun;
uint32_t X86_InterruptFlag;

static jmp_buf exit_env;
static int exit_code, in_game;

void sp_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void sp_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

void sp_exit(int code)
{
    exit_code = code;
    if (in_game) longjmp(exit_env, 1);
    exit(code);
}

/* called from SR_CheckTimer (calls and backward jumps in the recompiled code) when a tick passed */
void Game_RunTimer(void)
{
    Game_TimerRun = Game_TimerTick;
    video_poll();
    ail_run_timers();
}

/* The game busy-waits (kbhit, PIT, retrace, mouse status). When such a poll repeats quickly
   without anything new, give the CPU back for a moment; timers and events keep running. */
void sp_idle_poll(void)
{
    static uint64_t window_start;
    static uint32_t polls;
    uint64_t t = SDL_GetTicksNS();
    if (t - window_start > 1000000ull) { window_start = t; polls = 0; }
    if (++polls < 64) return;
    polls = 0;
    video_poll();
    ail_run_timers();
    SDL_DelayNS(500000);
    window_start = SDL_GetTicksNS();
}

static Uint64 SDLCALL tick(void *userdata, SDL_TimerID id, Uint64 interval)
{
    (void)userdata; (void)id;
    Game_TimerTick++;
    return interval;
}

/* What Watcom's cstart (0xA0646) would have set up under DOS/4GW. */
static void setup_clib(void)
{
    uint32_t top = H2G(stack_start);
    GVAR32(G_CLIB_STACK_TOP) = top;                  /* stack top */
    GVAR32(G_CLIB_STACK_TOP2) = top;
    GVAR32(G_CLIB_CURBRK) = top;                  /* current break */
    GVAR32(G_CLIB_STACKLOW) = H2G(GDATA(G_BSS_END)) + 0x100;   /* stack low limit: end of BSS */
    GVAR16(G_CLIB_PSP) = 0x24;                 /* PSP selector */
    GVAR8(G_CLIB_OSMAJOR) = 6;                     /* DOS 6.22 */
    GVAR8(G_CLIB_OSMINOR) = 22;
    GVAR8(G_CLIB_EXTENDER) = 1;                     /* extender: DOS/4G */
    GVAR8(G_CLIB_EXTENDER2) = 0;
    GVAR16(G_CLIB_NO87) = 0;                    /* NO87 not set */

    static const char env[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0BLASTER=A220 I7 D1 H5 T6\0\0";
    char *e = (char *)lowmem_alloc(sizeof env);
    memcpy(e, env, sizeof env);
    GVAR32(G_CLIB_ENVP) = H2G(e);               /* environment */
    GVAR16(G_CLIB_ENVSEL) = 0x2c;
    GVAR32(G_CLIB_CMDLINE) = H2G(lowmem_strdup(""));               /* command line */
    GVAR32(G_CLIB_PGMNAME) = H2G(lowmem_strdup("C:\\STEEL.EXE"));  /* program name */
}

static char *find_cue(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *de;
    char *found = NULL;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l > 4 && strcasecmp(de->d_name + l - 4, ".cue") == 0) {
            size_t n = strlen(dir) + l + 2;
            found = (char *)malloc(n);
            snprintf(found, n, "%s/%s", dir, de->d_name);
            break;
        }
    }
    closedir(d);
    return found;
}

int main(int argc, char **argv)
{
    const char *game_dir = NULL, *cue = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc) game_dir = argv[++i];
        else if (!strcmp(argv[i], "--cd") && i + 1 < argc) cue = argv[++i];
        else if (!strcmp(argv[i], "--debug")) sp_debug = 1;
        else { fprintf(stderr, "usage: %s [--game-dir DIR] [--cd FILE.CUE] [--debug]\n", argv[0]); return 2; }
    }
    if (getenv("SP_DEBUG")) sp_debug = 1;
    static char cwd[1024];
    if (!game_dir) game_dir = getcwd(cwd, sizeof cwd);
    if (!cue) cue = find_cue(game_dir);

    char probe[1200];
    snprintf(probe, sizeof probe, "%s/SHP", game_dir);
    if (access(probe, F_OK) != 0) {
        snprintf(probe, sizeof probe, "%s/shp", game_dir);
        if (access(probe, F_OK) != 0) {
            fprintf(stderr, "Steel Panthers game files not found in %s (use --game-dir)\n", game_dir);
            return 1;
        }
    }

    lowmem_init();
    if (!video_init()) return 1;
    dosfs_init(game_dir, cue);
    dosint_init();

    the_stack.esp = (void *)(uintptr_t)H2G(stack_start);
    the_stack.stack_bottom = NULL;
    the_stack.stack_top = stack_start;
    g_stack = &the_stack;
    X86_InterruptFlag = 1;
    setup_clib();
    SDL_AddTimerNS(1000000, tick, NULL);

    in_game = 1;
    if (setjmp(exit_env) == 0) {
        c_call_guest_regs(g_stack, H2G(clib_InitRtns), 255, 0, 0, 0);
        /* main_ scans argv with i <= argc and so reads argv[argc] (NULL); under DOS/4GW address 0
           is readable, here it is not - give the terminating slot an empty string instead */
        uint32_t gargc = GVAR32(G_ARGC), *gargv = (uint32_t *)G2H(GVAR32(G_ARGV));
        if (gargv && gargv[gargc] == 0) gargv[gargc] = H2G(lowmem_strdup(""));
        c_call_guest_regs(g_stack, H2G(clib_CMain), 0, 0, 0, 0);
    }
    in_game = 0;
    video_shutdown();
    SDL_Quit();
    return exit_code;
}
