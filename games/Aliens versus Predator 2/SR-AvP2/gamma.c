/**
 *
 *  Gamma: the brightness of the whole picture. The original has no such setting in its menus (d3d.ren's console
 *  command "loadgamma <file>" set the video card's gamma ramp; the game's "Gamma" variable is another thing, a texture
 *  of the renderer's), and the game is dark.
 *
 *  Options > Display gets a "Gamma" slider, 50..200 (100: the picture as the game draws it), that takes effect at once:
 *  the 3D view's last pass (render3d.cpp) and the 2D frame (host_godot.cpp) give each colour c^(1 / gamma), as a gamma
 *  ramp. The value is the engine's console variable DisplayGamma (saved in autoexec.cfg with the others); SR-AvP2.cfg
 *  gamma (or AVP2_GAMMA) overrides it at the start.
 *
 *  The slider is cshell's own: CFolderDisplay::Build calls avp2_display_build_c just before its CBaseFolder::Build
 *  (both games' cshell instruction_replacements.sci; camera-asm.asm / runtime-asm.llasm), which makes it with
 *  CBaseFolder::CreateSlider (string ids of the port: gamma_string, for LoadStringA / FormatMessageA), SetSliderRange,
 *  SetNumericDisplay and SetSliderPos and puts it below "Custom performance" with CBaseFolder::AddFixedControl
 *  (global_aliases.sci). Its changes come to a command handler of the port
 *  (CSliderCtrl::m_pCommandHandler: an object in low memory whose SendCommand is avp2_gamma_changed_asm2c), which reads
 *  the slider's position while the slider calls it (no reads of the slider at other times: it's freed at the exit).
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "platform.h"
#include "ptr32.h"
#include "lowobj.h"
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
uint32_t call_game_this(uint32_t func, uint32_t nargs, const uint32_t *args);
float r3d_console_float(const char *name, float def);

#define GAMMA_MIN 50
#define GAMMA_MAX 200
#define GAMMA_STEP 5

// the port's string ids (cres.dll's go up to ~30000)
#define STR_GAMMA 0xfe00
#define STR_GAMMA_HELP 0xfe01

// ILTClient::RunConsoleString (cdecl, the command): the same in both games' lithtech.exe
#define LTCLIENT_RUNCONSOLESTRING 0x240

static float gamma_now = 1.0f;
static int gamma_loaded;
static uint32_t slider;

// the slider's variable (CSliderCtrl::m_pnValue: the folder's UpdateData)
static int32_t slider_value_init;
LOW_SLOT(slider_value)
#define slider_value LOW_OBJ(slider_value)

// the command handler: a C++ object (CLTGUICommandHandler) - its vtable pointer, then the vtable (SendCommand: +4)
typedef struct {
    uint32_t vtable;
    uint32_t functions[2];
} command_handler;
static command_handler handler_init;
LOW_SLOT(handler)
#define handler LOW_OBJ(handler)

static char command_init[64];
LOW_SLOT(command)
#define command LOW_OBJ(command)

static float clamp_gamma(float g)
{
    if (!(g >= GAMMA_MIN / 100.0f)) return (g > 0.0f) ? GAMMA_MIN / 100.0f : 1.0f;
    return (g > GAMMA_MAX / 100.0f) ? GAMMA_MAX / 100.0f : g;
}

static int32_t slider_pos(float g)
{
    int32_t p = (int32_t)lrintf(g * 100.0f / GAMMA_STEP) * GAMMA_STEP;
    return (p < GAMMA_MIN) ? GAMMA_MIN : (p > GAMMA_MAX) ? GAMMA_MAX : p;
}

// the gamma now (1: as the game draws it)
float gamma_value(void)
{
    return gamma_now;
}

// render3d: each frame on the game's thread, once the renderer can read console variables: the saved value
void gamma_frame(void)
{
    const char *cfg;
    float v;

    if (gamma_loaded) return;
    cfg = config_get("gamma");
    if (cfg != NULL)
    {
        gamma_now = clamp_gamma((float)atof(cfg));
        gamma_loaded = 1;
    }
    else
    {
        v = r3d_console_float("DisplayGamma", -1.0f);
        if (isnan(v)) return;   // not yet
        gamma_now = (v > 0.0f) ? clamp_gamma(v) : 1.0f;
        gamma_loaded = 1;
    }
    if (winapi_debug) eprintf("gamma: %.2f%s\n", gamma_now, (cfg != NULL) ? " (SR-AvP2.cfg)" : "");
}

// the slider's text: the port's string ids (LoadStringA, FormatMessageA), NULL for others
const char *gamma_string(uint32_t id)
{
    if (id == STR_GAMMA) return "Gamma";
    if (id == STR_GAMMA_HELP) return "Brightness of the whole picture (100: as the game draws it).";
    return NULL;
}

static void save_console(float g)
{
    uint32_t de = camera_client_interface(), fn, args[1];

    if (de == 0) return;
    fn = *(uint32_t *)G2H(de + LTCLIENT_RUNCONSOLESTRING);
    if (fn == 0) return;
    // "+": a variable the engine saves in autoexec.cfg (as cshell's WriteConsoleFloat)
    snprintf(command, sizeof(command_init), "+DisplayGamma %.2f", g);
    args[0] = H2G(command);
    call_game(fn, 1, args);
}

// below the page's "Custom performance" (CFolderDisplay::Build's CustomPos, the layout's): the page's list has room for
// two lines (Resolution, Performance - a third one scrolls them, without arrows)
#define SLIDER_DX 9
#define SLIDER_DY 60
// eNavGroups kCustomGroup0: the group of "Custom performance"
#define SLIDER_GROUP 3

// cshell's CFolderDisplay::Build (cdecl: the folder; its esp is args + 1: its LTIntPt CustomPos at esp+0xc), before its
// CBaseFolder::Build
void CCALL avp2_display_build_c(uint32_t *args)
{
    uint32_t folder = args[0], s, a[8];
    int32_t x = (int32_t)args[4], y = (int32_t)args[5];

    if (!config_get_int("gamma_slider", 1)) return;
    slider_value = slider_pos(gamma_now);
    a[0] = folder;
    a[1] = STR_GAMMA;
    a[2] = STR_GAMMA_HELP;
    a[3] = (uint32_t) * (int32_t *)cshell_display_kGap;      // the page's column and slider widths (layout.txt)
    a[4] = (uint32_t) * (int32_t *)cshell_display_kWidth;
    a[5] = H2G(&slider_value);
    a[6] = 0;                                               // the small font (as the page's list)
    a[7] = 0;
    s = call_game_this(H2G(cshell_CreateSlider), 8, a);
    if (s == 0)
    {
        eprintf("gamma: no slider (CreateSlider failed)\n");
        return;
    }
    a[0] = s;
    a[1] = GAMMA_MIN;
    a[2] = GAMMA_MAX;
    call_game_this(H2G(cshell_SetSliderRange), 3, a);
    *(int32_t *)G2H(s + game.slider_increment) = GAMMA_STEP;
    a[1] = 1;                                               // the number on the bar
    a[2] = 0;
    a[3] = 0;
    call_game_this(H2G(cshell_SetNumericDisplay), 4, a);
    a[1] = (uint32_t)slider_value;
    call_game_this(H2G(cshell_SetSliderPos), 2, a);
    handler.vtable = H2G(handler.functions);
    handler.functions[0] = 0;
    handler.functions[1] = H2G((const void *)avp2_gamma_changed_asm2c);
    *(uint32_t *)G2H(s + game.slider_handler) = H2G(&handler);
    // CBaseFolder::AddFixedControl(control, group, LTIntPt, selectable)
    a[0] = folder;
    a[1] = s;
    a[2] = SLIDER_GROUP;
    a[3] = (uint32_t)(x + SLIDER_DX);
    a[4] = (uint32_t)(y + SLIDER_DY);
    a[5] = 1;
    call_game_this(H2G(cshell_AddFixedControl), 6, a);
    slider = s;
    if (winapi_debug) eprintf("gamma: slider %08x on the Display page (folder %08x) at %d,%d, %d\n", s, folder, x + SLIDER_DX, y + SLIDER_DY, slider_value);
}

// the slider's CLTGUICommandHandler::SendCommand(command, param1, param2): its position changed (it calls this while
// it exists)
void CCALL avp2_gamma_changed_c(uint32_t *args)
{
    int32_t pos;

    (void)args;
    if (slider == 0) return;
    pos = *(int32_t *)G2H(slider + game.slider_pos);
    if ((pos < GAMMA_MIN) || (pos > GAMMA_MAX)) return;
    slider_value = pos;
    gamma_now = pos / 100.0f;
    gamma_loaded = 1;
    save_console(gamma_now);
    if (winapi_debug) eprintf("gamma: %.2f\n", gamma_now);
}

#ifdef __cplusplus
}
#endif
