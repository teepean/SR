/**
 *
 *  winmm joystick API (joyGetNumDevs, joyGetDevCapsA, joyGetPosEx, joyGetPos) on SDL2.
 *
 *  Game controllers (SDL_GameController): X/Y = left stick, Z = triggers as one throttle axis
 *  (right trigger = forward), R/U = right stick, buttons A B X Y LB RB Back Start LS RS Guide,
 *  d-pad = POV hat. Other joysticks (wheels, flight sticks): axes 0-5 = X Y Z R U V, up to 32 buttons,
 *  first hat = POV. The game lets the player bind axes and buttons in its controls screen.
 *
 *  Backends: SDL (below, default) or on Linux optionally evdev (joystick_evdev.c, reads /dev/input/event*
 *  directly). SR-I76.cfg: joystick_backend = sdl | evdev. joystick_device = <name substring|SDL index>
 *  exposes only matching devices; without it wheels/gamepads are listed first (the game reads joystick 0).
 *  Note: if another program grabs the device (e.g. Wine's winedevice.exe with a controller), neither backend
 *  gets any input beyond the initial state.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <SDL.h>
#include "platform.h"
#include "winapi.h"
#include "config.h"
#include "joystick_backend.h"

EXTERN_C_BEGIN

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

#define MAX_DEVICES JOY_MAX_DEVICES

int joy_debug;
static int use_evdev;
static int merge_devices = 1;       // config "joystick_merge": joystick 0 reports all devices combined

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
    SDL_JoystickType type;
    int axes, buttons, hats;
} device;

static device devices[MAX_DEVICES];
static int num_devices, initialized;

static int subsystem_ok;

// called at startup: device enumeration can be asynchronous (SDL3/sdl2-compat), so the subsystem is
// started early and the devices are opened when the game first asks
void joystick_startup(void)
{
    const char *backend;

    joy_debug = winapi_debug;
    merge_devices = config_get_int("joystick_merge", 1);
    if (!config_get_int("joystick", 1)) return;
#if defined(__linux__)
    backend = config_get("joystick_backend");
    use_evdev = (getenv("I76_VIRTUAL_JOYSTICK") == NULL) && (backend != NULL) && (strcasecmp(backend, "evdev") == 0);
    if (use_evdev) return;
#else
    (void)backend;
#endif
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

static const char *joystick_type_name(SDL_JoystickType type)
{
    switch (type)
    {
        case SDL_JOYSTICK_TYPE_GAMECONTROLLER: return "game controller";
        case SDL_JOYSTICK_TYPE_WHEEL: return "wheel";
        case SDL_JOYSTICK_TYPE_ARCADE_STICK: return "arcade stick";
        case SDL_JOYSTICK_TYPE_FLIGHT_STICK: return "flight stick";
        case SDL_JOYSTICK_TYPE_DANCE_PAD: return "dance pad";
        case SDL_JOYSTICK_TYPE_GUITAR: return "guitar";
        case SDL_JOYSTICK_TYPE_DRUM_KIT: return "drum kit";
        case SDL_JOYSTICK_TYPE_ARCADE_PAD: return "arcade pad";
        case SDL_JOYSTICK_TYPE_THROTTLE: return "throttle";
        default: return "joystick";
    }
}

// devices usable for driving come first, so a wheel/gamepad ends up as winmm joystick 0: the game
// binds its default controls to the first joystick and reads that one in the Control Configuration screen
static int joystick_type_priority(SDL_JoystickType type)
{
    switch (type)
    {
        case SDL_JOYSTICK_TYPE_WHEEL: return 0;
        case SDL_JOYSTICK_TYPE_GAMECONTROLLER: return 1;
        case SDL_JOYSTICK_TYPE_FLIGHT_STICK: return 2;
        case SDL_JOYSTICK_TYPE_ARCADE_STICK: return 3;
        case SDL_JOYSTICK_TYPE_ARCADE_PAD: return 4;
        default: return 5;
    }
}

static int contains_ci(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;

    if (n == 0) return 1;
    for (; *haystack != 0; haystack++)
    {
        for (i = 0; i < n; i++)
        {
            if (haystack[i] == 0) return 0;
            if (tolower((unsigned char)haystack[i]) != tolower((unsigned char)needle[i])) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

// fills order[] with the SDL device indices to expose; joystick_device (name substring or SDL index)
// filters the list (no match = all devices). Returns the count.
static int build_selection(int *order)
{
    const char *sel = config_get("joystick_device");
    int i, n = SDL_NumJoysticks(), count = 0;

    if (n > 64) n = 64;
    for (i = 0; i < n; i++)
    {
        const char *name;
        if (!eligible(i)) continue;
        if ((sel != NULL) && (*sel != 0))
        {
            if (strspn(sel, "0123456789") == strlen(sel)) { if (atoi(sel) != i) continue; }
            else
            {
                name = SDL_JoystickNameForIndex(i);
                if ((name == NULL) || !contains_ci(name, sel)) continue;
            }
        }
        order[count++] = i;
    }
    if ((count == 0) && (sel != NULL) && (*sel != 0))
    {
        if (winapi_debug) eprintf("joystick: joystick_device '%s' matched no device, using all of them\n", sel);
        for (i = 0; (i < n) && (count < 64); i++) if (eligible(i)) order[count++] = i;
    }
    return count;
}

static void open_devices(void)
{
    int order[64], i, j, count;

    close_devices();
    count = build_selection(order);

    // stable sort by type priority (same type keeps the SDL order)
    for (i = 1; i < count; i++)
    {
        int key = order[i];
        int kp = joystick_type_priority(SDL_JoystickGetDeviceType(key));
        for (j = i - 1; (j >= 0) && (joystick_type_priority(SDL_JoystickGetDeviceType(order[j])) > kp); j--)
            order[j + 1] = order[j];
        order[j + 1] = key;
    }

    if (winapi_debug)
    {
        int n = SDL_NumJoysticks();
        for (i = 0; i < n; i++)
            eprintf("joystick: SDL device %d: %s [%s]\n", i,
                    SDL_JoystickNameForIndex(i) ? SDL_JoystickNameForIndex(i) : "?",
                    joystick_type_name(SDL_JoystickGetDeviceType(i)));
    }

    for (i = 0; (i < count) && (num_devices < MAX_DEVICES); i++)
    {
        int index = order[i];
        device *d = &devices[num_devices];
        d->type = SDL_JoystickGetDeviceType(index);
        if (SDL_IsGameController(index))
        {
            d->controller = SDL_GameControllerOpen(index);
            if (d->controller == NULL) continue;
            d->joystick = SDL_GameControllerGetJoystick(d->controller);
            d->axes = 5;
            d->buttons = 11;
            d->hats = 1;
        }
        else
        {
            d->joystick = SDL_JoystickOpen(index);
            if (d->joystick == NULL) continue;
            d->axes = SDL_JoystickNumAxes(d->joystick);
            if (d->axes > 6) d->axes = 6;
            d->buttons = SDL_JoystickNumButtons(d->joystick);
            if (d->buttons > 32) d->buttons = 32;
            d->hats = SDL_JoystickNumHats(d->joystick);
        }
        if (winapi_debug) eprintf("joystick %d: %s (%s, %d axes, %d buttons, %d hats)\n", num_devices,
                                  d->controller ? SDL_GameControllerName(d->controller) : SDL_JoystickName(d->joystick),
                                  d->controller ? "game controller" : joystick_type_name(d->type), d->axes, d->buttons, d->hats);
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
    int i, order[64], expected, stale = 0;

    init_joysticks();
    if (!subsystem_ok) return;
    now = SDL_GetTicks();
    if (now - last < 500) return;
    last = now;

    SDL_PumpEvents();
    expected = build_selection(order);
    if (expected > MAX_DEVICES) expected = MAX_DEVICES;
    for (i = 0; i < num_devices; i++)
    {
        if ((devices[i].joystick == NULL) || !SDL_JoystickGetAttached(devices[i].joystick)) stale = 1;
    }
    if (stale || (expected != num_devices))
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

static int backend_count(void)
{
#if defined(__linux__)
    if (use_evdev) return config_get_int("joystick", 1) ? evdev_joystick_count(SDL_GetTicks()) : 0;
#endif
    check_devices();
    return num_devices;
}

uint32_t CCALL joyGetNumDevs_c(void)
{
    return backend_count();
}

uint32_t CCALL joyGetDevCapsA_c(uint32_t uJoyID, joycaps_a *pjc, uint32_t cbjc)
{
    device *d;

    int count, axes, buttons, hats;
    const char *name;

    count = backend_count();
    if (winapi_debug >= 2) eprintf("joyGetDevCapsA: %u\n", uJoyID);
    if ((pjc == NULL) || (cbjc < sizeof(joycaps_a))) return MMSYSERR_INVALPARAM;
    if (uJoyID >= (uint32_t)count) return JOYERR_PARMS;
#if defined(__linux__)
    if (use_evdev)
    {
        name = evdev_joystick_name(uJoyID);
        evdev_joystick_caps(uJoyID, &axes, &buttons, &hats);
    }
    else
#endif
    {
        d = &devices[uJoyID];
        name = device_name(d);
        axes = d->axes;
        buttons = d->buttons;
        hats = d->hats;
    }

    // merged joystick 0: report the widest capabilities so the game offers all merged axes/buttons
    if (merge_devices && (uJoyID == 0))
    {
        int k;
        for (k = 1; k < num_devices; k++)
        {
            if (devices[k].joystick == NULL) continue;
            if (devices[k].axes > axes) axes = devices[k].axes;
            if (devices[k].buttons > buttons) buttons = devices[k].buttons;
            if (devices[k].hats > hats) hats = devices[k].hats;
        }
    }

    memset(pjc, 0, sizeof(joycaps_a));
    pjc->wMid = 0x045E;
    pjc->wPid = (uint16_t)(0x0100 + uJoyID);
    snprintf(pjc->szPname, sizeof(pjc->szPname), "%.31s", name);
    pjc->wXmax = pjc->wYmax = pjc->wZmax = pjc->wRmax = pjc->wUmax = pjc->wVmax = 65535;
    pjc->wNumButtons = buttons;
    pjc->wMaxButtons = 32;
    pjc->wPeriodMin = 10;
    pjc->wPeriodMax = 1000;
    pjc->wMaxAxes = 6;
    pjc->wNumAxes = (axes < 2) ? 2 : axes;
    if (axes >= 3) pjc->wCaps |= JOYCAPS_HASZ;
    if (axes >= 4) pjc->wCaps |= JOYCAPS_HASR;
    if (axes >= 5) pjc->wCaps |= JOYCAPS_HASU;
    if (axes >= 6) pjc->wCaps |= JOYCAPS_HASV;
    if (hats > 0) pjc->wCaps |= JOYCAPS_HASPOV | JOYCAPS_POV4DIR;
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

// when merging, an axis reports the value that deviates most from the center, so moving any device is visible
static uint32_t merge_axis(uint32_t a, uint32_t b)
{
    int da = (int)a - 32768, db = (int)b - 32768;
    return ((db < 0 ? -db : db) > (da < 0 ? -da : da)) ? b : a;
}

// winmm joystick 0 = all devices combined: the game detects a button/axis only on the configured joystick,
// so a press on any pad/wheel has to show up there (Control Configuration "Press any key or button...")
static void merge_devices_into(joyinfoex *pji, uint32_t uJoyID)
{
    joyinfoex other;
    int i;

    if (!merge_devices || (uJoyID != 0) || (num_devices < 2)) return;
    for (i = 1; i < num_devices; i++)
    {
        if (devices[i].joystick == NULL) continue;
        read_device(&devices[i], &other);
        pji->dwButtons |= other.dwButtons;
        if (pji->dwPOV == JOY_POVCENTERED) pji->dwPOV = other.dwPOV;
        pji->dwXpos = merge_axis(pji->dwXpos, other.dwXpos);
        pji->dwYpos = merge_axis(pji->dwYpos, other.dwYpos);
        pji->dwZpos = merge_axis(pji->dwZpos, other.dwZpos);
        pji->dwRpos = merge_axis(pji->dwRpos, other.dwRpos);
        pji->dwUpos = merge_axis(pji->dwUpos, other.dwUpos);
        pji->dwVpos = merge_axis(pji->dwVpos, other.dwVpos);
    }
}

uint32_t CCALL joyGetPosEx_c(uint32_t uJoyID, joyinfoex *pji)
{
    uint32_t size, flags;
    int count = backend_count();

    if (pji == NULL) return MMSYSERR_INVALPARAM;
    if (uJoyID >= (uint32_t)count) return JOYERR_PARMS;
    size = pji->dwSize;
    flags = pji->dwFlags;

#if defined(__linux__)
    if (use_evdev)
    {
        joy_state st;
        if (!evdev_joystick_read(uJoyID, &st)) return JOYERR_UNPLUGGED;
        pji->dwXpos = st.axis[0]; pji->dwYpos = st.axis[1]; pji->dwZpos = st.axis[2];
        pji->dwRpos = st.axis[3]; pji->dwUpos = st.axis[4]; pji->dwVpos = st.axis[5];
        pji->dwButtons = st.buttons;
        pji->dwButtonNumber = (uint32_t)__builtin_popcount(st.buttons);
        pji->dwPOV = st.pov;
    }
    else
#endif
    {
        if (!SDL_JoystickGetAttached(devices[uJoyID].joystick)) return JOYERR_UNPLUGGED;
        read_device(&devices[uJoyID], pji);
        merge_devices_into(pji, uJoyID);
    }
    pji->dwSize = size;
    pji->dwFlags = flags;

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

EXTERN_C_END
