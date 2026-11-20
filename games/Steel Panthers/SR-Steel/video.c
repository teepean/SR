/*
 * Display, keyboard and mouse (SDL3), VGA/VESA emulation and the Miles VFX device driver.
 *
 * The game draws into its own 640x480 window buffer and calls the VFX driver to copy
 * rectangles to the screen; the film player writes to the VESA linear frame buffer. Both end
 * up in g_fb, which is converted through the VGA palette and presented at up to 60 Hz.
 */
#define _GNU_SOURCE
#include "sp.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

uint8_t *g_fb;
uint8_t g_palette[256][3];
uint32_t g_fb_width = FB_W, g_fb_height = FB_H;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t tex_w, tex_h;
static uint32_t *rgb_buf;
static int dirty = 1;
static uint64_t last_present_ns, last_poll_ns;

static uint8_t *bank_window;        /* 64 KB at 0xA0000 for banked VESA access and mode 13h */
static uint32_t cur_bank;
static int banks_used, mode13;

static uint64_t now_ns(void) { return SDL_GetTicksNS(); }

/* ------------------------------------------------------------------ framebuffer */

void video_mark_dirty(void) { dirty = 1; }

static void flush_bank(void)
{
    if (!bank_window || !banks_used) return;
    uint32_t off = cur_bank * 0x10000u;
    if (off < 1024u * 1024u) memcpy(g_fb + off, bank_window, 0x10000);
}

void video_set_bank(uint32_t bank)
{
    if (!bank_window) return;
    flush_bank();
    banks_used = 1;
    cur_bank = bank;
    uint32_t off = cur_bank * 0x10000u;
    if (off < 1024u * 1024u) memcpy(bank_window, g_fb + off, 0x10000);
}

void video_set_mode(uint32_t w, uint32_t h)
{
    mode13 = (w == 320 && h == 200);
    g_fb_width = w;
    g_fb_height = h;
    dirty = 1;
}

int video_init(void)
{
    g_fb = (uint8_t *)lowmem_alloc(1024 * 1024);
    rgb_buf = (uint32_t *)SDL_malloc(1024 * 768 * 4);
    void *p = mmap((void *)0xA0000, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    bank_window = (p == MAP_FAILED || p != (void *)0xA0000) ? NULL : (uint8_t *)p;
    if (!bank_window) sp_log("video: no VGA window at 0xA0000 (banked modes unavailable)\n");

    for (int i = 0; i < 256; i++) g_palette[i][0] = g_palette[i][1] = g_palette[i][2] = (uint8_t)(i >> 2);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { sp_log("SDL_Init: %s\n", SDL_GetError()); return 0; }
    window = SDL_CreateWindow("Steel Panthers", FB_W * 2, FB_H * 2, SDL_WINDOW_RESIZABLE);
    if (!window) { sp_log("SDL_CreateWindow: %s\n", SDL_GetError()); return 0; }
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) { sp_log("SDL_CreateRenderer: %s\n", SDL_GetError()); return 0; }
    SDL_SetRenderVSync(renderer, 0);
    SDL_HideCursor();
    video_set_mode(FB_W, FB_H);
    return 1;
}

void video_shutdown(void)
{
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    texture = NULL; renderer = NULL; window = NULL;
}

void video_present(int force)
{
    uint64_t t = now_ns();
    if (!force && t - last_present_ns < 16000000ull) return;
    if (!renderer) return;
    last_present_ns = t;

    uint32_t w = g_fb_width, h = g_fb_height;
    if (!texture || tex_w != w || tex_h != h) {
        if (texture) SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
#if SDL_VERSION_ATLEAST(3, 4, 0)
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_PIXELART);
#else
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
#endif
        SDL_SetRenderLogicalPresentation(renderer, (int)(mode13 ? 320 : w), (int)(mode13 ? 240 : h), SDL_LOGICAL_PRESENTATION_LETTERBOX);
        tex_w = w; tex_h = h;
    }

    flush_bank();
    const uint8_t *src = mode13 && bank_window ? bank_window : g_fb;
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) {
        uint32_t r = g_palette[i][0], g = g_palette[i][1], b = g_palette[i][2];
        r = (r << 2) | (r >> 4); g = (g << 2) | (g >> 4); b = (b << 2) | (b >> 4);
        pal[i] = 0xff000000u | (r << 16) | (g << 8) | b;
    }
    for (uint32_t i = 0; i < w * h; i++) rgb_buf[i] = pal[src[i]];
    SDL_UpdateTexture(texture, NULL, rgb_buf, (int)(w * 4));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    dirty = 0;
}

/* vertical retrace emulation: 60 Hz, retrace during the first ~1.2 ms of each frame */
static uint64_t frame_of(uint64_t t) { return t / 16666667ull; }

uint32_t video_retrace_status(void)
{
    static uint64_t last_frame;
    uint64_t t = now_ns();
    uint64_t f = frame_of(t);
    if (f != last_frame) { last_frame = f; video_poll(); }
    return (t % 16666667ull) < 1200000ull ? 0x09 : 0x00;
}

void video_wait_frame(void)
{
    uint64_t t = now_ns();
    uint64_t next = (frame_of(t) + 1) * 16666667ull;
    if (next > t) SDL_DelayNS(next - t);
    video_present(1);
    video_poll();
}

/* ------------------------------------------------------------------ VGA DAC ports */

static uint8_t dac_read_idx, dac_write_idx, dac_read_comp, dac_write_comp;

void video_vga_out(uint32_t port, uint8_t v)
{
    switch (port) {
    case 0x3c7: dac_read_idx = v; dac_read_comp = 0; break;
    case 0x3c8: dac_write_idx = v; dac_write_comp = 0; break;
    case 0x3c9:
        g_palette[dac_write_idx][dac_write_comp] = v & 63;
        if (++dac_write_comp == 3) { dac_write_comp = 0; dac_write_idx++; }
        dirty = 1;
        break;
    }
}

uint8_t video_vga_in(uint32_t port)
{
    switch (port) {
    case 0x3c7: return 0;
    case 0x3c8: return dac_write_idx;
    case 0x3c9: {
        uint8_t v = g_palette[dac_read_idx][dac_read_comp];
        if (++dac_read_comp == 3) { dac_read_comp = 0; dac_read_idx++; }
        return v; }
    case 0x3da: case 0x3ba: return (uint8_t)video_retrace_status();
    }
    return 0;
}

/* ------------------------------------------------------------------ keyboard */

#define KQ 64
static uint16_t kq[KQ];
static int kq_head, kq_tail;
static uint8_t last_scan;

static void kq_push(uint16_t k)
{
    int n = (kq_tail + 1) % KQ;
    if (n == kq_head) return;
    kq[kq_tail] = k;
    kq_tail = n;
}

static uint8_t xt_scancode(SDL_Scancode s)
{
    static const uint8_t letters[26] = { 0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c };
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return letters[s - SDL_SCANCODE_A];
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return (uint8_t)(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return (uint8_t)(0x3b + (s - SDL_SCANCODE_F1));
    switch (s) {
    case SDL_SCANCODE_0: return 0x0b;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x1c;
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_BACKSPACE: return 0x0e;
    case SDL_SCANCODE_TAB: return 0x0f;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_MINUS: return 0x0c;
    case SDL_SCANCODE_EQUALS: return 0x0d;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1a;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
    case SDL_SCANCODE_BACKSLASH: case SDL_SCANCODE_NONUSBACKSLASH: return 0x2b;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: case SDL_SCANCODE_KP_DIVIDE: return 0x35;
    case SDL_SCANCODE_CAPSLOCK: return 0x3a;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_HOME: case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_UP: case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4a;
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_KP_4: return 0x4b;
    case SDL_SCANCODE_KP_5: return 0x4c;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_KP_6: return 0x4d;
    case SDL_SCANCODE_KP_PLUS: return 0x4e;
    case SDL_SCANCODE_END: case SDL_SCANCODE_KP_1: return 0x4f;
    case SDL_SCANCODE_DOWN: case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_PAGEDOWN: case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_INSERT: case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_DELETE: case SDL_SCANCODE_KP_PERIOD: return 0x53;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LSHIFT: return 0x2a;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x1d;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x38;
    default: return 0;
    }
}

/* Arrow keys (and the keypad arrows with NumLock off) scroll the battle map, see
   Game_map_edge_scroll. Under DOS they reached the game as 0 + scan code, which it only reads as
   the letters H/K/M/P, so they are not passed on. */
static int is_scroll_key(SDL_Scancode s, SDL_Keymod mod)
{
    switch (s) {
    case SDL_SCANCODE_UP: case SDL_SCANCODE_DOWN: case SDL_SCANCODE_LEFT: case SDL_SCANCODE_RIGHT: return 1;
    case SDL_SCANCODE_KP_8: case SDL_SCANCODE_KP_2: case SDL_SCANCODE_KP_4: case SDL_SCANCODE_KP_6: return !(mod & SDL_KMOD_NUM);
    default: return 0;
    }
}

static void key_down(const SDL_KeyboardEvent *e)
{
    uint8_t sc = xt_scancode(e->scancode);
    if (!sc) return;
    last_scan = sc;
    SDL_Keymod mod = e->mod;
    if (is_scroll_key(e->scancode, mod)) return;
    int shift = (mod & SDL_KMOD_SHIFT) != 0, ctrl = (mod & SDL_KMOD_CTRL) != 0, alt = (mod & SDL_KMOD_ALT) != 0;
    if (sc == 0x2a || sc == 0x36 || sc == 0x1d || sc == 0x38 || sc == 0x3a || sc == 0x45 || sc == 0x46) return;

    /* function keys */
    if (sc >= 0x3b && sc <= 0x44) {
        uint8_t n = sc - 0x3b;
        kq_push((uint16_t)((alt ? 0x68 + n : ctrl ? 0x5e + n : shift ? 0x54 + n : sc) << 8));
        return;
    }
    if (sc == 0x57 || sc == 0x58) {
        uint8_t n = sc == 0x57 ? 0 : 1;
        kq_push((uint16_t)((alt ? 0x8b + n : ctrl ? 0x89 + n : shift ? 0x87 + n : 0x85 + n) << 8));
        return;
    }
    /* cursor / editing keys (not on the keypad with NumLock on) */
    int keypad_digit = e->scancode >= SDL_SCANCODE_KP_1 && e->scancode <= SDL_SCANCODE_KP_PERIOD && (mod & SDL_KMOD_NUM);
    if (sc >= 0x47 && sc <= 0x53 && sc != 0x4a && sc != 0x4e && !keypad_digit) {
        uint8_t code = sc;
        if (ctrl) {
            switch (sc) { case 0x4b: code = 0x73; break; case 0x4d: code = 0x74; break; case 0x4f: code = 0x75; break;
                          case 0x51: code = 0x76; break; case 0x47: code = 0x77; break; case 0x49: code = 0x84; break; }
        }
        kq_push((uint16_t)(code << 8));
        return;
    }

    SDL_Keycode k = SDL_GetKeyFromScancode(e->scancode, mod & (SDL_KMOD_SHIFT | SDL_KMOD_CAPS | SDL_KMOD_NUM), false);
    uint8_t ascii = 0;
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) ascii = 13;
    else if (k == SDLK_ESCAPE) ascii = 27;
    else if (k == SDLK_BACKSPACE) ascii = 8;
    else if (k == SDLK_TAB) ascii = 9;
    else if (k >= 32 && k < 127) ascii = (uint8_t)k;
    else if (keypad_digit) ascii = e->scancode == SDL_SCANCODE_KP_PERIOD ? '.' : e->scancode == SDL_SCANCODE_KP_0 ? '0' : (uint8_t)('1' + (e->scancode - SDL_SCANCODE_KP_1));
    else if (e->scancode == SDL_SCANCODE_KP_MINUS) ascii = '-';
    else if (e->scancode == SDL_SCANCODE_KP_PLUS) ascii = '+';
    else if (e->scancode == SDL_SCANCODE_KP_MULTIPLY) ascii = '*';
    else if (e->scancode == SDL_SCANCODE_KP_DIVIDE) ascii = '/';

    if (alt) { kq_push((uint16_t)(sc << 8)); return; }
    if (ctrl && ascii >= 'a' && ascii <= 'z') ascii = (uint8_t)(ascii - 'a' + 1);
    else if (ctrl && ascii >= 'A' && ascii <= 'Z') ascii = (uint8_t)(ascii - 'A' + 1);
    kq_push((uint16_t)((sc << 8) | ascii));
}

int kbd_peek(uint16_t *key)
{
    video_poll();
    if (kq_head == kq_tail) return 0;
    *key = kq[kq_head];
    return 1;
}

uint16_t kbd_get(void)
{
    uint16_t k;
    while (!kbd_peek(&k)) {
        ail_run_timers();
        video_present(0);
        SDL_DelayNS(1000000);
    }
    kq_head = (kq_head + 1) % KQ;
    return k;
}

uint8_t kbd_shift_flags(void)
{
    SDL_Keymod m = SDL_GetModState();
    uint8_t f = 0;
    if (m & SDL_KMOD_RSHIFT) f |= 1;
    if (m & SDL_KMOD_LSHIFT) f |= 2;
    if (m & SDL_KMOD_CTRL) f |= 4;
    if (m & SDL_KMOD_ALT) f |= 8;
    if (m & SDL_KMOD_NUM) f |= 0x20;
    if (m & SDL_KMOD_CAPS) f |= 0x40;
    return f;
}

uint8_t kbd_last_scancode(void) { return last_scan; }

/* ------------------------------------------------------------------ mouse */

static float mouse_fx = FB_W / 2, mouse_fy = FB_H / 2;   /* in screen pixels */
static int mouse_buttons;
static int range_x0 = 0, range_x1 = 639, range_y0 = 0, range_y1 = 479;
static float motion_dx, motion_dy;

/* screen pixel 0..size-1 -> the range the program set with INT 33h 0007h/0008h, so that both the
   first and the last pixel reach the ends of the range (the battle map scrolls at x 639 / y 479) */
static int mouse_to_range(float f, uint32_t size, int lo, int hi)
{
    if (size < 2 || hi <= lo) return lo;
    int v = lo + (int)(f * (float)(hi - lo) / (float)(size - 1) + 0.5f);
    return v < lo ? lo : v > hi ? hi : v;
}

void mouse_get(int *x, int *y, int *b)
{
    video_poll();
    *x = mouse_to_range(mouse_fx, g_fb_width, range_x0, range_x1);
    *y = mouse_to_range(mouse_fy, g_fb_height, range_y0, range_y1);
    *b = mouse_buttons;
}

void mouse_set_pos(int x, int y)
{
    if (x < range_x0) x = range_x0;
    if (x > range_x1) x = range_x1;
    if (y < range_y0) y = range_y0;
    if (y > range_y1) y = range_y1;
    mouse_fx = range_x1 > range_x0 ? (float)(x - range_x0) * (float)(g_fb_width - 1) / (float)(range_x1 - range_x0) : 0;
    mouse_fy = range_y1 > range_y0 ? (float)(y - range_y0) * (float)(g_fb_height - 1) / (float)(range_y1 - range_y0) : 0;
    if (renderer && window && SDL_GetMouseFocus() == window) {
        float wx, wy;
        if (SDL_RenderCoordinatesToWindow(renderer, mouse_fx, mouse_fy, &wx, &wy)) SDL_WarpMouseInWindow(window, wx, wy);
    }
}

void mouse_set_range_x(int a, int b) { if (a > b) { int t = a; a = b; b = t; } range_x0 = a; range_x1 = b; }
void mouse_set_range_y(int a, int b) { if (a > b) { int t = a; a = b; b = t; } range_y0 = a; range_y1 = b; }

void mouse_motion(int *dx, int *dy)
{
    video_poll();
    *dx = (int)motion_dx; *dy = (int)motion_dy;
    motion_dx -= (float)*dx; motion_dy -= (float)*dy;
}

/* Mouse capture, as DOSBox's autolock: a click in the window confines the pointer to it, so the
   map can be scrolled by pushing against the screen edge. Ctrl+Alt releases it (DOSBox's Ctrl+F10
   is KDE's "Present Windows" shortcut and never reaches the program there, but works elsewhere),
   and so does leaving the window (Alt+Tab, a desktop shortcut). */
static int mouse_captured;

static void set_mouse_capture(int on)
{
    if (!window || on == mouse_captured) return;
    mouse_captured = on;
    SDL_SetWindowMouseGrab(window, on);
    SDL_SetWindowTitle(window, on ? "Steel Panthers - Ctrl+Alt releases the mouse" : "Steel Panthers");
}

static int is_release_key(const SDL_KeyboardEvent *k)
{
    SDL_Keymod m = SDL_GetModState() | k->mod;
    switch (k->scancode) {
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return (m & SDL_KMOD_ALT) != 0;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return (m & SDL_KMOD_CTRL) != 0;
    case SDL_SCANCODE_F10: return (m & SDL_KMOD_CTRL) != 0;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ map scrolling
 * Replaces the game's check at 0x2B65C (v1.1: 0x2A204) (called from the battle map and the deployment and editor
 * map loops): the original scrolls one step while the mouse is at an edge of the 640x480 screen,
 * through 0x2B6F6(dx, dy) (v1.1: 0x2A29E), which clamps the view and returns 1 when it moved. The arrow keys now
 * do the same. The map loops poll this about 100000 times a second, and the original speed came
 * only from how long a 1995 PC took to redraw the map, so one step (2 hexes) is taken at most
 * every SCROLL_INTERVAL_NS: the first at once, then a screen width in about a second. */

#define SCROLL_INTERVAL_NS 100000000ull

extern uint8_t map_scroll_by[];
static SDL_Scancode auto_hold_key;
static uint64_t auto_hold_until;

static int scroll_held(SDL_Scancode arrow, SDL_Scancode keypad)
{
    const bool *ks = SDL_GetKeyboardState(NULL);
    if (ks[arrow] || (ks[keypad] && !(SDL_GetModState() & SDL_KMOD_NUM))) return 1;
    return auto_hold_key == arrow && now_ns() < auto_hold_until;
}

uint32_t Game_map_edge_scroll(void)
{
    video_poll();
    int32_t mx = (int32_t)GVAR32(G_MOUSE_X), my = (int32_t)GVAR32(G_MOUSE_Y);
    int32_t step = (int16_t)GVAR16(G_SCROLL_STEP);
    uint32_t fn = H2G(map_scroll_by), r = 0;
    int left = mx == 0 || scroll_held(SDL_SCANCODE_LEFT, SDL_SCANCODE_KP_4);
    int right = mx == 639 || scroll_held(SDL_SCANCODE_RIGHT, SDL_SCANCODE_KP_6);
    int up = my == 0 || scroll_held(SDL_SCANCODE_UP, SDL_SCANCODE_KP_8);
    int down = my == 479 || scroll_held(SDL_SCANCODE_DOWN, SDL_SCANCODE_KP_2);
    static uint64_t next_ns;
    uint64_t t = now_ns();
    if (!(left | right | up | down)) { next_ns = 0; return 0; }
    if (t < next_ns) return 0;
    next_ns = t + SCROLL_INTERVAL_NS;
    if (left) r |= c_call_guest_regs(g_stack, fn, (uint32_t)-step, 0, 0, 0);
    if (right) r |= c_call_guest_regs(g_stack, fn, (uint32_t)step, 0, 0, 0);
    if (up) r |= c_call_guest_regs(g_stack, fn, 0, (uint32_t)-step, 0, 0);
    if (down) r |= c_call_guest_regs(g_stack, fn, 0, (uint32_t)step, 0, 0);
    return r & 0xff;
}

/* ------------------------------------------------------------------ events */

extern uint16_t sp_bios_ica[3];
extern int sp_mouse_handler_active;

static void update_mouse_ica(void)
{
    if (!sp_mouse_handler_active) return;
    int mx = mouse_to_range(mouse_fx, g_fb_width, range_x0, range_x1);
    int my = mouse_to_range(mouse_fy, g_fb_height, range_y0, range_y1);
    sp_bios_ica[0] = (uint16_t)mouse_buttons;
    sp_bios_ica[1] = (uint16_t)mx;
    sp_bios_ica[2] = (uint16_t)my;
}

/* ------------------------------------------------------------------ test automation
 * SP_AUTOMATE=file runs commands from the event loop, one per line:
 *   sleep MS | key NAME (SDL key name) | click X Y [right] | move X Y | shot NAME | quit
 * SP_SHOT_DIR (default: current directory) receives shot NAME.bmp files. */

static FILE *auto_file;
static uint64_t auto_wait_until;
static int auto_release_buttons;
static uint64_t auto_release_at;

static void save_shot(const char *name)
{
    const char *dir = getenv("SP_SHOT_DIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.bmp", dir ? dir : ".", name);
    flush_bank();
    uint32_t w = g_fb_width, h = g_fb_height;
    const uint8_t *src = mode13 && bank_window ? bank_window : g_fb;
    SDL_Surface *surf = SDL_CreateSurface((int)w, (int)h, SDL_PIXELFORMAT_XRGB8888);
    if (!surf) return;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)surf->pixels + y * surf->pitch);
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *c = g_palette[src[y * w + x]];
            uint32_t r = (c[0] << 2) | (c[0] >> 4), g = (c[1] << 2) | (c[1] >> 4), b = (c[2] << 2) | (c[2] >> 4);
            row[x] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
    SDL_SaveBMP(surf, path);
    SDL_DestroySurface(surf);
    sp_log("automation: saved %s\n", path);
}

static void automation_step(void)
{
    static int inited;
    if (!inited) {
        inited = 1;
        const char *f = getenv("SP_AUTOMATE");
        if (f && *f) auto_file = fopen(f, "rt");
        auto_wait_until = now_ns();
    }
    uint64_t t = now_ns();
    if (auto_release_buttons && t >= auto_release_at) { mouse_buttons = 0; auto_release_buttons = 0; }
    if (!auto_file || t < auto_wait_until) return;
    char line[256];
    while (t >= auto_wait_until && fgets(line, sizeof line, auto_file)) {
        char cmd[32] = "", arg[128] = "";
        int a = 0, b = 0;
        if (sscanf(line, "%31s", cmd) != 1 || cmd[0] == '#') continue;
        if (!strcmp(cmd, "sleep") && sscanf(line, "%*s %d", &a) == 1) { auto_wait_until = t + (uint64_t)a * 1000000ull; return; }
        if (!strcmp(cmd, "key") && sscanf(line, "%*s %127s", arg) == 1) {
            SDL_KeyboardEvent ke;
            memset(&ke, 0, sizeof ke);
            ke.scancode = SDL_GetScancodeFromName(arg);
            ke.mod = SDL_KMOD_NONE;
            key_down(&ke);
        } else if (!strcmp(cmd, "hold") && sscanf(line, "%*s %127s %d", arg, &a) == 2) {
            auto_hold_key = SDL_GetScancodeFromName(arg);
            auto_hold_until = t + (uint64_t)a * 1000000ull;
        } else if ((!strcmp(cmd, "click") || !strcmp(cmd, "move")) && sscanf(line, "%*s %d %d %127s", &a, &b, arg) >= 2) {
            mouse_fx = (float)a * (float)g_fb_width / FB_W;
            mouse_fy = (float)b * (float)g_fb_height / FB_H;
            if (!strcmp(cmd, "click")) {
                mouse_buttons = !strcmp(arg, "right") ? 2 : 1;
                auto_release_buttons = 1;
                auto_release_at = t + 120000000ull;
                auto_wait_until = t + 200000000ull;
                return;
            }
        } else if (!strcmp(cmd, "shot") && sscanf(line, "%*s %127s", arg) == 1) {
            save_shot(arg);
        } else if (!strcmp(cmd, "quit")) {
            sp_exit(0);
        }
    }
}

void video_poll(void)
{
    uint64_t t = now_ns();
    if (t - last_poll_ns < 1000000ull) return;
    last_poll_ns = t;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            sp_exit(0);
        case SDL_EVENT_KEY_DOWN:
            if (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            if (is_release_key(&e.key)) {
                set_mouse_capture(0);
                break;
            }
            key_down(&e.key);
            break;
        case SDL_EVENT_KEY_UP: {
            uint8_t sc = xt_scancode(e.key.scancode);
            if (sc) last_scan = sc | 0x80;
            break; }
        case SDL_EVENT_MOUSE_MOTION:
            SDL_ConvertEventToRenderCoordinates(renderer, &e);
            motion_dx += e.motion.xrel; motion_dy += e.motion.yrel;
            mouse_fx = e.motion.x; mouse_fy = e.motion.y;
            if (mouse_fx < 0) mouse_fx = 0;
            if (mouse_fy < 0) mouse_fy = 0;
            if (mouse_fx > (float)g_fb_width - 1) mouse_fx = (float)g_fb_width - 1;
            if (mouse_fy > (float)g_fb_height - 1) mouse_fy = (float)g_fb_height - 1;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) set_mouse_capture(1);
            int bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : e.button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) mouse_buttons |= bit; else mouse_buttons &= ~bit;
            break; }
        case SDL_EVENT_WINDOW_EXPOSED:
            dirty = 1;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            set_mouse_capture(0);
            break;
        }
    }
    automation_step();
    update_mouse_ica();
    video_present(0);
}

/* ------------------------------------------------------------------ Miles VFX device driver */

typedef struct { uint32_t buffer; int32_t x_max, y_max; uint32_t stencil, shadow; } VfxWindow;
typedef struct { uint32_t window; int32_t x0, y0, x1, y1; } VfxPane;

static uint32_t *vfx_table;
static int32_t *vfx_desc;

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* copy (scaled with nearest neighbour when sizes differ) src rect -> dst rect, both inclusive and valid */
static void blit(const uint8_t *src, int32_t spitch, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                 uint8_t *dst, int32_t dpitch, int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1)
{
    int32_t sw = sx1 - sx0 + 1, sh = sy1 - sy0 + 1, dw = dx1 - dx0 + 1, dh = dy1 - dy0 + 1;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (sw == dw && sh == dh) {
        for (int32_t y = 0; y < dh; y++) memcpy(dst + (size_t)(dy0 + y) * dpitch + dx0, src + (size_t)(sy0 + y) * spitch + sx0, (size_t)dw);
        return;
    }
    for (int32_t y = 0; y < dh; y++) {
        const uint8_t *s = src + (size_t)(sy0 + (int32_t)((int64_t)y * sh / dh)) * spitch + sx0;
        uint8_t *d = dst + (size_t)(dy0 + y) * dpitch + dx0;
        for (int32_t x = 0; x < dw; x++) d[x] = s[(int64_t)x * sw / dw];
    }
}

static int clip_screen(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
    int32_t mx = (int32_t)g_fb_width - 1, my = (int32_t)g_fb_height - 1;
    if (*x0 > mx || *y0 > my || *x1 < 0 || *y1 < 0) return 0;
    *x0 = clampi(*x0, 0, mx); *x1 = clampi(*x1, 0, mx);
    *y0 = clampi(*y0, 0, my); *y1 = clampi(*y1, 0, my);
    return *x0 <= *x1 && *y0 <= *y1;
}

uint32_t Game_VFX_describe_driver(void) { return H2G(vfx_desc); }

uint32_t Game_VFX_init_driver(void)
{
    video_set_mode(640, 480);
    memset(g_fb, 0, 640 * 480);
    dirty = 1;
    return 0;
}

uint32_t Game_VFX_shutdown_driver(void) { return 0; }

uint32_t Game_VFX_area_wipe(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color)
{
    int32_t a = (int32_t)x0, b = (int32_t)y0, c = (int32_t)x1, d = (int32_t)y1;
    if (!clip_screen(&a, &b, &c, &d)) return 0;
    for (int32_t y = b; y <= d; y++) memset(g_fb + (size_t)y * g_fb_width + a, (int)(color & 255), (size_t)(c - a + 1));
    dirty = 1;
    return 0;
}

uint32_t Game_VFX_wait_vblank(void) { video_wait_frame(); return 0; }
uint32_t Game_VFX_wait_vblank_leading(void) { video_wait_frame(); return 0; }

uint32_t Game_VFX_window_refresh(uint32_t win, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    VfxWindow *w = (VfxWindow *)G2H(win);
    int32_t a = (int32_t)x0, b = (int32_t)y0, c = (int32_t)x1, d = (int32_t)y1;
    if (!clip_screen(&a, &b, &c, &d)) return 0;
    blit((const uint8_t *)G2H(w->buffer), w->x_max + 1, 0, 0, w->x_max, w->y_max, g_fb, (int32_t)g_fb_width, a, b, c, d);
    dirty = 1;
    return 0;
}

uint32_t Game_VFX_window_read(uint32_t win, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    VfxWindow *w = (VfxWindow *)G2H(win);
    int32_t a = (int32_t)x0, b = (int32_t)y0, c = (int32_t)x1, d = (int32_t)y1;
    if (!clip_screen(&a, &b, &c, &d)) return 0;
    flush_bank();
    blit(g_fb, (int32_t)g_fb_width, a, b, c, d, (uint8_t *)G2H(w->buffer), w->x_max + 1, 0, 0, w->x_max, w->y_max);
    return 0;
}

uint32_t Game_VFX_pane_refresh(uint32_t pane, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    VfxPane *p = (VfxPane *)G2H(pane);
    VfxWindow *w = (VfxWindow *)G2H(p->window);
    if (p->x0 > w->x_max || p->y0 > w->y_max || p->x1 < 0 || p->y1 < 0) return 0;
    int32_t sx0 = clampi(p->x0, 0, w->x_max), sx1 = clampi(p->x1, 0, w->x_max);
    int32_t sy0 = clampi(p->y0, 0, w->y_max), sy1 = clampi(p->y1, 0, w->y_max);
    int32_t a = (int32_t)x0, b = (int32_t)y0, c = (int32_t)x1, d = (int32_t)y1;
    if (!clip_screen(&a, &b, &c, &d)) return 0;
    blit((const uint8_t *)G2H(w->buffer), w->x_max + 1, sx0, sy0, sx1, sy1, g_fb, (int32_t)g_fb_width, a, b, c, d);
    dirty = 1;
    return 0;
}

uint32_t Game_VFX_DAC_read(uint32_t index, uint32_t rgb)
{
    uint8_t *c = (uint8_t *)G2H(rgb);
    memcpy(c, g_palette[index & 255], 3);
    return 0;
}

uint32_t Game_VFX_DAC_write(uint32_t index, uint32_t rgb)
{
    const uint8_t *c = (const uint8_t *)G2H(rgb);
    for (int i = 0; i < 3; i++) g_palette[index & 255][i] = c[i] & 63;
    dirty = 1;
    return 0;
}

uint32_t Game_VFX_slot10(uint32_t a) { sp_log("VFX driver function 10 (%08x) called\n", a); return 0; }
uint32_t Game_VFX_slot12(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { sp_log("VFX driver function 12 (%08x %08x %08x %08x) called\n", a, b, c, d); return 0; }

/* stubs in sp-asm.asm */
extern uint8_t SR_VFX_describe_driver[], SR_VFX_init_driver[], SR_VFX_shutdown_driver[], SR_VFX_area_wipe[],
    SR_VFX_wait_vblank[], SR_VFX_wait_vblank_leading[], SR_VFX_window_refresh[], SR_VFX_window_read[],
    SR_VFX_DAC_read[], SR_VFX_DAC_write[], SR_VFX_slot10[], SR_VFX_pane_refresh[], SR_VFX_slot12[];

/* replaces the game's DLL loader (0xA6C8F): any VFX driver name gets the native driver */
uint32_t Game_load_driver_dll(uint32_t name, uint32_t a, uint32_t b)
{
    sp_log("load driver \"%s\" (%u, %u)\n", (const char *)G2H(name), a, b);
    if (!vfx_table) {
        vfx_table = (uint32_t *)lowmem_alloc(4096);
        vfx_desc = (int32_t *)(vfx_table + 32);
        const uint8_t *fn[13] = { SR_VFX_describe_driver, SR_VFX_init_driver, SR_VFX_shutdown_driver, SR_VFX_area_wipe,
            SR_VFX_wait_vblank, SR_VFX_wait_vblank_leading, SR_VFX_window_refresh, SR_VFX_window_read,
            SR_VFX_DAC_read, SR_VFX_DAC_write, SR_VFX_slot10, SR_VFX_pane_refresh, SR_VFX_slot12 };
        for (int i = 0; i < 13; i++) vfx_table[i] = H2G(fn[i]);
        static const int32_t desc[8] = { 640, 480, 1, 256, 1, 1, 0, 0 };
        memcpy(vfx_desc, desc, sizeof desc);
    }
    return H2G(vfx_table);
}
