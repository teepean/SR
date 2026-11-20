/*
 * Interrupts and I/O ports seen by the recompiled program: DPMI (INT 31h), video BIOS/VESA
 * (INT 10h), keyboard (INT 16h), mouse (INT 33h), multiplex/MSCDEX (INT 2Fh), Watcom's
 * int386x() and the VGA ports.
 */
#define _GNU_SOURCE
#include "sp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LFB_PHYS 0xE0000000u

static uint32_t next_selector = 0x100;
static uint16_t vesa_mode = 3;
static uint32_t vesa_bank;
static uint32_t pm_vectors[256];

static void dispatch(uint32_t intno, Regs *r, int real_mode);

static uint64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ INT 10h */

static uint8_t *rm_ptr(uint16_t seg, uint16_t off) { return (uint8_t *)G2H(((uint32_t)seg << 4) + off); }

static int vesa_mode_size(uint16_t mode, uint32_t *w, uint32_t *h)
{
    switch (mode & 0x1ff) {
    case 0x100: *w = 640; *h = 400; return 1;
    case 0x101: *w = 640; *h = 480; return 1;
    case 0x103: *w = 800; *h = 600; return 1;
    case 0x105: *w = 1024; *h = 768; return 1;
    case 0x013: *w = 320; *h = 200; return 1;
    }
    return 0;
}

static void vesa_info(uint8_t *b)
{
    static const uint16_t modes[] = { 0x100, 0x101, 0x103, 0x105, 0xffff };
    memset(b, 0, 256);
    memcpy(b, "VESA", 4);
    b[4] = 0x00; b[5] = 0x02;                /* version 2.0 */
    /* OEM string and mode list are placed inside the 512-byte buffer (real-mode far pointers) */
    uint32_t lin = H2G(b);
    strcpy((char *)b + 0x100, "Steel Panthers SR");
    uint32_t oem = lin + 0x100, list = lin + 0x120;
    uint16_t v;
    v = (uint16_t)(oem & 15); memcpy(b + 6, &v, 2); v = (uint16_t)(oem >> 4); memcpy(b + 8, &v, 2);
    b[10] = 1;                               /* DAC switchable */
    v = (uint16_t)(list & 15); memcpy(b + 14, &v, 2); v = (uint16_t)(list >> 4); memcpy(b + 16, &v, 2);
    memcpy(b + 0x120, modes, sizeof modes);
    v = 16; memcpy(b + 18, &v, 2);           /* 16 * 64 KB of video memory */
}

static int vesa_mode_info(uint16_t mode, uint8_t *b)
{
    uint32_t w, h;
    if (!vesa_mode_size(mode, &w, &h)) return 0;
    memset(b, 0, 256);
    uint16_t v;
    v = 0x9b; memcpy(b, &v, 2);              /* supported, colour, graphics, LFB */
    b[2] = 7; b[3] = 0;                      /* window A readable/writable */
    v = 64; memcpy(b + 4, &v, 2);            /* granularity KB */
    v = 64; memcpy(b + 6, &v, 2);            /* window size KB */
    v = 0xa000; memcpy(b + 8, &v, 2);
    v = (uint16_t)w; memcpy(b + 16, &v, 2);  /* bytes per scan line */
    v = (uint16_t)w; memcpy(b + 18, &v, 2);
    v = (uint16_t)h; memcpy(b + 20, &v, 2);
    b[22] = 8; b[23] = 16; b[24] = 1; b[25] = 8; b[26] = (uint8_t)((w * h + 65535) / 65536); b[27] = 4; b[29] = 0; b[30] = 1;
    uint32_t phys = LFB_PHYS; memcpy(b + 40, &phys, 4);
    return 1;
}

static void int10(Regs *r, int real_mode, uint16_t es)
{
    uint16_t ax = AX(r);
    if (AH(r) == 0x00) {
        uint8_t m = AL(r) & 0x7f;
        vesa_mode = m;
        if (m == 0x13) video_set_mode(320, 200);
        else if (m == 0x03) video_set_mode(640, 480);
        if (sp_debug) sp_log("INT 10h set mode %02x\n", m);
        return;
    }
    switch (ax) {
    case 0x1a00: AL(r) = 0x1a; BL(r) = 0x08; BH(r) = 0; return;
    case 0x4f00: {
        uint8_t *b = real_mode ? rm_ptr(es, DI(r)) : (uint8_t *)G2H(r->edi);
        vesa_info(b);
        AX(r) = 0x004f;
        return; }
    case 0x4f01: {
        uint8_t *b = real_mode ? rm_ptr(es, DI(r)) : (uint8_t *)G2H(r->edi);
        AX(r) = vesa_mode_info(CX(r), b) ? 0x004f : 0x014f;
        return; }
    case 0x4f02: {
        uint32_t w, h;
        if (!vesa_mode_size(BX(r), &w, &h)) { AX(r) = 0x014f; return; }
        vesa_mode = BX(r) & 0x1ff;
        vesa_bank = 0;
        video_set_mode(w, h);
        if (!(BX(r) & 0x8000)) memset(g_fb, 0, w * h);
        if (sp_debug) sp_log("VESA set mode %04x (%ux%u)\n", BX(r), w, h);
        AX(r) = 0x004f;
        return; }
    case 0x4f03: BX(r) = vesa_mode; AX(r) = 0x004f; return;
    case 0x4f05:
        if (BH(r) == 0) {
            extern void video_set_bank(uint32_t bank);
            vesa_bank = DX(r);
            video_set_bank(vesa_bank);
        } else DX(r) = (uint16_t)vesa_bank;
        AX(r) = 0x004f;
        return;
    case 0x4f06: case 0x4f07: AX(r) = 0x004f; return;
    case 0x0f00: AL(r) = (uint8_t)vesa_mode; AH(r) = 80; BH(r) = 0; return;
    case 0x1010: g_palette[BX(r) & 255][0] = DH(r) & 63; g_palette[BX(r) & 255][1] = CH(r) & 63; g_palette[BX(r) & 255][2] = CL(r) & 63; video_mark_dirty(); return;
    case 0x1012: {
        const uint8_t *s = real_mode ? rm_ptr(es, DX(r)) : (const uint8_t *)G2H(r->edx);
        for (uint32_t i = 0; i < CX(r) && BX(r) + i < 256; i++) for (int c = 0; c < 3; c++) g_palette[BX(r) + i][c] = s[i * 3 + c] & 63;
        video_mark_dirty();
        return; }
    case 0x1017: {
        uint8_t *d = real_mode ? rm_ptr(es, DX(r)) : (uint8_t *)G2H(r->edx);
        for (uint32_t i = 0; i < CX(r) && BX(r) + i < 256; i++) for (int c = 0; c < 3; c++) d[i * 3 + c] = g_palette[BX(r) + i][c];
        return; }
    }
    if (sp_debug) sp_log("INT 10h AX=%04x not handled\n", ax);
}

/* ------------------------------------------------------------------ INT 16h */

static void int16(Regs *r)
{
    uint16_t k;
    switch (AH(r)) {
    case 0x00: case 0x10: AX(r) = kbd_get(); return;
    case 0x01: case 0x11:
        if (kbd_peek(&k)) { AX(r) = k; set_zf(r, 0); } else { set_zf(r, 1); sp_idle_poll(); }
        return;
    case 0x02: case 0x12: AL(r) = kbd_shift_flags(); return;
    }
}

/* ------------------------------------------------------------------ INT 33h */

static uint32_t mouse_handler, mouse_mask;

/* The game installs a real-mode INT 33h event handler that stores BX/CX/DX at 0:4F0/4F2/4F4;
   those accesses are redirected here and kept current while a handler is installed. */
uint16_t sp_bios_ica[3];
int sp_mouse_handler_active;
static int mouse_press_x[3], mouse_press_y[3], mouse_press_n[3];

static void int33(Regs *r)
{
    int x, y, b;
    switch (AX(r)) {
    case 0x0000: case 0x0021:
        AX(r) = 0xffff; BX(r) = 2;
        mouse_set_range_x(0, 639); mouse_set_range_y(0, 479);
        return;
    case 0x0001: case 0x0002: return;
    case 0x0003:
        mouse_get(&x, &y, &b);
        BX(r) = (uint16_t)b; CX(r) = (uint16_t)x; DX(r) = (uint16_t)y;
        sp_idle_poll();
        return;
    case 0x0004: mouse_set_pos((int16_t)CX(r), (int16_t)DX(r)); return;
    case 0x0005: case 0x0006: {
        int n = BX(r) < 3 ? BX(r) : 0;
        mouse_get(&x, &y, &b);
        AX(r) = (uint16_t)b; BX(r) = (uint16_t)mouse_press_n[n]; CX(r) = (uint16_t)mouse_press_x[n]; DX(r) = (uint16_t)mouse_press_y[n];
        mouse_press_n[n] = 0;
        return; }
    case 0x0007: mouse_set_range_x((int16_t)CX(r), (int16_t)DX(r)); return;
    case 0x0008: mouse_set_range_y((int16_t)CX(r), (int16_t)DX(r)); return;
    case 0x000b: { int dx, dy; mouse_motion(&dx, &dy); CX(r) = (uint16_t)dx; DX(r) = (uint16_t)dy; return; }
    case 0x000c:
        mouse_mask = CX(r); mouse_handler = r->edx;
        sp_mouse_handler_active = mouse_mask != 0;
        if (sp_debug) sp_log("mouse event handler %s (mask %x)\n", sp_mouse_handler_active ? "installed" : "removed", mouse_mask);
        return;
    case 0x000f: case 0x001a: case 0x001d: return;
    case 0x0024: BX(r) = 0x0626; CH(r) = 4; CL(r) = 0; return;
    }
    if (sp_debug) sp_log("INT 33h AX=%04x not handled\n", AX(r));
}

/* ------------------------------------------------------------------ INT 2Fh */

static void int2f(Regs *r, int real_mode, uint16_t es)
{
    switch (AX(r)) {
    case 0x1600: AL(r) = 0; return;                       /* no Windows */
    case 0x1687: AX(r) = 1; return;                       /* no DPMI host entry */
    case 0x1500:
        if (dosfs_cd_present()) { BX(r) = 1; CX(r) = 3; } else BX(r) = 0;
        return;
    case 0x150b: AX(r) = (CX(r) == 3 && dosfs_cd_present()) ? 0x5ad8 : 0; BX(r) = 0xadad; return;
    case 0x150c: BX(r) = 0x0215; return;
    case 0x150d: { uint8_t *b = real_mode ? rm_ptr(es, BX(r)) : (uint8_t *)G2H(r->ebx); b[0] = 3; return; }
    case 0x1510: {
        uint8_t *req = real_mode ? rm_ptr(es, BX(r)) : (uint8_t *)G2H(r->ebx);
        cdaudio_request(req);
        set_cf(r, 0);
        return; }
    }
    if (sp_debug) sp_log("INT 2Fh AX=%04x not handled\n", AX(r));
}

/* ------------------------------------------------------------------ INT 31h (DPMI) */

static void int31(Regs *r)
{
    set_cf(r, 0);
    switch (AX(r)) {
    case 0x0000: AX(r) = (uint16_t)next_selector; next_selector += 8 * (CX(r) ? CX(r) : 1); return;
    case 0x0001: case 0x0007: case 0x0008: case 0x0009: case 0x000c: return;
    case 0x0002: AX(r) = (uint16_t)next_selector; next_selector += 8; return;
    case 0x0003: AX(r) = 8; return;
    case 0x0006: CX(r) = 0; DX(r) = 0; return;
    case 0x000a: AX(r) = (uint16_t)next_selector; next_selector += 8; return;
    case 0x000b: memset(G2H(r->edi), 0, 8); return;
    case 0x0100: {
        uint16_t seg;
        if (!dosmem_alloc(BX(r), &seg)) { AX(r) = 8; BX(r) = 0; set_cf(r, 1); return; }
        AX(r) = seg; DX(r) = seg;
        if (sp_debug) sp_log("DPMI DOS alloc %u paragraphs -> %04x\n", BX(r), seg);
        return; }
    case 0x0101: if (!dosmem_free(DX(r))) { AX(r) = 9; set_cf(r, 1); } return;
    case 0x0102: AX(r) = 8; set_cf(r, 1); return;
    case 0x0200: CX(r) = 0; DX(r) = 0; return;
    case 0x0201: return;
    case 0x0202: case 0x0203: return;
    case 0x0204: CX(r) = 0; r->edx = pm_vectors[BL(r)]; return;
    case 0x0205: pm_vectors[BL(r)] = r->edx; if (sp_debug) sp_log("DPMI set PM vector %02x -> %08x\n", BL(r), r->edx); return;
    case 0x0300: case 0x0301: case 0x0302: {
        uint8_t *rm = (uint8_t *)G2H(r->edi);
        Regs rr;
        memcpy(&rr.edi, rm + 0x00, 4); memcpy(&rr.esi, rm + 0x04, 4); memcpy(&rr.ebp, rm + 0x08, 4);
        memcpy(&rr.ebx, rm + 0x10, 4); memcpy(&rr.edx, rm + 0x14, 4); memcpy(&rr.ecx, rm + 0x18, 4); memcpy(&rr.eax, rm + 0x1c, 4);
        uint16_t fl; memcpy(&fl, rm + 0x20, 2); rr.eflags = fl;
        uint16_t es; memcpy(&es, rm + 0x22, 2);
        if (AX(r) == 0x0300) {
            uint8_t n = BL(r);
            if (n == 0x10) int10(&rr, 1, es);
            else if (n == 0x2f) int2f(&rr, 1, es);
            else if (n == 0x33) int33(&rr);
            else if (n == 0x21) dos_int21(&rr);
            else if (sp_debug) sp_log("DPMI simulate real-mode INT %02xh AX=%04x\n", n, AX(&rr));
        } else if (sp_debug) sp_log("DPMI call real-mode procedure %04x\n", AX(r));
        memcpy(rm + 0x00, &rr.edi, 4); memcpy(rm + 0x04, &rr.esi, 4); memcpy(rm + 0x08, &rr.ebp, 4);
        memcpy(rm + 0x10, &rr.ebx, 4); memcpy(rm + 0x14, &rr.edx, 4); memcpy(rm + 0x18, &rr.ecx, 4); memcpy(rm + 0x1c, &rr.eax, 4);
        fl = (uint16_t)rr.eflags; memcpy(rm + 0x20, &fl, 2);
        return; }
    case 0x0303: CX(r) = 0xf000; DX(r) = 0x0100; return;
    case 0x0304: return;
    case 0x0400: AX(r) = 0x005a; BX(r) = 5; CL(r) = 5; DX(r) = 0x0870; return;
    case 0x0500: {
        uint32_t *b = (uint32_t *)G2H(r->edi);
        uint32_t big = lowmem_largest_free();
        for (int i = 0; i < 12; i++) b[i] = 0xffffffffu;
        b[0] = big; b[1] = big / 4096; b[2] = big / 4096; b[3] = big / 4096; b[4] = big / 4096; b[5] = big / 4096; b[6] = big / 4096; b[7] = big / 4096;
        return; }
    case 0x0501: {
        uint32_t size = ((uint32_t)BX(r) << 16) | CX(r);
        void *p = lowmem_alloc(size);
        if (!p) { AX(r) = 0x8013; set_cf(r, 1); return; }
        uint32_t a = H2G(p);
        BX(r) = (uint16_t)(a >> 16); CX(r) = (uint16_t)a; SI(r) = (uint16_t)(a >> 16); DI(r) = (uint16_t)a;
        if (sp_debug) sp_log("DPMI alloc %u bytes -> %08x\n", size, a);
        return; }
    case 0x0502: lowmem_free(G2H(((uint32_t)SI(r) << 16) | DI(r))); return;
    case 0x0503: {
        uint32_t size = ((uint32_t)BX(r) << 16) | CX(r);
        void *p = lowmem_resize(G2H(((uint32_t)SI(r) << 16) | DI(r)), size);
        if (!p) { AX(r) = 0x8013; set_cf(r, 1); return; }
        uint32_t a = H2G(p);
        BX(r) = (uint16_t)(a >> 16); CX(r) = (uint16_t)a; SI(r) = (uint16_t)(a >> 16); DI(r) = (uint16_t)a;
        return; }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603: case 0x0702: case 0x0703: return;
    case 0x0604: BX(r) = 0; CX(r) = 0x1000; return;
    case 0x0800: {
        uint32_t phys = ((uint32_t)BX(r) << 16) | CX(r);
        uint32_t lin = phys;
        if (phys >= LFB_PHYS && phys < LFB_PHYS + 0x400000) lin = H2G(g_fb) + (phys - LFB_PHYS);
        BX(r) = (uint16_t)(lin >> 16); CX(r) = (uint16_t)lin;
        if (sp_debug) sp_log("DPMI map physical %08x -> %08x\n", phys, lin);
        return; }
    case 0x0801: return;
    case 0x0a00: AX(r) = 0x8001; set_cf(r, 1); return;
    }
    sp_log("INT 31h AX=%04x not implemented\n", AX(r));
    AX(r) = 0x8001;
    set_cf(r, 1);
}

/* ------------------------------------------------------------------ dispatch */

static void dispatch(uint32_t intno, Regs *r, int real_mode)
{
    (void)real_mode;
    switch (intno) {
    case 0x10: int10(r, 0, 0); break;
    case 0x16: int16(r); break;
    case 0x1a:
        if (AH(r) == 0) {
            uint32_t ticks = (uint32_t)((double)ms_now() * 18.2065 / 1000.0);
            CX(r) = (uint16_t)(ticks >> 16); DX(r) = (uint16_t)ticks; AL(r) = 0;
        }
        break;
    case 0x21: dos_int21(r); break;
    case 0x2f: int2f(r, 0, 0); break;
    case 0x31: int31(r); break;
    case 0x33: int33(r); break;
    default:
        sp_log("INT %02xh AX=%04x not implemented\n", intno, AX(r));
        break;
    }
}

/* SP_STATS: histogram of interrupt calls and port reads, printed every 5 seconds */
static uint32_t stat_int[256][256], stat_port[0x400];
static uint64_t stat_last;

static void stats_tick(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("SP_STATS") != NULL;
    if (!enabled) return;
    uint64_t t = ms_now();
    if (!stat_last) stat_last = t;
    if (t - stat_last < 5000) return;
    stat_last = t;
    for (int i = 0; i < 256; i++) for (int a = 0; a < 256; a++)
        if (stat_int[i][a] > 100) sp_log("stats: INT %02xh AH=%02x x%u\n", i, a, stat_int[i][a]);
    for (int p = 0; p < 0x400; p++) if (stat_port[p] > 100) sp_log("stats: in port %x x%u\n", p, stat_port[p]);
    memset(stat_int, 0, sizeof stat_int);
    memset(stat_port, 0, sizeof stat_port);
}

void X86_InterruptProcedure(uint32_t intno, uint32_t esp)
{
    Regs *r = (Regs *)G2H(esp);
    stat_int[intno & 255][AH(r)]++;
    stats_tick();
    dispatch(intno, r, 0);
}

/* Watcom int386x(int, union REGS *in, union REGS *out, struct SREGS *seg) */
uint32_t Game_int386x(uint32_t intno, uint32_t in, uint32_t out, uint32_t sregs)
{
    const uint32_t *ri = (const uint32_t *)G2H(in);
    uint32_t *ro = (uint32_t *)G2H(out);
    Regs r;
    memset(&r, 0, sizeof r);
    r.eax = ri[0]; r.ebx = ri[1]; r.ecx = ri[2]; r.edx = ri[3]; r.esi = ri[4]; r.edi = ri[5];
    r.eflags = 0x202;
    (void)sregs;
    stat_int[intno & 255][(r.eax >> 8) & 255]++;
    dispatch(intno, &r, 0);
    ro[0] = r.eax; ro[1] = r.ebx; ro[2] = r.ecx; ro[3] = r.edx; ro[4] = r.esi; ro[5] = r.edi;
    ro[6] = r.eflags & FLAG_CF;
    return r.eax;
}

/* ------------------------------------------------------------------ ports */

/* PIT channel 0: counts down from 65536 at 1.193182 MHz; latch via port 43h, read low/high byte */
static uint16_t pit_latch;
static int pit_latched, pit_hi_next;

static uint16_t pit_count(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ticks = ((uint64_t)ts.tv_sec * 1193182ull) + ((uint64_t)ts.tv_nsec * 1193182ull) / 1000000000ull;
    return (uint16_t)(65536 - (ticks % 65536));
}

static uint8_t pit_read(void)
{
    sp_idle_poll();
    if (!pit_latched) { pit_latch = pit_count(); pit_latched = 1; pit_hi_next = 0; }
    uint8_t v = pit_hi_next ? (uint8_t)(pit_latch >> 8) : (uint8_t)pit_latch;
    if (pit_hi_next) pit_latched = 0;
    pit_hi_next ^= 1;
    return v;
}

uint32_t X86_InPortProcedure(uint32_t port, uint32_t length)
{
    (void)length;
    stat_port[port & 0x3ff]++;
    stats_tick();
    switch (port) {
    case 0x3da: case 0x3ba:
        sp_idle_poll();
        return video_vga_in(port);
    case 0x3c7: case 0x3c8: case 0x3c9:
        return video_vga_in(port);
    case 0x40: return pit_read();
    case 0x60: return kbd_last_scancode();
    case 0x64: return 0x14;
    case 0x201: return 0xff;
    }
    if (sp_debug) sp_log("in port %x\n", port);
    return 0;
}

void X86_OutPortProcedure(uint32_t port, uint32_t length, uint32_t value)
{
    (void)length;
    switch (port) {
    case 0x3c7: case 0x3c8: case 0x3c9:
        video_vga_out(port, (uint8_t)value);
        return;
    case 0x43:
        if ((value & 0xc0) == 0 && (value & 0x30) == 0) { pit_latch = pit_count(); pit_latched = 1; pit_hi_next = 0; }
        else if ((value & 0xc0) == 0) { pit_latched = 0; pit_hi_next = 0; }
        return;
    case 0x20: case 0x21: case 0xa0: case 0xa1: case 0x40: case 0x41: case 0x42: case 0x61:
        return;
    }
    if (sp_debug) sp_log("out port %x, %x\n", port, value);
}

/* segment-prefixed memory accesses (only in dead startup code); flat model */
uint32_t X86_ReadMemProcedure(uint32_t address, uint32_t size)
{
    sp_log("unexpected segment memory read at %08x\n", address);
    switch (size) { case 1: return *(uint8_t *)G2H(address); case 2: return *(uint16_t *)G2H(address); default: return *(uint32_t *)G2H(address); }
}

void X86_WriteMemProcedure(uint32_t address, uint32_t size, uint32_t value)
{
    sp_log("unexpected segment memory write at %08x\n", address);
    switch (size) { case 1: *(uint8_t *)G2H(address) = (uint8_t)value; break; case 2: *(uint16_t *)G2H(address) = (uint16_t)value; break; default: *(uint32_t *)G2H(address) = value; }
}

uint32_t X86_ReadFsDword(uint32_t address) { return *(uint32_t *)G2H(address); }
void X86_WriteFsDword(uint32_t address, uint32_t value) { *(uint32_t *)G2H(address) = value; }

void dosint_init(void) {}
