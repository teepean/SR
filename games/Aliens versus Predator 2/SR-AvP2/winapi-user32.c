/**
 *
 *  USER32 emulation (windows, messages, input) on SDL2.
 *
 *  There is one game window; its client area is the display framebuffer (display.c).
 *  Screen coordinates are identical to client coordinates of the game window.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL.h>
#include "platform.h"
#include "printf_x86.h"
#include "display.h"
#include "dinput.h"
#include "peimage.h"
#include "host.h"

#include "winapi.h"
#include "winapi-gdi32.h"
#include "ptr32.h"
#include "Game-Memory.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)


// window procedure: address of recompiled game code, called through call_game (stdcall, 4 arguments)
typedef uint32_t wndproc_t;


/* ------------------------------------------------------------------ */
/* messages                                                            */

#define WM_CREATE         0x0001
#define WM_DESTROY        0x0002
#define WM_SIZE           0x0005
#define WM_ACTIVATE       0x0006
#define WM_SETFOCUS       0x0007
#define WM_KILLFOCUS      0x0008
#define WM_PAINT          0x000F
#define WM_CLOSE          0x0010
#define WM_QUIT           0x0012
#define WM_ERASEBKGND     0x0014
#define WM_SHOWWINDOW     0x0018
#define WM_ACTIVATEAPP    0x001C
#define WM_SETCURSOR      0x0020
#define WM_KEYDOWN        0x0100
#define WM_KEYUP          0x0101
#define WM_CHAR           0x0102
#define WM_SYSKEYDOWN     0x0104
#define WM_SYSKEYUP       0x0105
#define WM_SYSCHAR        0x0106
#define WM_MOUSEMOVE      0x0200
#define WM_LBUTTONDOWN    0x0201
#define WM_MBUTTONDBLCLK  0x0209
#define WM_LBUTTONUP      0x0202
#define WM_RBUTTONDOWN    0x0204
#define WM_RBUTTONUP      0x0205
#define WM_MBUTTONDOWN    0x0207
#define WM_MBUTTONUP      0x0208

#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT   0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010

typedef struct {
    PTR32(void) hwnd;
    uint32_t message;
    uint32_t wParam;
    uint32_t lParam;
    uint32_t time;
    int32_t pt_x;
    int32_t pt_y;
} win_msg;

#define QUEUE_SIZE 512
static win_msg queue[QUEUE_SIZE];
static int queue_head, queue_count;


/* ------------------------------------------------------------------ */
/* windows                                                             */

#define WINDOW_MAGIC 0x574e4448

typedef struct {
    char name[64];
    wndproc_t wndproc;
    int32_t cbWndExtra;
    uint32_t style;
    uint32_t hcursor, hicon;
} window_class;

typedef struct {
    uint32_t magic;
    window_class *cls;
    wndproc_t wndproc;
    uint32_t style, exstyle;
    int32_t x, y, width, height;
    int visible;
    uint32_t userdata;
    uint8_t extra[64];
    char title[128];
} window;

#define MAX_CLASSES 16
static window_class classes[MAX_CLASSES];
static int num_classes;

static window *main_window;
static window *focus_window;
static int app_active;

static window *get_window(void *hwnd)
{
    if ((hwnd != NULL) && (((window *)hwnd)->magic == WINDOW_MAGIC)) return (window *)hwnd;
    return NULL;
}

uint32_t winapi_call_wndproc(void *hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    window *w = get_window(hwnd);
    if ((w == NULL) || (w->wndproc == 0)) return 0;
    {
        uint32_t args[4] = { H2G(hwnd), msg, wparam, lparam };
        return call_game(w->wndproc, 4, args);
    }
}

static void post_message(void *hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    win_msg *m;
    int mx, my;

    // like Windows, keep at most one pending WM_MOUSEMOVE (the latest position): the game only removes
    // keyboard messages during a mission, so every mouse motion event would pile up, fill the queue and
    // make key presses get dropped (and the in-game menu then crawls through the backlog)
    if (msg == WM_MOUSEMOVE)
    {
        int i;
        for (i = queue_count - 1; i >= 0; i--)
        {
            win_msg *q = &queue[(queue_head + i) % QUEUE_SIZE];
            if ((q->message >= WM_LBUTTONDOWN) && (q->message <= WM_MBUTTONDBLCLK)) break;  // keep order around clicks
            if ((q->message == WM_MOUSEMOVE) && (q->hwnd == hwnd))
            {
                host_mouse_state(&mx, &my);
                q->wParam = wparam;
                q->lParam = lparam;
                q->time = winapi_get_ticks();
                q->pt_x = mx;
                q->pt_y = my;
                return;
            }
        }
    }

    if (queue_count >= QUEUE_SIZE)
    {
        if (winapi_debug) eprintf("post_message: queue full, message 0x%x dropped\n", msg);
        return;
    }
    m = &queue[(queue_head + queue_count) % QUEUE_SIZE];
    queue_count++;

    host_mouse_state(&mx, &my);
    m->hwnd = hwnd;
    m->message = msg;
    m->wParam = wparam;
    m->lParam = lparam;
    m->time = winapi_get_ticks();
    m->pt_x = mx;
    m->pt_y = my;
}


/* ------------------------------------------------------------------ */
/* keyboard                                                            */

static uint8_t key_state[256];      // bit 7 = down, bit 0 = toggled
static uint8_t key_pressed[256];    // pressed since last GetAsyncKeyState

// scripted input state (see run_script)
static int script_mouse_active, script_mouse_x, script_mouse_y;
static uint32_t script_buttons;

typedef struct { uint8_t vk, scan, ext; } key_map;
static key_map sdl_keys[SDL_NUM_SCANCODES];

static void set_key(int sc, uint8_t vk, uint8_t scan, uint8_t ext)
{
    sdl_keys[sc].vk = vk;
    sdl_keys[sc].scan = scan;
    sdl_keys[sc].ext = ext;
}

static void init_keymap(void)
{
    static const uint8_t letter_scan[26] = { 30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44 };
    int i;

    for (i = 0; i < 26; i++) set_key(SDL_SCANCODE_A + i, 'A' + i, letter_scan[i], 0);
    for (i = 0; i < 9; i++) set_key(SDL_SCANCODE_1 + i, '1' + i, 2 + i, 0);
    set_key(SDL_SCANCODE_0, '0', 11, 0);
    for (i = 0; i < 10; i++) set_key(SDL_SCANCODE_F1 + i, 0x70 + i, 59 + i, 0);
    set_key(SDL_SCANCODE_F11, 0x7A, 87, 0);
    set_key(SDL_SCANCODE_F12, 0x7B, 88, 0);

    set_key(SDL_SCANCODE_ESCAPE, 0x1B, 1, 0);
    set_key(SDL_SCANCODE_MINUS, 0xBD, 12, 0);
    set_key(SDL_SCANCODE_EQUALS, 0xBB, 13, 0);
    set_key(SDL_SCANCODE_BACKSPACE, 0x08, 14, 0);
    set_key(SDL_SCANCODE_TAB, 0x09, 15, 0);
    set_key(SDL_SCANCODE_LEFTBRACKET, 0xDB, 26, 0);
    set_key(SDL_SCANCODE_RIGHTBRACKET, 0xDD, 27, 0);
    set_key(SDL_SCANCODE_RETURN, 0x0D, 28, 0);
    set_key(SDL_SCANCODE_LCTRL, 0x11, 29, 0);
    set_key(SDL_SCANCODE_SEMICOLON, 0xBA, 39, 0);
    set_key(SDL_SCANCODE_APOSTROPHE, 0xDE, 40, 0);
    set_key(SDL_SCANCODE_GRAVE, 0xC0, 41, 0);
    set_key(SDL_SCANCODE_LSHIFT, 0x10, 42, 0);
    set_key(SDL_SCANCODE_BACKSLASH, 0xDC, 43, 0);
    set_key(SDL_SCANCODE_COMMA, 0xBC, 51, 0);
    set_key(SDL_SCANCODE_PERIOD, 0xBE, 52, 0);
    set_key(SDL_SCANCODE_SLASH, 0xBF, 53, 0);
    set_key(SDL_SCANCODE_RSHIFT, 0x10, 54, 0);
    set_key(SDL_SCANCODE_KP_MULTIPLY, 0x6A, 55, 0);
    set_key(SDL_SCANCODE_LALT, 0x12, 56, 0);
    set_key(SDL_SCANCODE_SPACE, 0x20, 57, 0);
    set_key(SDL_SCANCODE_CAPSLOCK, 0x14, 58, 0);
    set_key(SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 69, 0);
    set_key(SDL_SCANCODE_SCROLLLOCK, 0x91, 70, 0);
    set_key(SDL_SCANCODE_KP_7, 0x67, 71, 0);
    set_key(SDL_SCANCODE_KP_8, 0x68, 72, 0);
    set_key(SDL_SCANCODE_KP_9, 0x69, 73, 0);
    set_key(SDL_SCANCODE_KP_MINUS, 0x6D, 74, 0);
    set_key(SDL_SCANCODE_KP_4, 0x64, 75, 0);
    set_key(SDL_SCANCODE_KP_5, 0x65, 76, 0);
    set_key(SDL_SCANCODE_KP_6, 0x66, 77, 0);
    set_key(SDL_SCANCODE_KP_PLUS, 0x6B, 78, 0);
    set_key(SDL_SCANCODE_KP_1, 0x61, 79, 0);
    set_key(SDL_SCANCODE_KP_2, 0x62, 80, 0);
    set_key(SDL_SCANCODE_KP_3, 0x63, 81, 0);
    set_key(SDL_SCANCODE_KP_0, 0x60, 82, 0);
    set_key(SDL_SCANCODE_KP_PERIOD, 0x6E, 83, 0);
    set_key(SDL_SCANCODE_PAUSE, 0x13, 69, 0);

    // extended keys
    set_key(SDL_SCANCODE_KP_ENTER, 0x0D, 28, 1);
    set_key(SDL_SCANCODE_RCTRL, 0x11, 29, 1);
    set_key(SDL_SCANCODE_KP_DIVIDE, 0x6F, 53, 1);
    set_key(SDL_SCANCODE_RALT, 0x12, 56, 1);
    set_key(SDL_SCANCODE_HOME, 0x24, 71, 1);
    set_key(SDL_SCANCODE_UP, 0x26, 72, 1);
    set_key(SDL_SCANCODE_PAGEUP, 0x21, 73, 1);
    set_key(SDL_SCANCODE_LEFT, 0x25, 75, 1);
    set_key(SDL_SCANCODE_RIGHT, 0x27, 77, 1);
    set_key(SDL_SCANCODE_END, 0x23, 79, 1);
    set_key(SDL_SCANCODE_DOWN, 0x28, 80, 1);
    set_key(SDL_SCANCODE_PAGEDOWN, 0x22, 81, 1);
    set_key(SDL_SCANCODE_INSERT, 0x2D, 82, 1);
    set_key(SDL_SCANCODE_DELETE, 0x2E, 83, 1);
}

// US keyboard layout: VK -> character
static int vk_to_char(uint32_t vk, int shift, int caps)
{
    static const char *shifted_digits = ")!@#$%^&*(";

    if (vk >= 'A' && vk <= 'Z')
    {
        return ((shift != 0) != (caps != 0)) ? (int)vk : (int)(vk + 32);
    }
    if (vk >= '0' && vk <= '9') return shift ? shifted_digits[vk - '0'] : (int)vk;
    if (vk >= 0x60 && vk <= 0x69) return '0' + (vk - 0x60);
    switch (vk)
    {
        case 0x20: return ' ';
        case 0x0D: return '\r';
        case 0x08: return '\b';
        case 0x09: return '\t';
        case 0x1B: return 0x1B;
        case 0x6A: return '*';
        case 0x6B: return '+';
        case 0x6D: return '-';
        case 0x6E: return '.';
        case 0x6F: return '/';
        case 0xBA: return shift ? ':' : ';';
        case 0xBB: return shift ? '+' : '=';
        case 0xBC: return shift ? '<' : ',';
        case 0xBD: return shift ? '_' : '-';
        case 0xBE: return shift ? '>' : '.';
        case 0xBF: return shift ? '?' : '/';
        case 0xC0: return shift ? '~' : '`';
        case 0xDB: return shift ? '{' : '[';
        case 0xDC: return shift ? '|' : '\\';
        case 0xDD: return shift ? '}' : ']';
        case 0xDE: return shift ? '"' : '\'';
    }
    return 0;
}

static void update_modifier_vks(void)
{
    const uint8_t *keys = host_keyboard_state();

    key_state[0xA0] = keys[SDL_SCANCODE_LSHIFT] ? 0x80 : 0;
    key_state[0xA1] = keys[SDL_SCANCODE_RSHIFT] ? 0x80 : 0;
    key_state[0xA2] = keys[SDL_SCANCODE_LCTRL] ? 0x80 : 0;
    key_state[0xA3] = keys[SDL_SCANCODE_RCTRL] ? 0x80 : 0;
    key_state[0xA4] = keys[SDL_SCANCODE_LALT] ? 0x80 : 0;
    key_state[0xA5] = keys[SDL_SCANCODE_RALT] ? 0x80 : 0;
    key_state[0x10] = (key_state[0xA0] | key_state[0xA1]) | (key_state[0x10] & 1);
    key_state[0x11] = (key_state[0xA2] | key_state[0xA3]) | (key_state[0x11] & 1);
    key_state[0x12] = (key_state[0xA4] | key_state[0xA5]) | (key_state[0x12] & 1);
    // Caps Lock and Num Lock keep the toggle state handle_key gives them
}

static void handle_key(SDL_KeyboardEvent *ev, int down)
{
    key_map *k;
    uint32_t lparam, msg;
    int alt;

    if (ev->keysym.scancode >= SDL_NUM_SCANCODES) return;
    k = &sdl_keys[ev->keysym.scancode];
    if (k->vk == 0) return;

    // Alt+Enter: toggle full screen (handled here, not passed to the game)
    if (down && (k->vk == 0x0D) && (ev->keysym.mod & KMOD_ALT))
    {
        host_toggle_fullscreen();
        return;
    }

    if (down)
    {
        if (!(key_state[k->vk] & 0x80)) key_state[k->vk] ^= 1; // toggle state
        key_state[k->vk] |= 0x80;
        key_pressed[k->vk] = 1;
    }
    else
    {
        key_state[k->vk] &= ~0x80;
    }
    update_modifier_vks();

    alt = (ev->keysym.mod & KMOD_ALT) != 0;
    lparam = 1 | ((uint32_t)k->scan << 16) | (k->ext ? (1u << 24) : 0);
    if (alt) lparam |= 1u << 29;
    if (down)
    {
        if (ev->repeat) lparam |= 1u << 30;
        msg = (alt || k->vk == 0x12) ? WM_SYSKEYDOWN : WM_KEYDOWN;
    }
    else
    {
        lparam |= (1u << 30) | (1u << 31);
        msg = (alt || k->vk == 0x12) ? WM_SYSKEYUP : WM_KEYUP;
    }

    if (winapi_debug >= 2) eprintf("key %s vk 0x%x scan %u\n", down ? "down" : "up", k->vk, k->scan);
    if (focus_window != NULL) post_message(focus_window, msg, k->vk, lparam);
}

int16_t CCALL GetKeyState_c(int32_t nVirtKey)
{
    uint8_t s = key_state[nVirtKey & 0xff];
    // like Windows: a pressed key gives -128 / -127 (0xFF80 | toggle), i.e. more bits than 0x8000 are set -
    // the game tests Ctrl/Shift/Alt with GetKeyState(vk) & 0x1000 (sub_44E920: cheat codes, modifier keys)
    return (int16_t)(((s & 0x80) ? 0xFF80 : 0) | (s & 1));
}

int16_t CCALL GetAsyncKeyState_c(int32_t nVirtKey)
{
    static uint32_t last_pump;
    uint32_t now;
    int vk = nVirtKey & 0xff;
    int16_t res;

    now = winapi_get_ticks();
    if (now - last_pump > 4)
    {
        last_pump = now;
        winapi_process_events();
    }

    // mouse buttons
    if (vk >= 1 && vk <= 4)
    {
        uint32_t b = host_mouse_state(NULL, NULL);
        if (script_buttons & MK_LBUTTON) b |= SDL_BUTTON_LMASK;
        if (script_buttons & MK_RBUTTON) b |= SDL_BUTTON_RMASK;
        int down = (vk == 1) ? (b & SDL_BUTTON_LMASK) : (vk == 2) ? (b & SDL_BUTTON_RMASK) : (vk == 4) ? (b & SDL_BUTTON_MMASK) : 0;
        return down ? (int16_t)0x8000 : 0;
    }

    res = (int16_t)(((key_state[vk] & 0x80) ? 0x8000 : 0) | (key_pressed[vk] ? 1 : 0));
    key_pressed[vk] = 0;
    return res;
}

uint32_t CCALL MapVirtualKeyA_c(uint32_t uCode, uint32_t uMapType)
{
    int i;

    switch (uMapType)
    {
        case 0: // VK -> scan code
            for (i = 0; i < SDL_NUM_SCANCODES; i++)
            {
                if (sdl_keys[i].vk == uCode && sdl_keys[i].vk != 0) return sdl_keys[i].scan;
            }
            return 0;
        case 1: // scan code -> VK
        case 3:
            for (i = 0; i < SDL_NUM_SCANCODES; i++)
            {
                if (sdl_keys[i].scan == uCode && sdl_keys[i].vk != 0 && !sdl_keys[i].ext) return sdl_keys[i].vk;
            }
            return 0;
        case 2: // VK -> char
            return (uint32_t) vk_to_char(uCode, 0, 0);
    }
    return 0;
}

uint32_t CCALL GetKeyboardState_c(uint8_t *lpKeyState)
{
    if (lpKeyState == NULL) return 0;
    memcpy(lpKeyState, key_state, 256);
    return 1;
}

int32_t CCALL ToAscii_c(uint32_t uVirtKey, uint32_t uScanCode, const uint8_t *lpKeyState, uint16_t *lpChar, uint32_t uFlags)
{
    int shift, caps, c;

    if (lpKeyState == NULL) return 0;
    shift = (lpKeyState[0x10] & 0x80) != 0;
    caps = (lpKeyState[0x14] & 1) != 0;
    c = vk_to_char(uVirtKey, shift, caps);
    if (c == 0) return 0;
    if (lpChar != NULL) *lpChar = (uint16_t) c;
    return 1;
}

int32_t CCALL GetKeyboardType_c(int32_t nTypeFlag)
{
    switch (nTypeFlag)
    {
        case 0: return 4;   // IBM enhanced (101/102-key)
        case 1: return 0;
        case 2: return 12;  // number of function keys
    }
    return 0;
}



/* ------------------------------------------------------------------ */
/* scripted input (debugging): AVP2_INPUT_SCRIPT=<file>                 */
/*   <ms> click <x> <y> [right]   <ms> down|up <x> <y> [right]         */
/*   <ms> move <x> <y>            <ms> key|keydown|keyup <vk>          */
/*   <ms> quit                    <ms> close (WM_CLOSE)               */
/*   <ms> skey|sdown|sup <SDL key name> (also seen by DirectInput)     */
/*   <ms> rel <dx> <dy> (relative mouse motion, DirectInput: looking)  */
/*   <ms> mdown|mup|mclick <SDL button: 1 left, 3 right> (DirectInput) */
/* <vk>: hex/decimal number, a letter/digit, or ESCAPE RETURN SPACE    */
/* UP DOWN LEFT RIGHT TAB F1..F12                                      */

typedef struct {
    uint32_t time;
    char cmd[16];
    int32_t a, b, c;
} script_event;

static script_event *script;
static int script_count, script_pos, script_loaded;

static int parse_vk(const char *s)
{
    static const struct { const char *name; int vk; } names[] = {
        {"ESCAPE",0x1B},{"ESC",0x1B},{"RETURN",0x0D},{"ENTER",0x0D},{"SPACE",0x20},{"TAB",0x09},
        {"UP",0x26},{"DOWN",0x28},{"LEFT",0x25},{"RIGHT",0x27},{"SHIFT",0x10},{"CONTROL",0x11},{"CTRL",0x11},
        {"ALT",0x12},{"BACK",0x08},{"PGUP",0x21},{"PGDN",0x22},{"HOME",0x24},{"END",0x23},{NULL,0}
    };
    int i;
    if (s[0] == 'F' && s[1] >= '1' && s[1] <= '9') return 0x70 + atoi(s + 1) - 1;
    for (i = 0; names[i].name != NULL; i++) if (0 == strcasecmp(s, names[i].name)) return names[i].vk;
    if (s[1] == 0 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    if (s[1] == 0 && (s[0] >= 'a' && s[0] <= 'z')) return s[0] - 32;
    return (int) strtol(s, NULL, 0);
}

static void load_script(void)
{
    const char *name;
    FILE *f;
    char line[256], arg[64];

    script_loaded = 1;
    name = getenv("AVP2_INPUT_SCRIPT");
    if (name == NULL) return;
    f = fopen(name, "rt");
    if (f == NULL) { eprintf("input script not found: %s\n", name); return; }
    while (fgets(line, sizeof(line), f) != NULL)
    {
        script_event e;
        int n;
        memset(&e, 0, sizeof(e));
        arg[0] = 0;
        n = sscanf(line, "%u %15s %63s %d %d", &e.time, e.cmd, arg, &e.b, &e.c);
        if (n < 2 || line[0] == '#') continue;
        if (0 == strncmp(e.cmd, "key", 3)) e.a = parse_vk(arg);
        else if ((0 == strcmp(e.cmd, "skey")) || (0 == strcmp(e.cmd, "sdown")) || (0 == strcmp(e.cmd, "sup")))
        {
            e.a = (int) SDL_GetScancodeFromName(arg);
            if (e.a == SDL_SCANCODE_UNKNOWN) eprintf("input script: unknown key %s\n", arg);
        }
        else { e.a = atoi(arg); }
        if ((0 == strcmp(e.cmd, "click") || 0 == strcmp(e.cmd, "down") || 0 == strcmp(e.cmd, "up")) && n >= 5) e.c = 1; else if (0 != strncmp(e.cmd, "key", 3)) e.c = 0;
        script = (script_event *) realloc(script, (script_count + 1) * sizeof(script_event));
        script[script_count++] = e;
    }
    fclose(f);
    eprintf("input script: %d events\n", script_count);
}

static void script_key(int vk, int down)
{
    uint32_t scan = MapVirtualKeyA_c(vk, 0);
    uint32_t lparam = 1 | (scan << 16);
    if (down)
    {
        key_state[vk & 0xff] |= 0x80;
        key_pressed[vk & 0xff] = 1;
    }
    else
    {
        key_state[vk & 0xff] &= ~0x80;
        lparam |= (1u << 30) | (1u << 31);
    }
    if (focus_window != NULL) post_message(focus_window, down ? WM_KEYDOWN : WM_KEYUP, vk, lparam);
}

// a key event like the window system's (DirectInput sees it too)
static void dinput_event_and_key(SDL_Scancode sc, int down);

static void script_mouse(int x, int y, int button, int down)
{
    uint32_t lparam = ((uint32_t)(uint16_t)y << 16) | (uint16_t)x;
    script_mouse_active = 1;
    script_mouse_x = x;
    script_mouse_y = y;
    if (main_window == NULL) return;
    post_message(main_window, WM_MOUSEMOVE, script_buttons, lparam);
    if (button < 0) return;
    if (down) script_buttons |= button ? MK_RBUTTON : MK_LBUTTON;
    else script_buttons &= ~(button ? MK_RBUTTON : MK_LBUTTON);
    post_message(main_window, button ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP), script_buttons, lparam);
}

static void run_script(void)
{
    uint32_t now;

    if (!script_loaded) load_script();
    now = winapi_get_ticks();
    while ((script_pos < script_count) && (script[script_pos].time <= now))
    {
        script_event *e = &script[script_pos++];
        if (winapi_debug) eprintf("input script: %u %s %d %d %d\n", e->time, e->cmd, e->a, e->b, e->c);
        if (0 == strcmp(e->cmd, "key")) { script_key(e->a, 1); script_key(e->a, 0); }
        else if (0 == strcmp(e->cmd, "keydown")) script_key(e->a, 1);
        else if (0 == strcmp(e->cmd, "keyup")) script_key(e->a, 0);
        else if (0 == strcmp(e->cmd, "move")) script_mouse(e->a, e->b, -1, 0);
        else if (0 == strcmp(e->cmd, "down")) script_mouse(e->a, e->b, e->c, 1);
        else if (0 == strcmp(e->cmd, "up")) script_mouse(e->a, e->b, e->c, 0);
        else if (0 == strcmp(e->cmd, "click")) { script_mouse(e->a, e->b, e->c, 1); script_mouse(e->a, e->b, e->c, 0); }
        else if (0 == strcmp(e->cmd, "quit")) { eprintf("input script: quit\n"); app_exit(0); }
        else if (0 == strcmp(e->cmd, "skey")) { dinput_event_and_key((SDL_Scancode) e->a, 1); dinput_event_and_key((SDL_Scancode) e->a, 0); }
        else if (0 == strcmp(e->cmd, "sdown")) dinput_event_and_key((SDL_Scancode) e->a, 1);
        else if (0 == strcmp(e->cmd, "sup")) dinput_event_and_key((SDL_Scancode) e->a, 0);
        else if ((0 == strcmp(e->cmd, "mdown")) || (0 == strcmp(e->cmd, "mup")) || (0 == strcmp(e->cmd, "mclick")))
        {
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.button.button = (Uint8) e->a;
            if (strcmp(e->cmd, "mup") != 0)
            {
                ev.type = SDL_MOUSEBUTTONDOWN;
                ev.button.state = SDL_PRESSED;
                dinput_event(&ev);
            }
            if (strcmp(e->cmd, "mdown") != 0)
            {
                ev.type = SDL_MOUSEBUTTONUP;
                ev.button.state = SDL_RELEASED;
                dinput_event(&ev);
            }
        }
        else if (0 == strcmp(e->cmd, "rel"))
        {
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SDL_MOUSEMOTION;
            ev.motion.xrel = e->a;
            ev.motion.yrel = e->b;
            dinput_event(&ev);
        }
        // the window's close button (the game shuts down by itself)
        else if (0 == strcmp(e->cmd, "close")) { if (main_window != NULL) post_message(main_window, WM_CLOSE, 0, 0); }
        else if (0 == strcmp(e->cmd, "mflood"))
        {
            // many mouse motion messages at once, like a high-rate mouse (tests message coalescing)
            int i;
            for (i = 0; i < e->a; i++) if (main_window != NULL) post_message(main_window, WM_MOUSEMOVE, 0, ((uint32_t)(240 + (i & 7)) << 16) | 320);
            if (winapi_debug) eprintf("input script: queue has %d messages\n", queue_count);
        }
        // real SDL mouse in window coordinates (tests the window -> client mapping)
        else if (0 == strcmp(e->cmd, "wmove")) { script_mouse_active = 0; display_warp_window(e->a, e->b); }
        else if ((0 == strcmp(e->cmd, "wdown")) || (0 == strcmp(e->cmd, "wup")))
        {
            int x, y, wx, wy, down = (e->cmd[1] == 'd');
            script_mouse_active = 0;
            host_mouse_state(&wx, &wy);
            display_window_to_client(wx, wy, &x, &y);
            if (down) script_buttons |= MK_LBUTTON; else script_buttons &= ~MK_LBUTTON;
            if (main_window != NULL) post_message(main_window, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, script_buttons, ((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
            if (winapi_debug) eprintf("input script: window %d,%d -> client %d,%d\n", wx, wy, x, y);
        }
    }
}

/* ------------------------------------------------------------------ */
/* events                                                              */

static int cursor_show_count;


static void update_cursor_visibility(void)
{
    host_show_cursor(cursor_show_count >= 0);
}

static uint32_t mouse_wparam(void)
{
    uint32_t b = host_mouse_state(NULL, NULL);
    uint32_t w = script_buttons;
    if (b & SDL_BUTTON_LMASK) w |= MK_LBUTTON;
    if (b & SDL_BUTTON_RMASK) w |= MK_RBUTTON;
    if (b & SDL_BUTTON_MMASK) w |= MK_MBUTTON;
    if (key_state[0x10] & 0x80) w |= MK_SHIFT;
    if (key_state[0x11] & 0x80) w |= MK_CONTROL;
    return w;
}

static void set_active(int active)
{
    int i;

    if (active == app_active) return;
    app_active = active;
    if (winapi_debug) eprintf("user32: application %s\n", active ? "activated" : "deactivated");
    if (main_window == NULL) return;

    // the opposite change still waiting in the queue: both cancel (the engine restarts its renderer on every
    // deactivation, and the window system reports a short loss of focus while the window is being set up)
    for (i = queue_count - 1; i >= 0; i--)
    {
        win_msg *q = &queue[(queue_head + i) % QUEUE_SIZE];
        if ((q->message == WM_ACTIVATEAPP) && (q->hwnd == main_window))
        {
            int j;
            for (j = i; j < queue_count - 1; j++) queue[(queue_head + j) % QUEUE_SIZE] = queue[(queue_head + j + 1) % QUEUE_SIZE];
            queue_count--;
            return;
        }
    }
    post_message(main_window, WM_ACTIVATEAPP, active ? 1 : 0, 0);
}

static void dinput_event_and_key(SDL_Scancode sc, int down)
{
    SDL_Event ev;
    if ((sc <= SDL_SCANCODE_UNKNOWN) || (sc >= SDL_NUM_SCANCODES)) return;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.scancode = sc;
    ev.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    dinput_event(&ev);
    handle_key(&ev.key, down);
}

void winapi_process_events(void)
{
    SDL_Event ev;
    int x, y;

    while (host_poll_event(&ev))
    {
        dinput_event(&ev);
        switch (ev.type)
        {
            case SDL_QUIT:
                if (main_window != NULL) post_message(main_window, WM_CLOSE, 0, 0);
                break;
            case SDL_KEYDOWN:
                handle_key(&ev.key, 1);
                break;
            case SDL_KEYUP:
                handle_key(&ev.key, 0);
                break;
            case SDL_MOUSEMOTION:
                display_window_to_client(ev.motion.x, ev.motion.y, &x, &y);
                if (main_window != NULL) post_message(main_window, WM_MOUSEMOVE, mouse_wparam(), ((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            {
                uint32_t msg = 0;
                display_window_to_client(ev.button.x, ev.button.y, &x, &y);
                if (ev.button.button == SDL_BUTTON_LEFT) msg = (ev.type == SDL_MOUSEBUTTONDOWN) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                else if (ev.button.button == SDL_BUTTON_RIGHT) msg = (ev.type == SDL_MOUSEBUTTONDOWN) ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                else if (ev.button.button == SDL_BUTTON_MIDDLE) msg = (ev.type == SDL_MOUSEBUTTONDOWN) ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                if (msg && (main_window != NULL)) post_message(main_window, msg, mouse_wparam(), ((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
                break;
            }
            case SDL_WINDOWEVENT:
                switch (ev.window.event)
                {
                    case SDL_WINDOWEVENT_FOCUS_GAINED:
                        set_active(1);
                        break;
                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        set_active(0);
                        memset(key_state, 0, sizeof(key_state));
                        break;
                    case SDL_WINDOWEVENT_EXPOSED:
                        display_present(1);
                        break;
                }
                break;
        }
    }

    run_script();
    display_present(0);
    display_idle();
}


/* ------------------------------------------------------------------ */
/* message loop                                                        */

#define PM_REMOVE 0x0001

uint32_t CCALL PeekMessageA_c(win_msg *lpMsg, void *hWnd, uint32_t wMsgFilterMin, uint32_t wMsgFilterMax, uint32_t wRemoveMsg)
{
    int i;

    winapi_process_events();

    for (i = 0; i < queue_count; i++)
    {
        win_msg *m = &queue[(queue_head + i) % QUEUE_SIZE];

        if ((hWnd != NULL) && (m->hwnd != hWnd)) continue;
        if ((wMsgFilterMin != 0 || wMsgFilterMax != 0) && ((m->message < wMsgFilterMin) || (m->message > wMsgFilterMax))) continue;

        if (lpMsg != NULL) *lpMsg = *m;
        if (wRemoveMsg & PM_REMOVE)
        {
            // remove entry i
            int j;
            for (j = i; j > 0; j--)
            {
                queue[(queue_head + j) % QUEUE_SIZE] = queue[(queue_head + j - 1) % QUEUE_SIZE];
            }
            queue_head = (queue_head + 1) % QUEUE_SIZE;
            queue_count--;
        }
        return 1;
    }
    return 0;
}

uint32_t CCALL TranslateMessage_c(const win_msg *lpMsg)
{
    int c;

    if (lpMsg == NULL) return 0;
    if ((lpMsg->message != WM_KEYDOWN) && (lpMsg->message != WM_SYSKEYDOWN)) return 0;

    c = vk_to_char(lpMsg->wParam, (key_state[0x10] & 0x80) != 0, key_state[0x14] & 1);
    if (c == 0) return 0;
    if (key_state[0x11] & 0x80)
    {
        // Ctrl+letter -> control character
        if (c >= 'a' && c <= 'z') c -= 'a' - 1;
        else if (c >= 'A' && c <= 'Z') c -= 'A' - 1;
        else return 0;
    }
    post_message(lpMsg->hwnd, (lpMsg->message == WM_SYSKEYDOWN) ? WM_SYSCHAR : WM_CHAR, (uint32_t)c, lpMsg->lParam);
    return 1;
}

uint32_t CCALL DispatchMessageA_c(const win_msg *lpMsg)
{
    if (lpMsg == NULL) return 0;
    return winapi_call_wndproc(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
}

uint32_t CCALL SendMessageA_c(void *hWnd, uint32_t Msg, uint32_t wParam, uint32_t lParam)
{
    if ((winapi_debug >= 2) || (winapi_debug && Msg >= 0x400)) eprintf("SendMessageA: 0x%x 0x%x 0x%x\n", Msg, wParam, lParam);
    return winapi_call_wndproc(hWnd, Msg, wParam, lParam);
}

uint32_t CCALL PostMessageA_c(void *hWnd, uint32_t Msg, uint32_t wParam, uint32_t lParam)
{
    if (winapi_debug && Msg >= 0x400) eprintf("PostMessageA: 0x%x 0x%x 0x%x\n", Msg, wParam, lParam);
    post_message(hWnd, Msg, wParam, lParam);
    return 1;
}

void CCALL PostQuitMessage_c(int32_t nExitCode)
{
    if (winapi_debug) eprintf("PostQuitMessage: %d (0x%x)\n", nExitCode, nExitCode);
    post_message(NULL, WM_QUIT, (uint32_t) nExitCode, 0);
}

uint32_t CCALL DestroyWindow_c(void *hWnd);

uint32_t CCALL DefWindowProcA_c(void *hWnd, uint32_t Msg, uint32_t wParam, uint32_t lParam)
{
    switch (Msg)
    {
        case WM_CLOSE:
            DestroyWindow_c(hWnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            return 0;
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* window management                                                   */

typedef struct {
    uint32_t style;
    uint32_t lpfnWndProc;
    int32_t cbClsExtra;
    int32_t cbWndExtra;
    PTR32(void) hInstance;
    PTR32(void) hIcon;
    PTR32(void) hCursor;
    PTR32(void) hbrBackground;
    PTR32(const char) lpszMenuName;
    PTR32(const char) lpszClassName;
} wndclassa;

uint16_t CCALL RegisterClassA_c(const wndclassa *lpWndClass)
{
    window_class *c;

    if ((lpWndClass == NULL) || (num_classes >= MAX_CLASSES)) return 0;

    c = &classes[num_classes++];
    memset(c, 0, sizeof(window_class));
    if (lpWndClass->lpszClassName.get_value() >= 0x10000) strncpy(c->name, lpWndClass->lpszClassName, sizeof(c->name) - 1);
    c->wndproc = lpWndClass->lpfnWndProc;
    c->cbWndExtra = lpWndClass->cbWndExtra;
    c->style = lpWndClass->style;
    c->hcursor = lpWndClass->hCursor.get_value();
    c->hicon = lpWndClass->hIcon.get_value();
    if (winapi_debug) eprintf("RegisterClassA: %s\n", c->name);
    return (uint16_t)(0xc000 + num_classes);
}

static window_class *find_class(const char *name)
{
    int i;

    if (H2G(name) < 0x10000)      // an atom from RegisterClass
    {
        i = (int)H2G(name) - 0xc001;
        return (i >= 0 && i < num_classes) ? &classes[i] : NULL;
    }
    for (i = 0; i < num_classes; i++)
    {
        if (0 == strcasecmp(classes[i].name, name)) return &classes[i];
    }
    return NULL;
}

void * CCALL CreateWindowExA_c(uint32_t dwExStyle, const char *lpClassName, const char *lpWindowName, uint32_t dwStyle, int32_t x, int32_t y, int32_t nWidth, int32_t nHeight, void *hWndParent, void *hMenu, void *hInstance, void *lpParam)
{
    window *w;
    window_class *c;
    // CREATESTRUCTA: the game gets a pointer to it, so it must be in low memory (not on the C stack)
    static uint32_t *cs;      // the game reads it: game memory
    if (cs == NULL) cs = (uint32_t *) game_calloc(12, 4);

    c = find_class(lpClassName);
    if (c == NULL)
    {
        eprintf("CreateWindowExA: unknown class\n");
        return NULL;
    }

    w = (window *) game_calloc(1, sizeof(window));
    w->magic = WINDOW_MAGIC;
    w->cls = c;
    w->wndproc = c->wndproc;
    w->style = dwStyle;
    w->exstyle = dwExStyle;
    w->x = 0;
    w->y = 0;
    w->width = (nWidth > 0 && nWidth < 4096) ? nWidth : 640;
    w->height = (nHeight > 0 && nHeight < 4096) ? nHeight : 480;
    if (lpWindowName != NULL) strncpy(w->title, lpWindowName, sizeof(w->title) - 1);

    if (winapi_debug) eprintf("CreateWindowExA: %s \"%s\" %dx%d style 0x%x\n", c->name, w->title, w->width, w->height, dwStyle);

    if (main_window == NULL)
    {
        main_window = w;
        focus_window = w;
        if (!display_create(w->title, w->width, w->height))
        {
            game_free(w);
            main_window = NULL;
            return NULL;
        }
    }

    // CREATESTRUCTA
    cs[0] = H2G(lpParam);
    cs[1] = H2G(hInstance);
    cs[2] = H2G(hMenu);
    cs[3] = H2G(hWndParent);
    cs[4] = (uint32_t) w->height;
    cs[5] = (uint32_t) w->width;
    cs[6] = (uint32_t) y;
    cs[7] = (uint32_t) x;
    cs[8] = dwStyle;
    cs[9] = H2G(lpWindowName);
    cs[10] = H2G(lpClassName);
    cs[11] = dwExStyle;

    if ((int32_t)winapi_call_wndproc(w, WM_CREATE, 0, H2G(cs)) == -1)
    {
        if (main_window == w) main_window = NULL;
        game_free(w);
        return NULL;
    }
    winapi_call_wndproc(w, WM_SIZE, 0, ((uint32_t)w->height << 16) | (uint32_t)w->width);

    return w;
}

uint32_t CCALL DestroyWindow_c(void *hWnd)
{
    window *w = get_window(hWnd);
    if (w == NULL) return 0;

    winapi_call_wndproc(w, WM_DESTROY, 0, 0);
    if (w == main_window)
    {
        main_window = NULL;
        focus_window = NULL;
        display_destroy();
    }
    w->magic = 0;
    return 1;
}

uint32_t CCALL ShowWindow_c(void *hWnd, int32_t nCmdShow)
{
    window *w = get_window(hWnd);
    int was_visible;

    if (w == NULL) return 0;
    was_visible = w->visible;
    w->visible = (nCmdShow != 0);
    if (w->visible && !was_visible && (w == main_window))
    {
        winapi_call_wndproc(w, WM_ACTIVATEAPP, 1, 0);
        winapi_call_wndproc(w, WM_SETFOCUS, 0, 0);
        app_active = 1;
    }
    return was_visible;
}

uint32_t CCALL UpdateWindow_c(void *hWnd)
{
    if (get_window(hWnd) == NULL) return 0;
    winapi_call_wndproc(hWnd, WM_PAINT, 0, 0);
    return 1;
}

uint32_t CCALL SetWindowPos_c(void *hWnd, void *hWndInsertAfter, int32_t X, int32_t Y, int32_t cx, int32_t cy, uint32_t uFlags)
{
    window *w = get_window(hWnd);
    if (w == NULL) return 0;
    if (!(uFlags & 0x0001)) // SWP_NOSIZE
    {
        if ((cx > 0) && (cy > 0) && ((cx != w->width) || (cy != w->height)))
        {
            if (winapi_debug) eprintf("SetWindowPos: %dx%d\n", cx, cy);
            w->width = cx;
            w->height = cy;
            if (w == main_window) display_resize(cx, cy);
        }
    }
    return 1;
}

int32_t CCALL GetWindowLongA_c(void *hWnd, int32_t nIndex)
{
    window *w = get_window(hWnd);
    if (w == NULL) return 0;
    switch (nIndex)
    {
        case -4: return (int32_t) w->wndproc;
        case -16: return (int32_t) w->style;
        case -20: return (int32_t) w->exstyle;
        case -21: return (int32_t) w->userdata;
    }
    if ((nIndex >= 0) && (nIndex + 4 <= (int32_t)sizeof(w->extra))) return *(int32_t *)(w->extra + nIndex);
    return 0;
}

int32_t CCALL SetWindowLongA_c(void *hWnd, int32_t nIndex, int32_t dwNewLong)
{
    window *w = get_window(hWnd);
    int32_t old;

    if (w == NULL) return 0;
    old = GetWindowLongA_c(hWnd, nIndex);
    switch (nIndex)
    {
        case -4: w->wndproc = (wndproc_t) dwNewLong; return old;
        case -16: w->style = (uint32_t) dwNewLong; return old;
        case -20: w->exstyle = (uint32_t) dwNewLong; return old;
        case -21: w->userdata = (uint32_t) dwNewLong; return old;
    }
    if ((nIndex >= 0) && (nIndex + 4 <= (int32_t)sizeof(w->extra))) *(int32_t *)(w->extra + nIndex) = dwNewLong;
    return old;
}

typedef struct { int32_t left, top, right, bottom; } win_rect;
typedef struct { int32_t x, y; } win_point;

uint32_t CCALL GetClientRect_c(void *hWnd, win_rect *lpRect)
{
    window *w = get_window(hWnd);
    if ((w == NULL) || (lpRect == NULL)) return 0;
    lpRect->left = 0;
    lpRect->top = 0;
    lpRect->right = w->width;
    lpRect->bottom = w->height;
    return 1;
}

// the desktop: a window object (not shown) as big as the host's desktop
void * CCALL GetDesktopWindow_c(void)
{
    static window *desktop;
    if (desktop == NULL)
    {
        desktop = (window *) game_calloc(1, sizeof(window));
        desktop->magic = WINDOW_MAGIC;
        host_desktop_size(&desktop->width, &desktop->height);
        strcpy(desktop->title, "Desktop");
    }
    return desktop;
}

uint32_t CCALL IsIconic_c(void *hWnd) { return 0; }

// screen coordinates are client coordinates of the game window (no frames)
int32_t CCALL MapWindowPoints_c(void *hWndFrom, void *hWndTo, win_point *lpPoints, uint32_t cPoints) { return 0; }

// GCL_HCURSOR -12, GCL_HICON -14, GCL_WNDPROC -24, GCL_STYLE -26
uint32_t CCALL GetClassLongA_c(void *hWnd, int32_t nIndex)
{
    window *w = get_window(hWnd);
    if ((w == NULL) || (w->cls == NULL)) return 0;
    switch (nIndex)
    {
        case -12: return w->cls->hcursor;
        case -14: return w->cls->hicon;
        case -24: return w->cls->wndproc;
        case -26: return w->cls->style;
    }
    return 0;
}

uint32_t CCALL CallWindowProcA_c(uint32_t lpPrevWndFunc, void *hWnd, uint32_t Msg, uint32_t wParam, uint32_t lParam)
{
    uint32_t args[4] = { H2G(hWnd), Msg, wParam, lParam };
    if (lpPrevWndFunc == 0) return DefWindowProcA_c(hWnd, Msg, wParam, lParam);
    return call_game(lpPrevWndFunc, 4, args);
}

uint32_t CCALL GetWindowRect_c(void *hWnd, win_rect *lpRect)
{
    window *w = get_window(hWnd);
    if ((w == NULL) || (lpRect == NULL)) return 0;
    lpRect->left = w->x;
    lpRect->top = w->y;
    lpRect->right = w->x + w->width;
    lpRect->bottom = w->y + w->height;
    return 1;
}

uint32_t CCALL AdjustWindowRect_c(win_rect *lpRect, uint32_t dwStyle, uint32_t bMenu)
{
    // windows have no frame: client area == window area
    return (lpRect != NULL) ? 1 : 0;
}

uint32_t CCALL ClientToScreen_c(void *hWnd, win_point *lpPoint) { return (lpPoint != NULL) ? 1 : 0; }
uint32_t CCALL ScreenToClient_c(void *hWnd, win_point *lpPoint) { return (lpPoint != NULL) ? 1 : 0; }

uint32_t CCALL SetRect_c(win_rect *lprc, int32_t xLeft, int32_t yTop, int32_t xRight, int32_t yBottom)
{
    if (lprc == NULL) return 0;
    lprc->left = xLeft;
    lprc->top = yTop;
    lprc->right = xRight;
    lprc->bottom = yBottom;
    return 1;
}

int32_t CCALL GetSystemMetrics_c(int32_t nIndex)
{
    switch (nIndex)
    {
        case 0: return 640;     // SM_CXSCREEN
        case 1: return 480;     // SM_CYSCREEN
        case 4: return 0;       // SM_CYCAPTION (windows have no caption)
        case 5: case 6: return 0; // SM_CXBORDER, SM_CYBORDER
        case 7: case 8: return 0; // SM_CXDLGFRAME, SM_CYDLGFRAME
        case 15: return 0;      // SM_CYMENU
        case 32: case 33: return 0; // SM_CXFRAME, SM_CYFRAME
    }
    return 0;
}

void * CCALL GetFocus_c(void) { return focus_window; }

void * CCALL SetFocus_c(void *hWnd)
{
    void *old = focus_window;
    if (get_window(hWnd) != NULL) focus_window = (window *) hWnd;
    return old;
}

uint32_t CCALL SetMenu_c(void *hWnd, void *hMenu) { return 1; }
void * CCALL ImmAssociateContext_c(void *hWnd, void *hIMC) { return NULL; }

uint32_t CCALL ValidateRect_c(void *hWnd, const win_rect *lpRect) { return 1; }

typedef struct {
    PTR32(void) hdc;
    uint32_t fErase;
    win_rect rcPaint;
    uint32_t fRestore;
    uint32_t fIncUpdate;
    uint8_t rgbReserved[32];
} paintstruct;

void * CCALL BeginPaint_c(void *hWnd, paintstruct *lpPaint)
{
    window *w = get_window(hWnd);
    if ((w == NULL) || (lpPaint == NULL)) return NULL;
    memset(lpPaint, 0, sizeof(paintstruct));
    lpPaint->hdc = GetDC_c(hWnd);
    lpPaint->rcPaint.right = w->width;
    lpPaint->rcPaint.bottom = w->height;
    return lpPaint->hdc;
}

uint32_t CCALL EndPaint_c(void *hWnd, const paintstruct *lpPaint)
{
    if (lpPaint != NULL) ReleaseDC_c(hWnd, lpPaint->hdc);
    return 1;
}


/* ------------------------------------------------------------------ */
/* cursor                                                              */

// HCURSOR / HICON: objects in game memory (the game keeps them in 32-bit variables)
#define CURSOR_MAGIC 0x53525543     // "CURS"
#define ICON_MAGIC   0x4e4f4349     // "ICON"

typedef struct {
    uint32_t magic;
    uint32_t system_id;         // IDC_* of a system cursor, 0 for a cursor from resources
    void *host;                 // the window system's cursor
    int32_t hot_x, hot_y, width, height;
} win_cursor;

static win_cursor *current_cursor;

static win_cursor *get_cursor(void *h)
{
    if ((h != NULL) && (H2G(h) >= 0x10000) && (((win_cursor *)h)->magic == CURSOR_MAGIC)) return (win_cursor *)h;
    return NULL;
}

static uint32_t rd16u(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32u(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// a cursor resource (RT_CURSOR): hotspot, then a DIB of double height (XOR image above the AND mask)
static void *cursor_from_dib(const uint8_t *data, uint32_t size, int32_t *hot_x, int32_t *hot_y, int32_t *pw, int32_t *ph)
{
    const uint8_t *bi = data + 4, *colors, *xor_bits, *and_bits;
    uint32_t hdr, ncolors, xor_pitch, and_pitch;
    int32_t w, h, bpp, x, y;
    uint32_t *argb;
    void *c;

    if (size < 4 + 40) return NULL;
    *hot_x = (int32_t)rd16u(data);
    *hot_y = (int32_t)rd16u(data + 2);
    hdr = rd32u(bi);
    w = (int32_t)rd32u(bi + 4);
    h = (int32_t)rd32u(bi + 8) / 2;
    bpp = (int32_t)rd16u(bi + 14);
    ncolors = rd32u(bi + 32);
    if ((ncolors == 0) && (bpp <= 8)) ncolors = 1u << bpp;
    if ((w <= 0) || (h <= 0) || (w > 256) || (h > 256)) return NULL;
    colors = bi + hdr;
    xor_bits = colors + 4 * ((bpp <= 8) ? ncolors : 0);
    xor_pitch = ((w * bpp + 31) / 32) * 4;
    and_bits = xor_bits + xor_pitch * h;
    and_pitch = ((w + 31) / 32) * 4;
    if (and_bits + and_pitch * h > data + size) return NULL;

    argb = (uint32_t *) malloc((size_t)w * h * 4);
    if (argb == NULL) return NULL;
    for (y = 0; y < h; y++)
    {
        // DIB rows are bottom-up
        const uint8_t *xr = xor_bits + xor_pitch * (h - 1 - y);
        const uint8_t *ar = and_bits + and_pitch * (h - 1 - y);
        uint32_t *out = argb + w * y;
        for (x = 0; x < w; x++)
        {
            uint32_t rgb, a = 0xff, mask = (ar[x >> 3] >> (7 - (x & 7))) & 1;
            switch (bpp)
            {
                case 1:  rgb = rd32u(colors + 4 * ((xr[x >> 3] >> (7 - (x & 7))) & 1)) & 0xffffff; break;
                case 4:  rgb = rd32u(colors + 4 * ((xr[x >> 1] >> ((x & 1) ? 0 : 4)) & 15)) & 0xffffff; break;
                case 8:  rgb = rd32u(colors + 4 * xr[x]) & 0xffffff; break;
                case 24: rgb = xr[3 * x] | (xr[3 * x + 1] << 8) | (xr[3 * x + 2] << 16); break;
                case 32: rgb = rd32u(xr + 4 * x); a = rgb >> 24; rgb &= 0xffffff; break;
                default: rgb = 0; break;
            }
            if (mask)
            {
                // AND 1: transparent where the XOR colour is black; "invert the screen" pixels are drawn black
                if (rgb == 0) a = 0; else rgb = 0;
            }
            else if (bpp != 32) a = 0xff;
            out[x] = (a << 24) | rgb;
        }
    }
    c = host_create_cursor(argb, w, h, *hot_x, *hot_y);
    free(argb);
    *pw = w;
    *ph = h;
    return c;
}

static win_cursor *new_cursor(void)
{
    win_cursor *c = (win_cursor *) game_calloc(1, sizeof(win_cursor));
    c->magic = CURSOR_MAGIC;
    return c;
}

static win_cursor *system_cursor(uint32_t id)
{
    static win_cursor *cache[32];
    static const struct { uint32_t id; SDL_SystemCursor sdl; } map[] = {
        { 32512, SDL_SYSTEM_CURSOR_ARROW }, { 32513, SDL_SYSTEM_CURSOR_IBEAM }, { 32514, SDL_SYSTEM_CURSOR_WAIT },
        { 32515, SDL_SYSTEM_CURSOR_CROSSHAIR }, { 32642, SDL_SYSTEM_CURSOR_SIZENWSE }, { 32643, SDL_SYSTEM_CURSOR_SIZENESW },
        { 32644, SDL_SYSTEM_CURSOR_SIZEWE }, { 32645, SDL_SYSTEM_CURSOR_SIZENS }, { 32646, SDL_SYSTEM_CURSOR_SIZEALL },
        { 32648, SDL_SYSTEM_CURSOR_NO }, { 32649, SDL_SYSTEM_CURSOR_HAND }, { 32650, SDL_SYSTEM_CURSOR_WAITARROW },
    };
    unsigned int i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (map[i].id == id) break;
    if (i == sizeof(map) / sizeof(map[0])) i = 0;
    if (cache[i] == NULL)
    {
        cache[i] = new_cursor();
        cache[i]->system_id = map[i].id;
        cache[i]->host = host_system_cursor(map[i].sdl);
    }
    return cache[i];
}

// RT_GROUP_CURSOR -> the first RT_CURSOR image of the group
static win_cursor *load_cursor_resource(void *hInstance, const char *name)
{
    const uint8_t *image = winapi_module_image(hInstance), *e, *group, *cur;
    uint32_t size, gsize, count, id;
    win_cursor *c;

    if (image == NULL) return NULL;
    e = pe_resource_find(image, (H2G(name) < 0x10000) ? (uintptr_t)H2G(name) : (uintptr_t)name, 12);
    if (e == NULL) return NULL;
    group = (const uint8_t *)pe_resource_data(image, e, &gsize);
    count = rd16u(group + 4);
    if ((count == 0) || (gsize < 6 + 14)) return NULL;
    id = rd16u(group + 6 + 12);
    e = pe_resource_find(image, id, 1);
    if (e == NULL) return NULL;
    cur = (const uint8_t *)pe_resource_data(image, e, &size);

    c = new_cursor();
    c->host = cursor_from_dib(cur, size, &c->hot_x, &c->hot_y, &c->width, &c->height);
    if (winapi_debug) eprintf("cursor resource %p: %dx%d hotspot %d,%d%s\n", (void *)name, c->width, c->height, c->hot_x, c->hot_y, (c->host != NULL) ? "" : " (no host cursor)");
    return c;
}

void * CCALL LoadCursorA_c(void *hInstance, const char *lpCursorName)
{
    if (hInstance == NULL) return system_cursor(H2G(lpCursorName));
    return load_cursor_resource(hInstance, lpCursorName);
}

void * CCALL LoadCursorFromFileA_c(const char *lpFileName) { return system_cursor(32512); }

void * CCALL LoadIconA_c(void *hInstance, const char *lpIconName)
{
    static uint32_t *icon;
    if (icon == NULL)
    {
        icon = (uint32_t *) game_calloc(1, 16);
        icon[0] = ICON_MAGIC;
    }
    return icon;
}

// IMAGE_BITMAP 0, IMAGE_ICON 1, IMAGE_CURSOR 2
void * CCALL LoadImageA_c(void *hInst, const char *name, uint32_t type, int32_t cx, int32_t cy, uint32_t fuLoad)
{
    void *h = NULL;
    if (fuLoad & 0x10)  // LR_LOADFROMFILE
    {
        if (winapi_debug) eprintf("LoadImageA: file %s type %u (not supported)\n", name, type);
        return NULL;
    }
    switch (type)
    {
        case 2: h = LoadCursorA_c(hInst, name); break;
        case 1: h = LoadIconA_c(hInst, name); break;
        default: break;
    }
    if (winapi_debug) eprintf("LoadImageA: %p %s%p type %u -> %p\n", hInst, (H2G(name) < 0x10000) ? "#" : "", (void *)name, type, h);
    return h;
}

void * CCALL SetCursor_c(void *hCursor)
{
    win_cursor *old = current_cursor, *c = get_cursor(hCursor);
    current_cursor = c;
    if ((c != NULL) && (c->host != NULL)) host_set_cursor(c->host);
    return old;
}

int32_t CCALL ShowCursor_c(uint32_t bShow)
{
    cursor_show_count += bShow ? 1 : -1;
    update_cursor_visibility();
    return cursor_show_count;
}

uint32_t CCALL GetCursorPos_c(win_point *lpPoint)
{
    int x, y;

    if (lpPoint == NULL) return 0;
    winapi_process_events();
    if (script_mouse_active)
    {
        lpPoint->x = script_mouse_x;
        lpPoint->y = script_mouse_y;
        return 1;
    }
    host_mouse_state(&x, &y);
    // window coordinates -> client coordinates of the (letterboxed, scaled) picture
    display_window_to_client(x, y, &x, &y);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= display_width) x = display_width - 1;
    if (y >= display_height) y = display_height - 1;
    lpPoint->x = x;
    lpPoint->y = y;
    return 1;
}

uint32_t CCALL SetCursorPos_c(int32_t X, int32_t Y)
{
    display_warp_mouse(X, Y);
    return 1;
}

uint32_t CCALL ClipCursor_c(const win_rect *lpRect) { return 1; }


/* ------------------------------------------------------------------ */
/* misc                                                                */

void * CCALL FindWindowA_c(const char *lpClassName, const char *lpWindowName)
{
    // no other instance of the game is running
    return NULL;
}

int32_t CCALL MessageBoxA_c(void *hWnd, const char *lpText, const char *lpCaption, uint32_t uType)
{
    eprintf("MessageBox: %s: %s\n", (lpCaption != NULL) ? lpCaption : "", (lpText != NULL) ? lpText : "");
    if (getenv("AVP2_NO_MESSAGEBOX") == NULL)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, (lpCaption != NULL) ? lpCaption : "", (lpText != NULL) ? lpText : "", NULL);
    }
    // MB_YESNO(4)/MB_YESNOCANCEL(3) -> IDYES(6), otherwise IDOK(1)
    return ((uType & 0xf) == 4 || (uType & 0xf) == 3) ? 6 : 1;
}

int32_t CCALL wsprintfA_c(char *lpOut, const char *lpFmt, uint32_t *ap)
{
    return vsprintf_x86(lpOut, lpFmt, ap);
}

int32_t CCALL wvsprintfA_c(char *lpOut, const char *lpFmt, uint32_t *arglist)
{
    return vsprintf_x86(lpOut, lpFmt, arglist);
}

// dialogs are not supported (the game uses them only for rare error/network dialogs)
int32_t CCALL DialogBoxParamA_c(void *hInstance, const char *lpTemplateName, void *hWndParent, void *lpDialogFunc, uint32_t dwInitParam)
{
    eprintf("DialogBoxParamA: dialogs are not supported\n");
    return -1;
}
uint32_t CCALL EndDialog_c(void *hDlg, int32_t nResult) { return 1; }
void * CCALL GetDlgItem_c(void *hDlg, int32_t nIDDlgItem) { return NULL; }
uint32_t CCALL GetDlgItemTextA_c(void *hDlg, int32_t nIDDlgItem, char *lpString, int32_t cchMax) { if (lpString && cchMax > 0) lpString[0] = 0; return 0; }
uint32_t CCALL SetDlgItemTextA_c(void *hDlg, int32_t nIDDlgItem, const char *lpString) { return 1; }
uint32_t CCALL SendDlgItemMessageA_c(void *hDlg, int32_t nIDDlgItem, uint32_t Msg, uint32_t wParam, uint32_t lParam) { return 0; }

void winapi_user32_init(void)
{
    init_keymap();
}

EXTERN_C_END
