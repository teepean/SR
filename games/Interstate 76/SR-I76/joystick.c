/**
 *
 *  winmm joystick API (joyGetNumDevs, joyGetDevCapsA, joyGetPosEx, joyGetPos) on SDL2.
 *
 *  Game controllers (SDL_GameController): X/Y = left stick, Z = triggers as one throttle axis
 *  (right trigger = forward), R/U = right stick, buttons A B X Y LB RB Back Start LS RS Guide,
 *  d-pad = POV hat. Other joysticks (wheels, flight sticks): axes 0-5 = X Y Z R U V, up to 32 buttons,
 *  first hat = POV. The game lets the player bind axes and buttons in its controls screen.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include "platform.h"
#include "winapi.h"
#include "config.h"

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#define JOYERR_NOERROR 0
#define MMSYSERR_NODRIVER 6
#define MMSYSERR_INVALPARAM 11
#define JOYERR_PARMS 165
#define JOYERR_UNPLUGGED 167

#define JOYCAPS_HASZ 0x0001
#define JOYCAPS_HASR 0x0002
#define JOYCAPS_HASU 0x0004
#define JOYCAPS_HASV 0x0008
#define JOYCAPS_HASPOV 0x0010
#define JOYCAPS_POV4DIR 0x0020

#define JOY_POVCENTERED 0xFFFF

#define MAX_DEVICES 4

#pragma pack(push, 1)
typedef struct {
    uint16_t wMid, wPid;
    char szPname[32];
    uint32_t wXmin, wXmax, wYmin, wYmax, wZmin, wZmax;
    uint32_t wNumButtons, wPeriodMin, wPeriodMax;
    uint32_t wRmin, wRmax, wUmin, wUmax, wVmin, wVmax;
    uint32_t wCaps, wMaxAxes, wNumAxes, wMaxButtons;
    char szRegKey[32];
    char szOEMVxD[260];
} joycaps_a;        // 404 bytes

typedef struct {
    uint32_t dwSize, dwFlags;
    uint32_t dwXpos, dwYpos, dwZpos, dwRpos, dwUpos, dwVpos;
    uint32_t dwButtons, dwButtonNumber, dwPOV, dwReserved1, dwReserved2;
} joyinfoex;        // 52 bytes

typedef struct {
    uint32_t wXpos, wYpos, wZpos, wButtons;
} joyinfo;
#pragma pack(pop)

typedef struct {
    SDL_Joystick *joystick;
    SDL_GameController *controller;
    int axes, buttons, hats;
} device;

static device devices[MAX_DEVICES];
static int num_devices, initialized;

static int subsystem_ok;

// called at startup: device enumeration can be asynchronous (SDL3/sdl2-compat), so the subsystem is
// started early and the devices are opened when the game first asks
void joystick_startup(void)
{
    if (!config_get_int("joystick", 1)) return;
    // the game checks GetFocus itself; SDL's idea of focus can differ from the window manager's
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
    {
        eprintf("joystick: SDL init failed: %s\n", SDL_GetError());
        return;
    }
    subsystem_ok = 1;
}

static int eligible(int index)
{
    return (getenv("I76_VIRTUAL_JOYSTICK") == NULL) || SDL_JoystickIsVirtual(index);
}

static void close_devices(void)
{
    int i;
    for (i = 0; i < num_devices; i++)
    {
        if (devices[i].controller != NULL) SDL_GameControllerClose(devices[i].controller);
        else if (devices[i].joystick != NULL) SDL_JoystickClose(devices[i].joystick);
    }
    memset(devices, 0, sizeof(devices));
    num_devices = 0;
}

static void open_devices(void)
{
    int i, n;

    close_devices();
    n = SDL_NumJoysticks();
    for (i = 0; (i < n) && (num_devices < MAX_DEVICES); i++)
    {
        device *d = &devices[num_devices];
        if (!eligible(i)) continue;
        if (SDL_IsGameController(i))
        {
            d->controller = SDL_GameControllerOpen(i);
            if (d->controller == NULL) continue;
            d->joystick = SDL_GameControllerGetJoystick(d->controller);
            d->axes = 5;
            d->buttons = 11;
            d->hats = 1;
        }
        else
        {
            d->joystick = SDL_JoystickOpen(i);
            if (d->joystick == NULL) continue;
            d->axes = SDL_JoystickNumAxes(d->joystick);
            if (d->axes > 6) d->axes = 6;
            d->buttons = SDL_JoystickNumButtons(d->joystick);
            if (d->buttons > 32) d->buttons = 32;
            d->hats = SDL_JoystickNumHats(d->joystick);
        }
        if (winapi_debug) eprintf("joystick %d: %s (%s, %d axes, %d buttons, %d hats)\n", num_devices,
                                  d->controller ? SDL_GameControllerName(d->controller) : SDL_JoystickName(d->joystick),
                                  d->controller ? "game controller" : "joystick", d->axes, d->buttons, d->hats);
        num_devices++;
    }
    if (winapi_debug) eprintf("joystick: %d device(s)\n", num_devices);
}

static void init_joysticks(void)
{
    if (initialized) return;
    initialized = 1;
    if (!subsystem_ok) return;
    SDL_PumpEvents();

    // debugging without hardware: a virtual game controller
    if (getenv("I76_VIRTUAL_JOYSTICK") != NULL)
    {
        int vi = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        if (vi < 0) eprintf("joystick: can't attach a virtual joystick: %s\n", SDL_GetError());
    }
    open_devices();
}

// hotplug: SDL can replace a device (e.g. on Linux a controller first opened through evdev is taken over by
// the HIDAPI driver: the first handle is detached and keeps stale values), so the devices are reopened when
// the list changes or a handle is detached
static void check_devices(void)
{
    static uint32_t last;
    uint32_t now;
    int i, n, eligible_count = 0, stale = 0;

    init_joysticks();
    if (!subsystem_ok) return;
    now = SDL_GetTicks();
    if (now - last < 500) return;
    last = now;

    SDL_PumpEvents();
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) if (eligible(i)) eligible_count++;
    if (eligible_count > MAX_DEVICES) eligible_count = MAX_DEVICES;
    for (i = 0; i < num_devices; i++)
    {
        if ((devices[i].joystick == NULL) || !SDL_JoystickGetAttached(devices[i].joystick)) stale = 1;
    }
    if (stale || (eligible_count != num_devices))
    {
        if (winapi_debug) eprintf("joystick: device list changed, reopening\n");
        open_devices();
    }
}

static const char *device_name(const device *d)
{
    const char *name = d->controller ? SDL_GameControllerName(d->controller) : SDL_JoystickName(d->joystick);
    return (name != NULL) ? name : "Joystick";
}

uint32_t CCALL joyGetNumDevs_c(void)
{
    check_devices();
    return num_devices;
}

uint32_t CCALL joyGetDevCapsA_c(uint32_t uJoyID, joycaps_a *pjc, uint32_t cbjc)
{
    device *d;

    check_devices();
    if (winapi_debug >= 2) eprintf("joyGetDevCapsA: %u\n", uJoyID);
    if ((pjc == NULL) || (cbjc < sizeof(joycaps_a))) return MMSYSERR_INVALPARAM;
    if (uJoyID >= (uint32_t)num_devices) return JOYERR_PARMS;
    d = &devices[uJoyID];

    memset(pjc, 0, sizeof(joycaps_a));
    pjc->wMid = 0x045E;
    pjc->wPid = (uint16_t)(0x0100 + uJoyID);
    snprintf(pjc->szPname, sizeof(pjc->szPname), "%.31s", device_name(d));
    pjc->wXmax = pjc->wYmax = pjc->wZmax = pjc->wRmax = pjc->wUmax = pjc->wVmax = 65535;
    pjc->wNumButtons = d->buttons;
    pjc->wMaxButtons = 32;
    pjc->wPeriodMin = 10;
    pjc->wPeriodMax = 1000;
    pjc->wMaxAxes = 6;
    pjc->wNumAxes = (d->axes < 2) ? 2 : d->axes;
    if (d->axes >= 3) pjc->wCaps |= JOYCAPS_HASZ;
    if (d->axes >= 4) pjc->wCaps |= JOYCAPS_HASR;
    if (d->axes >= 5) pjc->wCaps |= JOYCAPS_HASU;
    if (d->axes >= 6) pjc->wCaps |= JOYCAPS_HASV;
    if (d->hats > 0) pjc->wCaps |= JOYCAPS_HASPOV | JOYCAPS_POV4DIR;
    snprintf(pjc->szRegKey, sizeof(pjc->szRegKey), "DINPUT.DLL");
    return JOYERR_NOERROR;
}

// SDL axis (-32768..32767) -> winmm (0..65535)
static uint32_t axis(int16_t v)
{
    return (uint32_t)((int32_t)v + 32768);
}

static uint32_t pov_from_hat(uint8_t hat)
{
    switch (hat)
    {
        case SDL_HAT_UP: return 0;
        case SDL_HAT_RIGHTUP: return 4500;
        case SDL_HAT_RIGHT: return 9000;
        case SDL_HAT_RIGHTDOWN: return 13500;
        case SDL_HAT_DOWN: return 18000;
        case SDL_HAT_LEFTDOWN: return 22500;
        case SDL_HAT_LEFT: return 27000;
        case SDL_HAT_LEFTUP: return 31500;
        default: return JOY_POVCENTERED;
    }
}

static void read_device(device *d, joyinfoex *ji)
{
    int i;

    ji->dwXpos = ji->dwYpos = ji->dwZpos = ji->dwRpos = ji->dwUpos = ji->dwVpos = 32768;
    ji->dwButtons = 0;
    ji->dwButtonNumber = 0;
    ji->dwPOV = JOY_POVCENTERED;

    if (d->controller != NULL)
    {
        static const SDL_GameControllerButton buttons[11] = {
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_BACK,
            SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
            SDL_CONTROLLER_BUTTON_GUIDE
        };
        int up, down, left, right;
        int32_t throttle;

        SDL_GameControllerUpdate();
        ji->dwXpos = axis(SDL_GameControllerGetAxis(d->controller, SDL_CONTROLLER_AXIS_LEFTX));
        ji->dwYpos = axis(SDL_GameControllerGetAxis(d->controller, SDL_CONTROLLER_AXIS_LEFTY));
        // one throttle axis like a joystick's throttle lever: right trigger = forward (low values)
        throttle = 32768 - SDL_GameControllerGetAxis(d->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) + SDL_GameControllerGetAxis(d->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        if (throttle < 0) throttle = 0;
        if (throttle > 65535) throttle = 65535;
        ji->dwZpos = (uint32_t)throttle;
        ji->dwRpos = axis(SDL_GameControllerGetAxis(d->controller, SDL_CONTROLLER_AXIS_RIGHTX));
        ji->dwUpos = axis(SDL_GameControllerGetAxis(d->controller, SDL_CONTROLLER_AXIS_RIGHTY));
        for (i = 0; i < 11; i++)
        {
            if (SDL_GameControllerGetButton(d->controller, buttons[i]))
            {
                ji->dwButtons |= 1u << i;
                ji->dwButtonNumber++;
            }
        }
        up = SDL_GameControllerGetButton(d->controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
        down = SDL_GameControllerGetButton(d->controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        left = SDL_GameControllerGetButton(d->controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        right = SDL_GameControllerGetButton(d->controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        ji->dwPOV = pov_from_hat((up ? SDL_HAT_UP : 0) | (down ? SDL_HAT_DOWN : 0) | (left ? SDL_HAT_LEFT : 0) | (right ? SDL_HAT_RIGHT : 0));
    }
    else
    {
        uint32_t *axes[6];
        axes[0] = &ji->dwXpos; axes[1] = &ji->dwYpos; axes[2] = &ji->dwZpos;
        axes[3] = &ji->dwRpos; axes[4] = &ji->dwUpos; axes[5] = &ji->dwVpos;

        SDL_JoystickUpdate();
        for (i = 0; i < d->axes; i++) *axes[i] = axis(SDL_JoystickGetAxis(d->joystick, i));
        for (i = 0; i < d->buttons; i++)
        {
            if (SDL_JoystickGetButton(d->joystick, i))
            {
                ji->dwButtons |= 1u << i;
                ji->dwButtonNumber++;
            }
        }
        if (d->hats > 0) ji->dwPOV = pov_from_hat(SDL_JoystickGetHat(d->joystick, 0));
    }
}

uint32_t CCALL joyGetPosEx_c(uint32_t uJoyID, joyinfoex *pji)
{
    uint32_t size, flags;

    check_devices();
    if (pji == NULL) return MMSYSERR_INVALPARAM;
    if (uJoyID >= (uint32_t)num_devices) return JOYERR_PARMS;
    if (!SDL_JoystickGetAttached(devices[uJoyID].joystick)) return JOYERR_UNPLUGGED;

    if (winapi_debug >= 2)
    {
        static uint32_t count, last;
        uint32_t now = SDL_GetTicks();
        count++;
        if (now - last >= 5000) { eprintf("joyGetPosEx: %u calls, flags 0x%x\n", count, pji->dwFlags); last = now; count = 0; }
    }
    size = pji->dwSize;
    flags = pji->dwFlags;
    read_device(&devices[uJoyID], pji);
    if (winapi_debug >= 2)
    {
        static uint32_t last;
        uint32_t now = SDL_GetTicks();
        if (now - last >= 500)
        {
            last = now;
            eprintf("joystick %u: x %u y %u z %u r %u u %u buttons 0x%x pov %u\n", uJoyID, pji->dwXpos, pji->dwYpos, pji->dwZpos, pji->dwRpos, pji->dwUpos, pji->dwButtons, pji->dwPOV);
        }
    }
    pji->dwSize = size;
    pji->dwFlags = flags;
    return JOYERR_NOERROR;
}

uint32_t CCALL joyGetPos_c(uint32_t uJoyID, joyinfo *pji)
{
    joyinfoex ji;
    uint32_t res;

    if (pji == NULL) return MMSYSERR_INVALPARAM;
    memset(&ji, 0, sizeof(ji));
    ji.dwSize = sizeof(ji);
    ji.dwFlags = 0xFF;
    res = joyGetPosEx_c(uJoyID, &ji);
    if (res != JOYERR_NOERROR) return res;
    pji->wXpos = ji.dwXpos;
    pji->wYpos = ji.dwYpos;
    pji->wZpos = ji.dwZpos;
    pji->wButtons = ji.dwButtons & 0x0F;
    return JOYERR_NOERROR;
}

// input scripts: drive the virtual controller (I76_VIRTUAL_JOYSTICK=1)
void joystick_script(const char *cmd, int a, int b)
{
    init_joysticks();
    if (0 == strcmp(cmd, "jreattach"))
    {
        // simulates a driver switch: the virtual device is detached and a new one attached
        int i, n = SDL_NumJoysticks();
        for (i = n - 1; i >= 0; i--) if (SDL_JoystickIsVirtual(i)) SDL_JoystickDetachVirtual(i);
        SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        SDL_PumpEvents();
        return;
    }
    if ((num_devices == 0) || (devices[0].joystick == NULL)) return;
    if (0 == strcmp(cmd, "jbutton")) SDL_JoystickSetVirtualButton(devices[0].joystick, a, (Uint8)b);
    else if (0 == strcmp(cmd, "jaxis")) SDL_JoystickSetVirtualAxis(devices[0].joystick, a, (Sint16)b);
    SDL_JoystickUpdate();
}
