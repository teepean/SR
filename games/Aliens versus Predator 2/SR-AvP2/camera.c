/**
 *
 *  Wide-screen (step 2): Hor+ in the engine. With a game resolution that isn't 4:3 (game_resolution = 1920x1080) the
 *  game still sets its 4:3 fields of view (90 x 78 degrees for the marine, ...), which d3d.ren and the renderer spread
 *  over the whole screen: everything stretched sideways. The engine's camera FOV is the one place everything takes it
 *  from - the scene the renderer draws, Get3DCameraPt (target brackets, the vision overlays' size), the engine's
 *  culling - so the client interface's SetCameraFOV is wrapped: it stores a FovX whose tangent is widened by the
 *  screen's aspect / (4/3) (the same vertical view, more to the sides), and GetCameraFOV gives the game back its own
 *  value (its zoom works on what it reads: no widening on top of a widening).
 *  The interface (ILTClient, a table of function pointers the engine fills: GetCameraFOV at +0x2a4, SetCameraFOV at
 *  +0x2a8 in both AvP2's and Primal Hunt's lithtech.exe - game.h; the entries hold the recompiled functions) reaches the game through cshell's CreateClientShell, which the
 *  engine gets from cshell's GetClientShellFunctions (looked up with GetProcAddress): that lookup returns a wrapper
 *  which hands out a wrapper of CreateClientShell, and that changes the two entries (if they hold the functions game.h
 *  expects) before cshell gets the interface. SR-AvP2.cfg hor_plus = 0 turns it off.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "platform.h"
#include "winapi.h"
#include "config.h"
#include "game.h"
#include "camera.h"
#include "asmsyms.h"

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

extern int winapi_debug;
uint32_t call_game(uint32_t func, uint32_t nargs, const uint32_t *args);
void ren_screen_size(int *w, int *h);

// the bridges (x64/camera-asm.asm, llasm/runtime-asm.llasm: the arguments on the game's stack -> these C functions)
// are in asmsyms.h

static uint32_t orig_functions, orig_create, orig_get, orig_set;

// tan(FovX / 2) is multiplied by this: the game's screen's aspect over 4:3 (1 for a 4:3 resolution)
float camera_hor_plus(void)
{
    static int enabled = -1;
    int w = 0, h = 0;
    float k;

    if (enabled < 0) enabled = config_get_int("hor_plus", 1);
    if (!enabled || (orig_set == 0)) return 1.0f;
    ren_screen_size(&w, &h);
    if ((w <= 0) || (h <= 0)) return 1.0f;
    k = ((float)w / (float)h) / (4.0f / 3.0f);
    return (fabsf(k - 1.0f) < 0.01f) ? 1.0f : k;
}

static float widen(float fov, float k)
{
    float t;
    if ((k == 1.0f) || !(fov > 0.0f) || (fov >= 3.1f)) return fov;
    t = 2.0f * atanf(tanf(fov * 0.5f) * k);
    return (t < 3.1f) ? t : 3.1f;
}

// SetCameraFOV(HLOCALOBJ, float fovX, float fovY) (cdecl): the arguments on the game's stack
void CCALL avp2_camera_set_c(uint32_t *args)
{
    uint32_t a[3];
    float fx;

    memcpy(a, args, sizeof(a));
    memcpy(&fx, &a[1], 4);
    fx = widen(fx, camera_hor_plus());
    memcpy(&a[1], &fx, 4);
    call_game(orig_set, 3, a);
}

// GetCameraFOV(HLOCALOBJ, float *pX, float *pY) (cdecl)
void CCALL avp2_camera_get_c(uint32_t *args)
{
    uint32_t a[3];
    float k = camera_hor_plus();

    memcpy(a, args, sizeof(a));
    call_game(orig_get, 3, a);
    if ((a[1] != 0) && (k != 1.0f))
    {
        float *px = (float *) G2H(a[1]);
        if ((*px > 0.0f) && (*px < 3.1f)) *px = 2.0f * atanf(tanf(*px * 0.5f) / k);
    }
}

// CreateClientShell(ILTClient *) (cdecl): the interface's camera FOV functions replaced, then cshell's own
uint32_t CCALL avp2_create_client_shell_c(uint32_t *args)
{
    uint32_t de = args[0];
    uint32_t a[1] = { de };

    if ((de != 0) && config_get_int("hor_plus", 1))
    {
        uint32_t *get = (uint32_t *) G2H(de + game.camera_fov[0]), *set = (uint32_t *) G2H(de + game.camera_fov[1]);
        if ((*get != 0) && (*set != 0) && (*get != *set) && (*set != H2G((const void *) avp2_camera_set_asm2c)))
        {
            orig_get = *get;
            orig_set = *set;
            *get = H2G((const void *) avp2_camera_get_asm2c);
            *set = H2G((const void *) avp2_camera_set_asm2c);
            if (winapi_debug) eprintf("camera: Hor+ for wide game resolutions (ILTClient %08x)\n", de);
        }
        else if (winapi_debug) eprintf("camera: ILTClient %08x has %08x / %08x: no Hor+\n", de, *get, *set);
    }
    return call_game(orig_create, 1, a);
}

// GetClientShellFunctions(CreateShellFn *, DeleteShellFn *) (cdecl): cshell's, then its CreateClientShell -> ours
void CCALL avp2_get_client_shell_functions_c(uint32_t *args)
{
    uint32_t a[2] = { args[0], args[1] };

    call_game(orig_functions, 2, a);
    if (a[0] != 0)
    {
        uint32_t *create = (uint32_t *) G2H(a[0]);
        if ((*create != 0) && (*create != H2G((const void *) avp2_create_client_shell_asm2c)))
        {
            orig_create = *create;
            *create = H2G((const void *) avp2_create_client_shell_asm2c);
        }
    }
}

// GetProcAddress: cshell's GetClientShellFunctions -> the wrapper (the engine looks it up when it loads cshell)
void *camera_hook_export(const char *name, void *address)
{
    if ((address == NULL) || (strcmp(name, "GetClientShellFunctions") != 0) || (game.camera_fov[1] == 0)) return address;
    orig_functions = H2G(address);
    return (void *) avp2_get_client_shell_functions_asm2c;
}

#ifdef __cplusplus
}
#endif
