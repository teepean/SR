/**
 *
 *  DirectInput 7 (the parts LithTech's input manager uses): the system keyboard and mouse, fed from the window's
 *  SDL events (dinput_event). The engine enumerates the devices and their objects, then builds its own data
 *  format (one dword per object, clientinput.cpp) - objects are therefore reported like Windows did: keys as
 *  buttons with the DIK code as instance and offset (bindings name them "##<DIK code>"), the mouse as
 *  c_dfDIMouse's X/Y/Z axes (offsets 0/4/8) and buttons (12..15). Joysticks are not offered (yet).
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "platform.h"
#include "ptr32.h"
#include "winapi.h"
#include "dinput.h"
#include "host.h"
#include "asmsyms.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

// IDirectInputAVtbl_asm2c, IDirectInputDevice2AVtbl_asm2c: x64/com-asm.asm (asmsyms.h)

#define DI_OK 0
#define DI_BUFFEROVERFLOW 1
#define DI_NOEFFECT 1
#define DIERR_INVALIDPARAM 0x80070057
#define DIERR_NOINTERFACE 0x80004002
#define DIERR_NOTACQUIRED 0x8007000C
#define DIERR_DEVICENOTREG 0x80040154
#define DIERR_UNSUPPORTED 0x80004001
#define DIERR_OBJECTNOTFOUND 0x80070002
#define DIERR_NOTINITIALIZED 0x80070015

#define DIDEVTYPE_MOUSE 2
#define DIDEVTYPE_KEYBOARD 3
#define DIDEVTYPEMOUSE_TRADITIONAL 2
#define DIDEVTYPEKEYBOARD_PCENH 4

#define DIDFT_RELAXIS 0x01
#define DIDFT_ABSAXIS 0x02
#define DIDFT_AXIS 0x03
#define DIDFT_PSHBUTTON 0x04
#define DIDFT_BUTTON 0x0C
#define DIDFT_POV 0x10
#define DIDFT_ANYINSTANCE 0x00FFFF00
#define DIDFT_INSTANCE(t) (((t) >> 8) & 0xFFFF)

#define DIPH_DEVICE 0
#define DIPH_BYOFFSET 1
#define DIPH_BYID 2

#define DIGDD_PEEK 1

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } guid32;

static const guid32 GUID_SysMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const guid32 GUID_SysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const guid32 GUID_XAxis = { 0xA36D02E0, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const guid32 GUID_YAxis = { 0xA36D02E1, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const guid32 GUID_ZAxis = { 0xA36D02E2, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const guid32 GUID_Button = { 0xA36D02F0, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const guid32 GUID_Key = { 0x55728220, 0xD33C, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
// IDirectInputDeviceA, IDirectInputDevice2A, IDirectInputDevice7A: all answered by the same object
static const uint32_t IID_IDirectInputDeviceA_d1 = 0x5944E680, IID_IDirectInputDevice2A_d1 = 0x5944E682, IID_IDirectInputDevice7A_d1 = 0x57D7C6BC;

static int guid_eq(const void *a, const void *b) { return memcmp(a, b, 16) == 0; }

/* ------------------------------------------------------------------ */
/* the devices' objects                                                */

typedef struct {
    const guid32 *guid;
    uint32_t ofs;           // offset in the device's native format (c_dfDIKeyboard: the DIK code; c_dfDIMouse)
    uint32_t type;          // DIDFT_*: type | instance << 8
    const char *name;
} di_object;

#define KEY(dik, name) { &GUID_Key, dik, DIDFT_PSHBUTTON | ((dik) << 8), name }
static const di_object keyboard_objects[] = {
    KEY(0x01, "Esc"), KEY(0x02, "1"), KEY(0x03, "2"), KEY(0x04, "3"), KEY(0x05, "4"), KEY(0x06, "5"), KEY(0x07, "6"),
    KEY(0x08, "7"), KEY(0x09, "8"), KEY(0x0A, "9"), KEY(0x0B, "0"), KEY(0x0C, "-"), KEY(0x0D, "="), KEY(0x0E, "Backspace"),
    KEY(0x0F, "Tab"), KEY(0x10, "Q"), KEY(0x11, "W"), KEY(0x12, "E"), KEY(0x13, "R"), KEY(0x14, "T"), KEY(0x15, "Y"),
    KEY(0x16, "U"), KEY(0x17, "I"), KEY(0x18, "O"), KEY(0x19, "P"), KEY(0x1A, "["), KEY(0x1B, "]"), KEY(0x1C, "Enter"),
    KEY(0x1D, "Ctrl"), KEY(0x1E, "A"), KEY(0x1F, "S"), KEY(0x20, "D"), KEY(0x21, "F"), KEY(0x22, "G"), KEY(0x23, "H"),
    KEY(0x24, "J"), KEY(0x25, "K"), KEY(0x26, "L"), KEY(0x27, ";"), KEY(0x28, "'"), KEY(0x29, "`"), KEY(0x2A, "Shift"),
    KEY(0x2B, "\\"), KEY(0x2C, "Z"), KEY(0x2D, "X"), KEY(0x2E, "C"), KEY(0x2F, "V"), KEY(0x30, "B"), KEY(0x31, "N"),
    KEY(0x32, "M"), KEY(0x33, ","), KEY(0x34, "."), KEY(0x35, "/"), KEY(0x36, "Right Shift"), KEY(0x37, "Num *"),
    KEY(0x38, "Alt"), KEY(0x39, "Space"), KEY(0x3A, "Caps Lock"), KEY(0x3B, "F1"), KEY(0x3C, "F2"), KEY(0x3D, "F3"),
    KEY(0x3E, "F4"), KEY(0x3F, "F5"), KEY(0x40, "F6"), KEY(0x41, "F7"), KEY(0x42, "F8"), KEY(0x43, "F9"), KEY(0x44, "F10"),
    KEY(0x45, "Num Lock"), KEY(0x46, "Scroll Lock"), KEY(0x47, "Num 7"), KEY(0x48, "Num 8"), KEY(0x49, "Num 9"),
    KEY(0x4A, "Num -"), KEY(0x4B, "Num 4"), KEY(0x4C, "Num 5"), KEY(0x4D, "Num 6"), KEY(0x4E, "Num +"), KEY(0x4F, "Num 1"),
    KEY(0x50, "Num 2"), KEY(0x51, "Num 3"), KEY(0x52, "Num 0"), KEY(0x53, "Num Del"), KEY(0x56, "<"), KEY(0x57, "F11"),
    KEY(0x58, "F12"), KEY(0x9C, "Num Enter"), KEY(0x9D, "Right Ctrl"), KEY(0xB5, "Num /"), KEY(0xB7, "Prnt Scrn"),
    KEY(0xB8, "Right Alt"), KEY(0xC5, "Pause"), KEY(0xC7, "Home"), KEY(0xC8, "Up"), KEY(0xC9, "Page Up"), KEY(0xCB, "Left"),
    KEY(0xCD, "Right"), KEY(0xCF, "End"), KEY(0xD0, "Down"), KEY(0xD1, "Page Down"), KEY(0xD2, "Insert"), KEY(0xD3, "Delete"),
    KEY(0xDB, "Left Windows"), KEY(0xDC, "Right Windows"), KEY(0xDD, "Application"),
};
#undef KEY

static const di_object mouse_objects[] = {
    { &GUID_XAxis, 0, DIDFT_RELAXIS | (0 << 8), "X-axis" },
    { &GUID_YAxis, 4, DIDFT_RELAXIS | (1 << 8), "Y-axis" },
    { &GUID_ZAxis, 8, DIDFT_RELAXIS | (2 << 8), "Wheel" },
    { &GUID_Button, 12, DIDFT_PSHBUTTON | (3 << 8), "Button 0" },
    { &GUID_Button, 13, DIDFT_PSHBUTTON | (4 << 8), "Button 1" },
    { &GUID_Button, 14, DIDFT_PSHBUTTON | (5 << 8), "Button 2" },
    { &GUID_Button, 15, DIDFT_PSHBUTTON | (6 << 8), "Button 3" },
};

// SDL scancode -> DIK code
static uint8_t dik_of_scancode(SDL_Scancode sc)
{
    static uint8_t map[SDL_NUM_SCANCODES];
    static int ready;
    if (!ready)
    {
        static const struct { SDL_Scancode sc; uint8_t dik; } t[] = {
            { SDL_SCANCODE_ESCAPE, 0x01 }, { SDL_SCANCODE_1, 0x02 }, { SDL_SCANCODE_2, 0x03 }, { SDL_SCANCODE_3, 0x04 },
            { SDL_SCANCODE_4, 0x05 }, { SDL_SCANCODE_5, 0x06 }, { SDL_SCANCODE_6, 0x07 }, { SDL_SCANCODE_7, 0x08 },
            { SDL_SCANCODE_8, 0x09 }, { SDL_SCANCODE_9, 0x0A }, { SDL_SCANCODE_0, 0x0B }, { SDL_SCANCODE_MINUS, 0x0C },
            { SDL_SCANCODE_EQUALS, 0x0D }, { SDL_SCANCODE_BACKSPACE, 0x0E }, { SDL_SCANCODE_TAB, 0x0F },
            { SDL_SCANCODE_Q, 0x10 }, { SDL_SCANCODE_W, 0x11 }, { SDL_SCANCODE_E, 0x12 }, { SDL_SCANCODE_R, 0x13 },
            { SDL_SCANCODE_T, 0x14 }, { SDL_SCANCODE_Y, 0x15 }, { SDL_SCANCODE_U, 0x16 }, { SDL_SCANCODE_I, 0x17 },
            { SDL_SCANCODE_O, 0x18 }, { SDL_SCANCODE_P, 0x19 }, { SDL_SCANCODE_LEFTBRACKET, 0x1A },
            { SDL_SCANCODE_RIGHTBRACKET, 0x1B }, { SDL_SCANCODE_RETURN, 0x1C }, { SDL_SCANCODE_LCTRL, 0x1D },
            { SDL_SCANCODE_A, 0x1E }, { SDL_SCANCODE_S, 0x1F }, { SDL_SCANCODE_D, 0x20 }, { SDL_SCANCODE_F, 0x21 },
            { SDL_SCANCODE_G, 0x22 }, { SDL_SCANCODE_H, 0x23 }, { SDL_SCANCODE_J, 0x24 }, { SDL_SCANCODE_K, 0x25 },
            { SDL_SCANCODE_L, 0x26 }, { SDL_SCANCODE_SEMICOLON, 0x27 }, { SDL_SCANCODE_APOSTROPHE, 0x28 },
            { SDL_SCANCODE_GRAVE, 0x29 }, { SDL_SCANCODE_LSHIFT, 0x2A }, { SDL_SCANCODE_BACKSLASH, 0x2B },
            { SDL_SCANCODE_Z, 0x2C }, { SDL_SCANCODE_X, 0x2D }, { SDL_SCANCODE_C, 0x2E }, { SDL_SCANCODE_V, 0x2F },
            { SDL_SCANCODE_B, 0x30 }, { SDL_SCANCODE_N, 0x31 }, { SDL_SCANCODE_M, 0x32 }, { SDL_SCANCODE_COMMA, 0x33 },
            { SDL_SCANCODE_PERIOD, 0x34 }, { SDL_SCANCODE_SLASH, 0x35 }, { SDL_SCANCODE_RSHIFT, 0x36 },
            { SDL_SCANCODE_KP_MULTIPLY, 0x37 }, { SDL_SCANCODE_LALT, 0x38 }, { SDL_SCANCODE_SPACE, 0x39 },
            { SDL_SCANCODE_CAPSLOCK, 0x3A }, { SDL_SCANCODE_F1, 0x3B }, { SDL_SCANCODE_F2, 0x3C }, { SDL_SCANCODE_F3, 0x3D },
            { SDL_SCANCODE_F4, 0x3E }, { SDL_SCANCODE_F5, 0x3F }, { SDL_SCANCODE_F6, 0x40 }, { SDL_SCANCODE_F7, 0x41 },
            { SDL_SCANCODE_F8, 0x42 }, { SDL_SCANCODE_F9, 0x43 }, { SDL_SCANCODE_F10, 0x44 },
            { SDL_SCANCODE_NUMLOCKCLEAR, 0x45 }, { SDL_SCANCODE_SCROLLLOCK, 0x46 }, { SDL_SCANCODE_KP_7, 0x47 },
            { SDL_SCANCODE_KP_8, 0x48 }, { SDL_SCANCODE_KP_9, 0x49 }, { SDL_SCANCODE_KP_MINUS, 0x4A },
            { SDL_SCANCODE_KP_4, 0x4B }, { SDL_SCANCODE_KP_5, 0x4C }, { SDL_SCANCODE_KP_6, 0x4D },
            { SDL_SCANCODE_KP_PLUS, 0x4E }, { SDL_SCANCODE_KP_1, 0x4F }, { SDL_SCANCODE_KP_2, 0x50 },
            { SDL_SCANCODE_KP_3, 0x51 }, { SDL_SCANCODE_KP_0, 0x52 }, { SDL_SCANCODE_KP_PERIOD, 0x53 },
            { SDL_SCANCODE_NONUSBACKSLASH, 0x56 }, { SDL_SCANCODE_F11, 0x57 }, { SDL_SCANCODE_F12, 0x58 },
            { SDL_SCANCODE_KP_ENTER, 0x9C }, { SDL_SCANCODE_RCTRL, 0x9D }, { SDL_SCANCODE_KP_DIVIDE, 0xB5 },
            { SDL_SCANCODE_PRINTSCREEN, 0xB7 }, { SDL_SCANCODE_RALT, 0xB8 }, { SDL_SCANCODE_PAUSE, 0xC5 },
            { SDL_SCANCODE_HOME, 0xC7 }, { SDL_SCANCODE_UP, 0xC8 }, { SDL_SCANCODE_PAGEUP, 0xC9 },
            { SDL_SCANCODE_LEFT, 0xCB }, { SDL_SCANCODE_RIGHT, 0xCD }, { SDL_SCANCODE_END, 0xCF },
            { SDL_SCANCODE_DOWN, 0xD0 }, { SDL_SCANCODE_PAGEDOWN, 0xD1 }, { SDL_SCANCODE_INSERT, 0xD2 },
            { SDL_SCANCODE_DELETE, 0xD3 }, { SDL_SCANCODE_LGUI, 0xDB }, { SDL_SCANCODE_RGUI, 0xDC },
            { SDL_SCANCODE_APPLICATION, 0xDD },
        };
        unsigned i;
        for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) map[t[i].sc] = t[i].dik;
        ready = 1;
    }
    return ((unsigned)sc < SDL_NUM_SCANCODES) ? map[sc] : 0;
}

/* ------------------------------------------------------------------ */
/* 32-bit structures                                                   */

#pragma pack(push, 1)
typedef struct {
    uint32_t dwSize;
    guid32 guidInstance, guidProduct;
    uint32_t dwDevType;
    char tszInstanceName[260], tszProductName[260];
    guid32 guidFFDriver;            // DirectInput 5+
    uint16_t wUsagePage, wUsage;
} DIDEVICEINSTANCEA_32;

typedef struct {
    uint32_t dwSize;
    guid32 guidType;
    uint32_t dwOfs, dwType, dwFlags;
    char tszName[260];
    uint32_t dwFFMaxForce, dwFFForceResolution;     // DirectInput 5+
    uint16_t wCollectionNumber, wDesignatorIndex, wUsagePage, wUsage;
    uint32_t dwDimension;
    uint16_t wExponent, wReserved;
} DIDEVICEOBJECTINSTANCEA_32;

typedef struct { PTR32(guid32) pguid; uint32_t dwOfs, dwType, dwFlags; } DIOBJECTDATAFORMAT_32;
typedef struct { uint32_t dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs; PTR32(DIOBJECTDATAFORMAT_32) rgodf; } DIDATAFORMAT_32;
typedef struct { uint32_t dwOfs, dwData, dwTimeStamp, dwSequence; } DIDEVICEOBJECTDATA_32;
typedef struct { uint32_t dwSize, dwHeaderSize, dwObj, dwHow; } DIPROPHEADER_32;
typedef struct { uint32_t dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, dwFFSamplePeriod, dwFFMinTimeResolution, dwFirmwareRevision, dwHardwareRevision, dwFFDriverVersion; } DIDEVCAPS_32;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* objects                                                             */

#define QUEUE_SIZE 256

typedef struct di_device {
    PTR32(void) lpVtbl;
    uint32_t refs;
    int kind;                       // DIDEVTYPE_KEYBOARD / DIDEVTYPE_MOUSE
    const di_object *objects;
    int num_objects;
    int acquired;
    // the application's data format: per native object its offset there (-1: not in the format)
    int app_ofs[128];
    uint32_t data_size;
    uint32_t buffer_size;           // DIPROP_BUFFERSIZE (0: no buffered data)
    // buffered data: native object index, value, time
    struct { int obj; uint32_t value, time; } queue[QUEUE_SIZE];
    int q_head, q_count, overflow;
    uint32_t sequence;
    // immediate state: keys/buttons, accumulated relative axes
    uint8_t buttons[256];
    int32_t axes[3];
    struct di_device *next;
} di_device;

typedef struct {
    PTR32(void) lpVtbl;
    uint32_t refs;
} di_object_root;

static di_device *devices;

static void queue_event(di_device *d, int obj, uint32_t value)
{
    int i;
    if (!d->acquired || (d->buffer_size == 0) || (d->app_ofs[obj] < 0)) return;
    if (d->q_count >= QUEUE_SIZE || d->q_count >= (int)d->buffer_size) { d->overflow = 1; return; }
    i = (d->q_head + d->q_count) % QUEUE_SIZE;
    d->queue[i].obj = obj;
    d->queue[i].value = value;
    d->queue[i].time = SDL_GetTicks();
    d->q_count++;
}

static int find_object(di_device *d, uint32_t native_ofs)
{
    int i;
    for (i = 0; i < d->num_objects; i++) if (d->objects[i].ofs == native_ofs) return i;
    return -1;
}

// called with every SDL event from the window's message processing
void dinput_event(const SDL_Event *ev)
{
    di_device *d;
    for (d = devices; d != NULL; d = d->next)
    {
        if (d->kind == DIDEVTYPE_KEYBOARD && (ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP))
        {
            uint8_t dik = dik_of_scancode(ev->key.keysym.scancode);
            int obj;
            if (dik == 0 || ev->key.repeat) continue;
            obj = find_object(d, dik);
            if (obj < 0) continue;
            d->buttons[dik] = (ev->type == SDL_KEYDOWN) ? 0x80 : 0;
            queue_event(d, obj, d->buttons[dik]);
        }
        else if (d->kind == DIDEVTYPE_MOUSE)
        {
            if (ev->type == SDL_MOUSEMOTION)
            {
                if (ev->motion.xrel) { d->axes[0] += ev->motion.xrel; queue_event(d, 0, (uint32_t)ev->motion.xrel); }
                if (ev->motion.yrel) { d->axes[1] += ev->motion.yrel; queue_event(d, 1, (uint32_t)ev->motion.yrel); }
            }
            else if (ev->type == SDL_MOUSEWHEEL)
            {
                int32_t z = ev->wheel.y * 120;
                if (z) { d->axes[2] += z; queue_event(d, 2, (uint32_t)z); }
            }
            else if (ev->type == SDL_MOUSEBUTTONDOWN || ev->type == SDL_MOUSEBUTTONUP)
            {
                // DirectInput's buttons: 0 left, 1 right, 2 middle, 3 the first extra one
                int b = (ev->button.button == SDL_BUTTON_LEFT) ? 0 : (ev->button.button == SDL_BUTTON_RIGHT) ? 1 :
                        (ev->button.button == SDL_BUTTON_MIDDLE) ? 2 : (ev->button.button == SDL_BUTTON_X1) ? 3 : -1;
                if (b < 0) continue;
                d->buttons[b] = (ev->type == SDL_MOUSEBUTTONDOWN) ? 0x80 : 0;
                queue_event(d, 3 + b, d->buttons[b]);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* IDirectInputA                                                       */

uint32_t CCALL DirectInputCreateA_c(void *hinst, uint32_t dwVersion, PTR32(di_object_root) *ppDI, void *punkOuter)
{
    di_object_root *di;
    if (ppDI == NULL) return DIERR_INVALIDPARAM;
    di = (di_object_root *)game_calloc(1, sizeof(di_object_root));
    di->lpVtbl = IDirectInputAVtbl_asm2c;
    di->refs = 1;
    *ppDI = di;
    if (winapi_debug) eprintf("DirectInputCreateA: version 0x%x\n", dwVersion);
    return DI_OK;
}

uint32_t CCALL IDirectInputA_QueryInterface_c(di_object_root *lpThis, const guid32 *riid, PTR32(void) *ppvObj)
{
    if (ppvObj != NULL) *ppvObj = NULL;
    return DIERR_NOINTERFACE;
}
uint32_t CCALL IDirectInputA_AddRef_c(di_object_root *lpThis) { return ++lpThis->refs; }
uint32_t CCALL IDirectInputA_Release_c(di_object_root *lpThis)
{
    if (--lpThis->refs) return lpThis->refs;
    lpThis->lpVtbl = NULL;
    game_free(lpThis);
    return 0;
}

static void fill_instance(DIDEVICEINSTANCEA_32 *inst, uint32_t size, int kind)
{
    memset(inst, 0, size);
    inst->dwSize = size;
    if (kind == DIDEVTYPE_KEYBOARD)
    {
        inst->guidInstance = inst->guidProduct = GUID_SysKeyboard;
        inst->dwDevType = DIDEVTYPE_KEYBOARD | (DIDEVTYPEKEYBOARD_PCENH << 8);
        strcpy(inst->tszInstanceName, "Keyboard");
        strcpy(inst->tszProductName, "Keyboard");
    }
    else
    {
        inst->guidInstance = inst->guidProduct = GUID_SysMouse;
        inst->dwDevType = DIDEVTYPE_MOUSE | (DIDEVTYPEMOUSE_TRADITIONAL << 8);
        strcpy(inst->tszInstanceName, "Mouse");
        strcpy(inst->tszProductName, "Mouse");
    }
}

uint32_t CCALL IDirectInputA_EnumDevices_c(di_object_root *lpThis, uint32_t dwDevType, uint32_t lpCallback, uint32_t pvRef, uint32_t dwFlags)
{
    static const int kinds[2] = { DIDEVTYPE_MOUSE, DIDEVTYPE_KEYBOARD };
    DIDEVICEINSTANCEA_32 *inst;
    int i;

    inst = (DIDEVICEINSTANCEA_32 *)game_malloc(sizeof(DIDEVICEINSTANCEA_32));
    for (i = 0; i < 2; i++)
    {
        uint32_t args[2];
        if ((dwDevType != 0) && ((dwDevType & 0xFF) != (uint32_t)kinds[i])) continue;
        fill_instance(inst, sizeof(*inst), kinds[i]);
        args[0] = H2G(inst);
        args[1] = pvRef;
        if (call_game(lpCallback, 2, args) == 0) break;     // DIENUM_STOP
    }
    game_free(inst);
    return DI_OK;
}

uint32_t CCALL IDirectInputA_CreateDevice_c(di_object_root *lpThis, const guid32 *rguid, PTR32(di_device) *ppDevice, void *punkOuter)
{
    di_device *d;
    int i;

    if ((rguid == NULL) || (ppDevice == NULL)) return DIERR_INVALIDPARAM;
    *ppDevice = NULL;
    d = (di_device *)game_calloc(1, sizeof(di_device));
    d->lpVtbl = IDirectInputDevice2AVtbl_asm2c;
    d->refs = 1;
    if (guid_eq(rguid, &GUID_SysKeyboard))
    {
        d->kind = DIDEVTYPE_KEYBOARD;
        d->objects = keyboard_objects;
        d->num_objects = sizeof(keyboard_objects) / sizeof(keyboard_objects[0]);
    }
    else if (guid_eq(rguid, &GUID_SysMouse))
    {
        d->kind = DIDEVTYPE_MOUSE;
        d->objects = mouse_objects;
        d->num_objects = sizeof(mouse_objects) / sizeof(mouse_objects[0]);
    }
    else
    {
        game_free(d);
        return DIERR_DEVICENOTREG;
    }
    for (i = 0; i < 128; i++) d->app_ofs[i] = -1;
    d->next = devices;
    devices = d;
    *ppDevice = d;
    if (winapi_debug) eprintf("DirectInput: created the %s\n", (d->kind == DIDEVTYPE_KEYBOARD) ? "keyboard" : "mouse");
    return DI_OK;
}

uint32_t CCALL IDirectInputA_GetDeviceStatus_c(di_object_root *lpThis, const guid32 *rguid)
{
    return (guid_eq(rguid, &GUID_SysKeyboard) || guid_eq(rguid, &GUID_SysMouse)) ? DI_OK : DIERR_DEVICENOTREG;
}
uint32_t CCALL IDirectInputA_RunControlPanel_c(di_object_root *lpThis, void *hwnd, uint32_t flags) { return DI_OK; }
uint32_t CCALL IDirectInputA_Initialize_c(di_object_root *lpThis, void *hinst, uint32_t version) { return DI_OK; }

/* ------------------------------------------------------------------ */
/* IDirectInputDevice2A                                                */

uint32_t CCALL IDirectInputDevice2A_QueryInterface_c(di_device *lpThis, const guid32 *riid, PTR32(void) *ppvObj)
{
    if (ppvObj == NULL) return DIERR_INVALIDPARAM;
    *ppvObj = NULL;
    if ((riid != NULL) && ((riid->d1 == IID_IDirectInputDeviceA_d1) || (riid->d1 == IID_IDirectInputDevice2A_d1) || (riid->d1 == IID_IDirectInputDevice7A_d1)))
    {
        lpThis->refs++;
        *ppvObj = lpThis;
        return DI_OK;
    }
    return DIERR_NOINTERFACE;
}

uint32_t CCALL IDirectInputDevice2A_AddRef_c(di_device *lpThis) { return ++lpThis->refs; }

uint32_t CCALL IDirectInputDevice2A_Release_c(di_device *lpThis)
{
    di_device **p;
    if (--lpThis->refs) return lpThis->refs;
    for (p = &devices; *p != NULL; p = &(*p)->next) if (*p == lpThis) { *p = lpThis->next; break; }
    if ((lpThis->kind == DIDEVTYPE_MOUSE) && lpThis->acquired) host_set_relative_mouse(0);
    lpThis->lpVtbl = NULL;
    game_free(lpThis);
    return 0;
}

uint32_t CCALL IDirectInputDevice2A_GetCapabilities_c(di_device *lpThis, DIDEVCAPS_32 *caps)
{
    uint32_t size;
    if (caps == NULL) return DIERR_INVALIDPARAM;
    size = caps->dwSize;
    memset(caps, 0, (size < sizeof(*caps)) ? size : sizeof(*caps));
    caps->dwSize = size;
    caps->dwFlags = 1 | 4;      // DIDC_ATTACHED | DIDC_EMULATED
    if (lpThis->kind == DIDEVTYPE_KEYBOARD)
    {
        caps->dwDevType = DIDEVTYPE_KEYBOARD | (DIDEVTYPEKEYBOARD_PCENH << 8);
        caps->dwButtons = lpThis->num_objects;
    }
    else
    {
        caps->dwDevType = DIDEVTYPE_MOUSE | (DIDEVTYPEMOUSE_TRADITIONAL << 8);
        caps->dwAxes = 3;
        caps->dwButtons = 4;
    }
    return DI_OK;
}

static void fill_object(DIDEVICEOBJECTINSTANCEA_32 *oi, uint32_t size, const di_object *o)
{
    memset(oi, 0, size);
    oi->dwSize = size;
    oi->guidType = *o->guid;
    oi->dwOfs = o->ofs;
    oi->dwType = o->type;
    snprintf(oi->tszName, sizeof(oi->tszName), "%s", o->name);
}

uint32_t CCALL IDirectInputDevice2A_EnumObjects_c(di_device *lpThis, uint32_t lpCallback, uint32_t pvRef, uint32_t dwFlags)
{
    DIDEVICEOBJECTINSTANCEA_32 *oi;
    int i;

    oi = (DIDEVICEOBJECTINSTANCEA_32 *)game_malloc(sizeof(DIDEVICEOBJECTINSTANCEA_32));
    for (i = 0; i < lpThis->num_objects; i++)
    {
        uint32_t args[2];
        // DIDFT_ALL (0) lists everything, otherwise the object's type must be among the asked ones
        if (((dwFlags & 0xFF) != 0) && !(lpThis->objects[i].type & dwFlags & 0xFF)) continue;
        fill_object(oi, sizeof(*oi), &lpThis->objects[i]);
        args[0] = H2G(oi);
        args[1] = pvRef;
        if (call_game(lpCallback, 2, args) == 0) break;
    }
    game_free(oi);
    return DI_OK;
}

// the object a property header names: DIPH_BYOFFSET (offset in the application's format) or DIPH_BYID
static int header_object(di_device *d, const DIPROPHEADER_32 *h)
{
    int i;
    for (i = 0; i < d->num_objects; i++)
    {
        if ((h->dwHow == DIPH_BYID) && ((d->objects[i].type & 0xFFFFFF) == (h->dwObj & 0xFFFFFF))) return i;
        if ((h->dwHow == DIPH_BYOFFSET) && (d->app_ofs[i] == (int)h->dwObj)) return i;
    }
    return -1;
}

// properties: GUIDs passed as MAKEDIPROP(n) values
uint32_t CCALL IDirectInputDevice2A_GetProperty_c(di_device *lpThis, uint32_t rguidProp, DIPROPHEADER_32 *pdiph)
{
    uint32_t *data = (uint32_t *)(pdiph + 1);
    if (pdiph == NULL) return DIERR_INVALIDPARAM;
    switch (rguidProp)
    {
        case 1:     // DIPROP_BUFFERSIZE
            data[0] = lpThis->buffer_size;
            return DI_OK;
        case 2:     // DIPROP_AXISMODE: relative
            data[0] = 1;
            return DI_OK;
        case 4:     // DIPROP_RANGE: mouse axes have none
            if ((pdiph->dwHow != DIPH_DEVICE) && (header_object(lpThis, pdiph) < 0)) return DIERR_OBJECTNOTFOUND;
            return DIERR_UNSUPPORTED;
        case 6:     // DIPROP_GRANULARITY
            data[0] = (lpThis->kind == DIDEVTYPE_MOUSE && pdiph->dwObj == 8) ? 120 : 1;
            return DI_OK;
        default:
            return DIERR_UNSUPPORTED;
    }
}

uint32_t CCALL IDirectInputDevice2A_SetProperty_c(di_device *lpThis, uint32_t rguidProp, const DIPROPHEADER_32 *pdiph)
{
    const uint32_t *data = (const uint32_t *)(pdiph + 1);
    if (pdiph == NULL) return DIERR_INVALIDPARAM;
    if (rguidProp == 1)
    {
        lpThis->buffer_size = data[0];
        if (winapi_debug >= 2) eprintf("DirectInput: buffer size %u\n", data[0]);
        return DI_OK;
    }
    return DI_OK;
}

uint32_t CCALL IDirectInputDevice2A_Acquire_c(di_device *lpThis)
{
    if (lpThis->acquired) return DI_NOEFFECT;
    lpThis->acquired = 1;
    // the game reads the mouse relatively (mouse look, its own menu cursor)
    if ((lpThis->kind == DIDEVTYPE_MOUSE) && (getenv("SDL_VIDEODRIVER") == NULL || strcmp(getenv("SDL_VIDEODRIVER"), "offscreen") != 0))
        host_set_relative_mouse(1);
    return DI_OK;
}

uint32_t CCALL IDirectInputDevice2A_Unacquire_c(di_device *lpThis)
{
    if (!lpThis->acquired) return DI_NOEFFECT;
    lpThis->acquired = 0;
    lpThis->q_count = 0;
    if (lpThis->kind == DIDEVTYPE_MOUSE) host_set_relative_mouse(0);
    return DI_OK;
}

uint32_t CCALL IDirectInputDevice2A_GetDeviceState_c(di_device *lpThis, uint32_t cbData, uint8_t *lpvData)
{
    int i;
    if (lpvData == NULL) return DIERR_INVALIDPARAM;
    if (!lpThis->acquired) return DIERR_NOTACQUIRED;
    memset(lpvData, 0, cbData);
    for (i = 0; i < lpThis->num_objects; i++)
    {
        int ofs = lpThis->app_ofs[i];
        const di_object *o = &lpThis->objects[i];
        if (ofs < 0) continue;
        if (o->type & DIDFT_AXIS)
        {
            if ((uint32_t)ofs + 4 <= cbData) memcpy(lpvData + ofs, &lpThis->axes[i], 4);
        }
        else if ((uint32_t)ofs < cbData)
        {
            lpvData[ofs] = (lpThis->kind == DIDEVTYPE_KEYBOARD) ? lpThis->buttons[o->ofs] : lpThis->buttons[o->ofs - 12];
        }
    }
    if (lpThis->kind == DIDEVTYPE_MOUSE) memset(lpThis->axes, 0, sizeof(lpThis->axes));
    return DI_OK;
}

uint32_t CCALL IDirectInputDevice2A_GetDeviceData_c(di_device *lpThis, uint32_t cbObjectData, DIDEVICEOBJECTDATA_32 *rgdod, uint32_t *pdwInOut, uint32_t dwFlags)
{
    uint32_t n, want, result;
    if (pdwInOut == NULL) return DIERR_INVALIDPARAM;
    if (!lpThis->acquired) { *pdwInOut = 0; return DIERR_NOTACQUIRED; }
    want = *pdwInOut;
    for (n = 0; (n < want) && ((int)n < lpThis->q_count); n++)
    {
        int i = (lpThis->q_head + n) % QUEUE_SIZE;
        if (rgdod != NULL)
        {
            DIDEVICEOBJECTDATA_32 *d = (DIDEVICEOBJECTDATA_32 *)((uint8_t *)rgdod + n * cbObjectData);
            d->dwOfs = (uint32_t)lpThis->app_ofs[lpThis->queue[i].obj];
            d->dwData = lpThis->queue[i].value;
            d->dwTimeStamp = lpThis->queue[i].time;
            d->dwSequence = ++lpThis->sequence;
        }
    }
    if (!(dwFlags & DIGDD_PEEK))
    {
        lpThis->q_head = (lpThis->q_head + n) % QUEUE_SIZE;
        lpThis->q_count -= n;
    }
    *pdwInOut = n;
    result = lpThis->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    lpThis->overflow = 0;
    return result;
}

uint32_t CCALL IDirectInputDevice2A_SetDataFormat_c(di_device *lpThis, const DIDATAFORMAT_32 *lpdf)
{
    uint32_t i;
    int j;
    char used[128];

    if (lpdf == NULL) return DIERR_INVALIDPARAM;
    if (lpThis->acquired) return 0x8007001E;    // DIERR_ACQUIRED
    for (j = 0; j < 128; j++) lpThis->app_ofs[j] = -1;
    memset(used, 0, sizeof(used));
    lpThis->data_size = lpdf->dwDataSize;
    for (i = 0; i < lpdf->dwNumObjs; i++)
    {
        const DIOBJECTDATAFORMAT_32 *f = (const DIOBJECTDATAFORMAT_32 *)((const uint8_t *)(DIOBJECTDATAFORMAT_32 *)lpdf->rgodf + i * lpdf->dwObjSize);
        const guid32 *g = f->pguid;
        int inst = DIDFT_INSTANCE(f->dwType);
        int any = ((f->dwType & DIDFT_ANYINSTANCE) == DIDFT_ANYINSTANCE);
        // the first native object (not taken yet) with a matching type, guid and instance
        for (j = 0; j < lpThis->num_objects; j++)
        {
            const di_object *o = &lpThis->objects[j];
            if (used[j]) continue;
            if ((f->dwType & 0xFF) && !(o->type & f->dwType & 0xFF)) continue;
            if ((g != NULL) && !guid_eq(g, o->guid)) continue;
            if (!any && (inst != DIDFT_INSTANCE(o->type))) continue;
            used[j] = 1;
            lpThis->app_ofs[j] = (int)f->dwOfs;
            break;
        }
    }
    if (winapi_debug) eprintf("DirectInput: %s data format, %u objects, %u bytes\n", (lpThis->kind == DIDEVTYPE_KEYBOARD) ? "keyboard" : "mouse", lpdf->dwNumObjs, lpdf->dwDataSize);
    return DI_OK;
}

uint32_t CCALL IDirectInputDevice2A_SetEventNotification_c(di_device *lpThis, void *hEvent) { return DI_OK; }
uint32_t CCALL IDirectInputDevice2A_SetCooperativeLevel_c(di_device *lpThis, void *hwnd, uint32_t dwFlags) { return DI_OK; }

uint32_t CCALL IDirectInputDevice2A_GetObjectInfo_c(di_device *lpThis, DIDEVICEOBJECTINSTANCEA_32 *pdidoi, uint32_t dwObj, uint32_t dwHow)
{
    DIPROPHEADER_32 h;
    int i;
    if (pdidoi == NULL) return DIERR_INVALIDPARAM;
    h.dwObj = dwObj;
    h.dwHow = dwHow;
    i = header_object(lpThis, &h);
    if (i < 0) return DIERR_OBJECTNOTFOUND;
    fill_object(pdidoi, pdidoi->dwSize ? pdidoi->dwSize : sizeof(*pdidoi), &lpThis->objects[i]);
    return DI_OK;
}

uint32_t CCALL IDirectInputDevice2A_GetDeviceInfo_c(di_device *lpThis, DIDEVICEINSTANCEA_32 *pdidi)
{
    if (pdidi == NULL) return DIERR_INVALIDPARAM;
    fill_instance(pdidi, pdidi->dwSize ? pdidi->dwSize : sizeof(*pdidi), lpThis->kind);
    return DI_OK;
}

uint32_t CCALL IDirectInputDevice2A_Poll_c(di_device *lpThis) { return DI_NOEFFECT; }

EXTERN_C_END
