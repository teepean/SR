/*
 * Steel Panthers (SSI 1995) - native runtime for the statically recompiled STEEL.EXE v1.1
 *
 * The recompiled code works with 32-bit pointers, so everything it can see (its own data, the
 * guest stack, DPMI/DOS memory, buffers handed to it) lives below 4 GB: the program is linked
 * non-PIE at 0x10000000 and dynamic guest memory comes from mmap(MAP_32BIT).
 */
#ifndef SP_H
#define SP_H

#include <stdint.h>
#include <stddef.h>
#include "x64/x64_stack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* guest (32-bit) <-> host pointers */
#define G2H(a) ((void *)(uintptr_t)(uint32_t)(a))
static inline uint32_t H2G(const void *p) { return (uint32_t)(uintptr_t)p; }

/* the recompiled data object starts at guest address 0xD0000 (label loc_D0000) */
extern uint8_t guest_data_start[];
#define GUEST_DATA_BASE 0xD0000u
#define GDATA(addr) ((void *)(guest_data_start + ((uint32_t)(addr) - GUEST_DATA_BASE)))
#define GVAR8(addr)  (*(uint8_t  *)GDATA(addr))
#define GVAR16(addr) (*(uint16_t *)GDATA(addr))
#define GVAR32(addr) (*(uint32_t *)GDATA(addr))

/* Variables of STEEL.EXE v1.2 used by the runtime (v1.1 addresses in NOTES.md; tools/addrmap.py
   maps them between builds). */
#define G_CLIB_STACK_TOP    0xe3828u    /* Watcom cstart variables (cstart itself is not run) */
#define G_CLIB_STACK_TOP2   0xe3810u
#define G_CLIB_CURBRK       0xe3814u
#define G_CLIB_STACKLOW     0xe3824u
#define G_CLIB_PSP          0xe3820u
#define G_CLIB_OSMAJOR      0xe384bu
#define G_CLIB_OSMINOR      0xe384cu
#define G_CLIB_EXTENDER     0xe3842u
#define G_CLIB_EXTENDER2    0xe3843u
#define G_CLIB_NO87         0xe3840u
#define G_CLIB_ENVP         0xe3845u
#define G_CLIB_ENVSEL       0xe3849u
#define G_CLIB_CMDLINE      0xe3818u
#define G_CLIB_PGMNAME      0xe381cu
#define G_BSS_END           0x16db9cu
#define G_ARGC              0x16d6fcu
#define G_ARGV              0x16d700u
#define G_MOUSE_X           0x16cea8u   /* screen position the map loops test for edge scrolling */
#define G_MOUSE_Y           0x16ceacu
#define G_SCROLL_STEP       0x16c69cu   /* int16, hexes per scroll step */

/* register frame left on the guest stack by x86_int (PUSHFD, PUSHAD) */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eflags;
} Regs;

#define FLAG_CF 0x0001u
#define FLAG_ZF 0x0040u

#define AX(r) (*(uint16_t *)&(r)->eax)
#define BX(r) (*(uint16_t *)&(r)->ebx)
#define CX(r) (*(uint16_t *)&(r)->ecx)
#define DX(r) (*(uint16_t *)&(r)->edx)
#define SI(r) (*(uint16_t *)&(r)->esi)
#define DI(r) (*(uint16_t *)&(r)->edi)
#define AL(r) (*(uint8_t *)&(r)->eax)
#define AH(r) (((uint8_t *)&(r)->eax)[1])
#define BL(r) (*(uint8_t *)&(r)->ebx)
#define BH(r) (((uint8_t *)&(r)->ebx)[1])
#define CL(r) (*(uint8_t *)&(r)->ecx)
#define CH(r) (((uint8_t *)&(r)->ecx)[1])
#define DL(r) (*(uint8_t *)&(r)->edx)
#define DH(r) (((uint8_t *)&(r)->edx)[1])

static inline void set_cf(Regs *r, int on) { if (on) r->eflags |= FLAG_CF; else r->eflags &= ~FLAG_CF; }
static inline void set_zf(Regs *r, int on) { if (on) r->eflags |= FLAG_ZF; else r->eflags &= ~FLAG_ZF; }

/* main.c */
extern _stack *g_stack;
extern int sp_debug;
extern volatile uint32_t Game_TimerTick;
extern uint32_t Game_TimerRun;
extern uint32_t X86_InterruptFlag;
void sp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sp_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void sp_exit(int code) __attribute__((noreturn));
void sp_idle_poll(void);
uint32_t c_call_guest_regs(_stack *stack, uint32_t func, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx);
uint32_t c_call_guest_cdecl(_stack *stack, uint32_t func, uint32_t nargs, const uint32_t *args);

/* lowmem.c */
void lowmem_init(void);
void *lowmem_alloc(uint32_t size);
void lowmem_free(void *p);
void *lowmem_resize(void *p, uint32_t size);
uint32_t lowmem_largest_free(void);
void *dosmem_alloc(uint32_t paragraphs, uint16_t *segment);
int dosmem_free(uint16_t segment);
char *lowmem_strdup(const char *s);

/* dosfs.c */
void dosfs_init(const char *game_dir, const char *cd_image);
void dos_int21(Regs *r);
int dosfs_cd_present(void);
int dosfs_cd_read_audio(uint32_t lba, uint8_t *buf, uint32_t frames);
uint32_t dosfs_cd_tracks(uint32_t *starts, uint32_t max, uint32_t *leadout);

/* dosint.c */
void dosint_init(void);

/* video.c */
#define FB_W 640
#define FB_H 480
extern uint8_t *g_fb;           /* 8-bit framebuffer in low memory (also the VESA linear frame buffer) */
extern uint8_t g_palette[256][3]; /* 6-bit VGA DAC values */
extern uint32_t g_fb_width, g_fb_height;
int video_init(void);
void video_shutdown(void);
void video_set_mode(uint32_t w, uint32_t h);
void video_mark_dirty(void);
void video_present(int force);
void video_poll(void);          /* pump SDL events; called often from the main thread */
void video_wait_frame(void);    /* wait for the next 60 Hz frame (vertical retrace) */
uint32_t video_retrace_status(void);
void video_vga_out(uint32_t port, uint8_t value);
uint8_t video_vga_in(uint32_t port);

/* input (video.c) */
int kbd_peek(uint16_t *key);
uint16_t kbd_get(void);         /* blocks */
uint8_t kbd_shift_flags(void);
uint8_t kbd_last_scancode(void);
void mouse_get(int *x, int *y, int *buttons);
void mouse_set_pos(int x, int y);
void mouse_set_range_x(int min, int max);
void mouse_set_range_y(int min, int max);
void mouse_motion(int *dx, int *dy);

/* ail.c */
void ail_init(void);
void ail_run_timers(void);
void cdaudio_request(uint8_t *req);

#ifdef __cplusplus
}
#endif

#endif
