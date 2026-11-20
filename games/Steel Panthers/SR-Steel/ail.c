/*
 * Miles AIL 3.03 replacement (digital samples, double-buffered streaming, timers) and the
 * MSCDEX Red Book audio player, on SDL3 audio. One audio stream callback mixes everything.
 *
 * The game calls the AIL API with the cdecl convention; handles given to it are pointers to
 * zeroed blocks in low memory, the real state lives here.
 */
#define _GNU_SOURCE
#include "sp.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define OUT_RATE 44100

enum { SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8 };

typedef struct {
    int used;
    uint8_t *guest;
    int status;
    uint32_t addr[2], len[2];
    int loaded[2];              /* buffer holds unplayed data */
    int cur;
    int streaming;
    double pos;                 /* in source frames within the current buffer */
    int bits16, stereo, is_signed;
    int rate, volume, pan, loops;
    uint32_t user[8];
} Sample;

#define MAX_SAMPLES 32
static Sample samples[MAX_SAMPLES];

typedef struct { int used, running; uint32_t callback, user; uint64_t period_ns, next_ns; } Timer;
#define MAX_TIMERS 16
static Timer timers[MAX_TIMERS];

static SDL_AudioStream *stream;
static uint8_t *dig_driver, *mdi_driver;
static int32_t preferences[64];
static int audio_disabled;

/* CD audio state (guarded by the stream lock) */
static int cd_playing, cd_paused;
static uint32_t cd_start_lba, cd_cur_lba, cd_end_lba;
static uint32_t cd_frame_off;   /* byte offset within the current 2352-byte frame */
static uint8_t cd_frame[2352];
static int cd_frame_valid;
static int cd_volume = 255;

static void lock(void) { if (stream) SDL_LockAudioStream(stream); }
static void unlock(void) { if (stream) SDL_UnlockAudioStream(stream); }

static Sample *get_sample(uint32_t h)
{
    for (int i = 0; i < MAX_SAMPLES; i++) if (samples[i].used && H2G(samples[i].guest) == h) return &samples[i];
    return NULL;
}

/* ------------------------------------------------------------------ mixer */

static int sample_frame(const Sample *s, const uint8_t *buf, uint32_t frame, int *l, int *r)
{
    if (s->bits16) {
        const int16_t *p = (const int16_t *)buf + (size_t)frame * (s->stereo ? 2 : 1);
        *l = p[0]; *r = s->stereo ? p[1] : p[0];
    } else {
        const uint8_t *p = buf + (size_t)frame * (s->stereo ? 2 : 1);
        int a = s->is_signed ? (int8_t)p[0] : (int)p[0] - 128;
        int b = s->stereo ? (s->is_signed ? (int8_t)p[1] : (int)p[1] - 128) : a;
        *l = a << 8; *r = b << 8;
    }
    return 1;
}

static void mix_sample(Sample *s, int32_t *acc, int frames)
{
    double step = (double)s->rate / OUT_RATE;
    int vl = s->volume * (127 - s->pan) / 64, vr = s->volume * s->pan / 64;
    if (vl > 127) vl = 127;
    if (vr > 127) vr = 127;
    for (int i = 0; i < frames && s->status == SMP_PLAYING; i++) {
        uint32_t bpf = (uint32_t)((s->bits16 ? 2 : 1) * (s->stereo ? 2 : 1));
        uint32_t nframes = s->len[s->cur] / bpf;
        uint32_t f = (uint32_t)s->pos;
        if (f >= nframes) {
            if (s->streaming) {
                s->loaded[s->cur] = 0;
                int other = s->cur ^ 1;
                if (s->loaded[other]) { s->cur = other; s->pos = 0; i--; continue; }
                s->status = SMP_DONE;
                break;
            }
            if (s->loops != 1) {
                if (s->loops > 1) s->loops--;
                s->pos = 0;
                i--;
                continue;
            }
            s->status = SMP_DONE;
            break;
        }
        int l, r;
        sample_frame(s, (const uint8_t *)G2H(s->addr[s->cur]), f, &l, &r);
        acc[i * 2] += l * vl / 127;
        acc[i * 2 + 1] += r * vr / 127;
        s->pos += step;
    }
}

static void mix_cd(int32_t *acc, int frames)
{
    for (int i = 0; i < frames; i++) {
        if (!cd_frame_valid || cd_frame_off >= 2352) {
            if (cd_cur_lba >= cd_end_lba) { cd_playing = 0; return; }
            dosfs_cd_read_audio(cd_cur_lba, cd_frame, 1);
            cd_cur_lba++;
            cd_frame_off = 0;
            cd_frame_valid = 1;
        }
        int16_t l, r;
        memcpy(&l, cd_frame + cd_frame_off, 2);
        memcpy(&r, cd_frame + cd_frame_off + 2, 2);
        cd_frame_off += 4;
        acc[i * 2] += l * cd_volume / 255;
        acc[i * 2 + 1] += r * cd_volume / 255;
    }
}

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *st, int additional, int total)
{
    (void)userdata; (void)total;
    static int32_t acc[4096 * 2];
    static int16_t out[4096 * 2];
    while (additional > 0) {
        int frames = additional / 4;
        if (frames > 4096) frames = 4096;
        if (frames <= 0) break;
        memset(acc, 0, (size_t)frames * 8);
        for (int k = 0; k < MAX_SAMPLES; k++)
            if (samples[k].used && samples[k].status == SMP_PLAYING) mix_sample(&samples[k], acc, frames);
        if (cd_playing && !cd_paused) mix_cd(acc, frames);
        for (int i = 0; i < frames * 2; i++) {
            int32_t v = acc[i];
            out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        SDL_PutAudioStreamData(st, out, frames * 4);
        additional -= frames * 4;
    }
}

void ail_init(void)
{
    if (stream || audio_disabled) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { sp_log("audio: %s\n", SDL_GetError()); audio_disabled = 1; return; }
    SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 2, OUT_RATE };
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
    if (!stream) { sp_log("audio: %s\n", SDL_GetError()); audio_disabled = 1; return; }
    SDL_ResumeAudioStreamDevice(stream);
}

/* ------------------------------------------------------------------ timers */

static uint64_t now_ns(void) { return SDL_GetTicksNS(); }

void ail_run_timers(void)
{
    static int in_timers;
    if (in_timers || !X86_InterruptFlag || !g_stack) return;
    in_timers = 1;
    uint64_t t = now_ns();
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer *tm = &timers[i];
        if (!tm->used || !tm->running || !tm->period_ns || t < tm->next_ns) continue;
        tm->next_ns += tm->period_ns;
        if (t > tm->next_ns + 4 * tm->period_ns) tm->next_ns = t + tm->period_ns;
        uint32_t arg = tm->user;
        c_call_guest_cdecl(g_stack, tm->callback, 1, &arg);
    }
    in_timers = 0;
}

uint32_t Game_AIL_register_timer(uint32_t callback)
{
    for (int i = 0; i < MAX_TIMERS; i++) if (!timers[i].used) {
        timers[i] = (Timer){ 1, 0, callback, 0, 0, 0 };
        if (sp_debug) sp_log("AIL_register_timer(%08x) -> %d\n", callback, i);
        return (uint32_t)i;
    }
    return (uint32_t)-1;
}

uint32_t Game_AIL_set_timer_frequency(uint32_t h, uint32_t hz)
{
    if (h < MAX_TIMERS && hz) timers[h].period_ns = 1000000000ull / hz;
    if (sp_debug) sp_log("AIL_set_timer_frequency(%d, %u)\n", (int)h, hz);
    return 0;
}

uint32_t Game_AIL_start_timer(uint32_t h)
{
    if (h < MAX_TIMERS && timers[h].used) { timers[h].running = 1; timers[h].next_ns = now_ns() + timers[h].period_ns; }
    return 0;
}

uint32_t Game_AIL_stop_timer(uint32_t h) { if (h < MAX_TIMERS) timers[h].running = 0; return 0; }
uint32_t Game_AIL_release_timer_handle(uint32_t h) { if (h < MAX_TIMERS) timers[h].used = timers[h].running = 0; return 0; }

uint32_t Game_AIL_interrupt_divisor(void)
{
    uint64_t best = 0;
    for (int i = 0; i < MAX_TIMERS; i++) if (timers[i].used && timers[i].period_ns && (!best || timers[i].period_ns < best)) best = timers[i].period_ns;
    if (!best) return 0;
    return (uint32_t)(best * 1193181ull / 1000000000ull);
}

/* ------------------------------------------------------------------ AIL API */

uint32_t Game_AIL_startup(void) { ail_init(); return 0; }

uint32_t Game_AIL_shutdown(void)
{
    lock();
    for (int i = 0; i < MAX_SAMPLES; i++) if (samples[i].used) samples[i].status = SMP_DONE;
    unlock();
    for (int i = 0; i < MAX_TIMERS; i++) timers[i].running = 0;
    return 0;
}

uint32_t Game_AIL_set_preference(uint32_t n, uint32_t v)
{
    if (n >= 64) return 0;
    uint32_t old = (uint32_t)preferences[n];
    preferences[n] = (int32_t)v;
    return old;
}

static uint32_t dig_install(void)
{
    ail_init();
    if (audio_disabled) return 0;
    if (!dig_driver) dig_driver = (uint8_t *)lowmem_alloc(4096);
    sp_log("AIL: digital audio driver installed\n");
    return H2G(dig_driver);
}

/* AIL 3.x (v1.2): int AIL_install_DIG_INI(HDIGDRIVER *dig) -> AIL_INIT_SUCCESS 0, AIL_NO_INI_FILE 1,
   AIL_INIT_FAILURE 2 */
uint32_t Game_AIL_install_DIG_INI(uint32_t pdig)
{
    uint32_t d = dig_install();
    if (pdig) *(uint32_t *)G2H(pdig) = d;
    return d ? 0 : 1;
}

uint32_t Game_AIL_install_DIG_driver_file(uint32_t name, uint32_t io)
{
    (void)io;
    sp_log("AIL_install_DIG_driver_file(%s)\n", name ? (const char *)G2H(name) : "");
    return dig_install();
}

/* int AIL_install_MDI_INI(HMDIDRIVER *mdi): no MIDI device (the original install has no MDI.INI
   either), music comes from the CD */
uint32_t Game_AIL_install_MDI_INI(uint32_t pmdi)
{
    if (pmdi) *(uint32_t *)G2H(pmdi) = 0;
    return 1;
}
uint32_t Game_AIL_uninstall_MDI_driver(uint32_t d) { (void)d; return 0; }
uint32_t Game_AIL_allocate_sequence_handle(uint32_t d) { (void)d; return 0; }
uint32_t Game_AIL_release_sequence_handle(uint32_t s) { (void)s; return 0; }
uint32_t Game_AIL_init_sequence(uint32_t s, uint32_t f, uint32_t n) { (void)s; (void)f; (void)n; return 0; }
uint32_t Game_AIL_start_sequence(uint32_t s) { (void)s; return 0; }
uint32_t Game_AIL_end_sequence(uint32_t s) { (void)s; return 0; }
uint32_t Game_AIL_active_sequence_count(uint32_t d) { (void)d; return 0; }

uint32_t Game_AIL_allocate_sample_handle(uint32_t dig)
{
    (void)dig;
    lock();
    for (int i = 0; i < MAX_SAMPLES; i++) if (!samples[i].used) {
        uint8_t *g = samples[i].guest;
        memset(&samples[i], 0, sizeof samples[i]);
        samples[i].guest = g ? g : (uint8_t *)lowmem_alloc(512);
        samples[i].used = 1;
        samples[i].status = SMP_DONE;
        samples[i].rate = 11025; samples[i].volume = 127; samples[i].pan = 64; samples[i].loops = 1;
        unlock();
        return H2G(samples[i].guest);
    }
    unlock();
    return 0;
}

uint32_t Game_AIL_release_sample_handle(uint32_t h)
{
    lock();
    Sample *s = get_sample(h);
    if (s) { s->used = 0; s->status = SMP_FREE; }
    unlock();
    return 0;
}

uint32_t Game_AIL_init_sample(uint32_t h)
{
    lock();
    Sample *s = get_sample(h);
    if (s) {
        s->status = SMP_DONE;
        s->addr[0] = s->addr[1] = s->len[0] = s->len[1] = 0;
        s->loaded[0] = s->loaded[1] = 0;
        s->cur = 0; s->pos = 0; s->streaming = 0;
        s->bits16 = s->stereo = s->is_signed = 0;
        s->rate = 11025; s->volume = 127; s->pan = 64; s->loops = 1;
    }
    unlock();
    return 0;
}

uint32_t Game_AIL_set_sample_address(uint32_t h, uint32_t start, uint32_t len)
{
    lock();
    Sample *s = get_sample(h);
    if (s) { s->addr[0] = start; s->len[0] = len; s->cur = 0; s->pos = 0; s->streaming = 0; s->loaded[0] = 1; s->loaded[1] = 0; }
    unlock();
    return 0;
}

uint32_t Game_AIL_set_sample_type(uint32_t h, uint32_t format, uint32_t flags)
{
    lock();
    Sample *s = get_sample(h);
    if (s) { s->bits16 = format & 1; s->stereo = (format >> 1) & 1; s->is_signed = flags & 1; }
    unlock();
    return 0;
}

uint32_t Game_AIL_start_sample(uint32_t h)
{
    lock();
    Sample *s = get_sample(h);
    if (s && s->len[s->cur]) { s->pos = 0; s->status = SMP_PLAYING; }
    if (sp_debug && s) sp_log("AIL_start_sample %08x: %u bytes, %d Hz, %s%s, vol %d pan %d loops %d\n", h, s->len[s->cur], s->rate,
                             s->bits16 ? "16-bit" : "8-bit", s->stereo ? " stereo" : "", s->volume, s->pan, s->loops);
    unlock();
    return 0;
}

uint32_t Game_AIL_end_sample(uint32_t h)
{
    lock();
    Sample *s = get_sample(h);
    if (s) { s->status = SMP_DONE; s->loaded[0] = s->loaded[1] = 0; }
    unlock();
    return 0;
}

uint32_t Game_AIL_set_sample_playback_rate(uint32_t h, uint32_t rate) { lock(); Sample *s = get_sample(h); if (s && rate) s->rate = (int)rate; unlock(); return 0; }
uint32_t Game_AIL_set_sample_volume(uint32_t h, uint32_t v) { lock(); Sample *s = get_sample(h); if (s) s->volume = (int)(v > 127 ? 127 : v); unlock(); return 0; }
uint32_t Game_AIL_set_sample_pan(uint32_t h, uint32_t p) { lock(); Sample *s = get_sample(h); if (s) s->pan = (int)(p > 127 ? 127 : p); unlock(); return 0; }
uint32_t Game_AIL_set_sample_loop_count(uint32_t h, uint32_t n) { lock(); Sample *s = get_sample(h); if (s) s->loops = (int)n; unlock(); return 0; }

uint32_t Game_AIL_sample_status(uint32_t h)
{
    lock();
    Sample *s = get_sample(h);
    uint32_t st = s ? (uint32_t)s->status : SMP_FREE;
    unlock();
    return st;
}

uint32_t Game_AIL_minimum_sample_buffer_size(uint32_t dig, uint32_t rate, uint32_t format)
{
    (void)dig;
    uint32_t bytes = (rate ? rate : 11025) / 8;
    if (format & 1) bytes *= 2;
    if (format & 2) bytes *= 2;
    bytes = (bytes + 1023) & ~1023u;
    if (bytes < 2048) bytes = 2048;
    return bytes;
}

uint32_t Game_AIL_sample_buffer_ready(uint32_t h)
{
    lock();
    Sample *s = get_sample(h);
    int32_t ready = -1;
    if (s) {
        if (!s->streaming) { s->streaming = 1; s->loaded[0] = s->loaded[1] = 0; s->cur = 0; s->pos = 0; }
        if (!s->loaded[s->cur]) ready = s->cur;
        else if (!s->loaded[s->cur ^ 1]) ready = s->cur ^ 1;
    }
    unlock();
    return (uint32_t)ready;
}

uint32_t Game_AIL_load_sample_buffer(uint32_t h, uint32_t n, uint32_t buf, uint32_t len)
{
    lock();
    Sample *s = get_sample(h);
    if (s && n < 2) {
        s->streaming = 1;
        s->addr[n] = buf; s->len[n] = len;
        s->loaded[n] = len != 0;
        if (s->status != SMP_PLAYING && len) {
            s->cur = (int)n; s->pos = 0; s->status = SMP_PLAYING;
        }
    }
    unlock();
    return 0;
}

uint32_t Game_AIL_set_sample_user_data(uint32_t h, uint32_t index, uint32_t value)
{
    Sample *s = get_sample(h);
    if (s && index < 8) s->user[index] = value;
    return 0;
}

/* ------------------------------------------------------------------ MSCDEX audio */

static uint32_t rb_to_lba(uint32_t rb) { return ((rb >> 16) & 0xff) * 4500 + ((rb >> 8) & 0xff) * 75 + (rb & 0xff) - 150; }
static uint32_t lba_to_rb(uint32_t lba) { lba += 150; return ((lba / 4500) << 16) | (((lba / 75) % 60) << 8) | (lba % 75); }

static uint8_t *far_ptr(const uint8_t *p) { uint16_t off, seg; memcpy(&off, p, 2); memcpy(&seg, p + 2, 2); return (uint8_t *)G2H(((uint32_t)seg << 4) + off); }

void cdaudio_request(uint8_t *req)
{
    uint8_t cmd = req[2];
    uint16_t status = 0x0100;
    uint32_t starts[100], leadout = 0;
    uint32_t ntracks = dosfs_cd_tracks(starts, 100, &leadout);
    ail_init();

    switch (cmd) {
    case 3: {   /* IOCTL input */
        uint8_t *cb = far_ptr(req + 0x0e);
        switch (cb[0]) {
        case 0x00: memset(cb + 1, 0, 4); break;
        case 0x01: { lock(); uint32_t v = cb[1] ? lba_to_rb(cd_cur_lba) : cd_cur_lba; unlock(); memcpy(cb + 2, &v, 4); break; }
        case 0x04: cb[1] = 0; cb[2] = (uint8_t)cd_volume; cb[3] = 1; cb[4] = (uint8_t)cd_volume; cb[5] = 2; cb[6] = 0; cb[7] = 3; cb[8] = 0; break;
        case 0x06: { uint32_t v = 0x0214; memcpy(cb + 1, &v, 4); break; }
        case 0x07: { uint16_t v = 2048; cb[1] = 0; memcpy(cb + 2, &v, 2); break; }
        case 0x08: memcpy(cb + 1, &leadout, 4); break;
        case 0x09: cb[1] = 1; break;
        case 0x0a: { cb[1] = 1; cb[2] = (uint8_t)ntracks; uint32_t v = lba_to_rb(leadout); memcpy(cb + 3, &v, 4); break; }
        case 0x0b: {
            uint32_t t = cb[1];
            if (t < 1 || t > ntracks) { status = 0x8000 | 0x0001; break; }
            uint32_t v = lba_to_rb(starts[t - 1]);
            memcpy(cb + 2, &v, 4);
            cb[6] = t == 1 ? 0x41 : 0x01;
            break; }
        case 0x0c: {
            lock(); uint32_t cur = cd_cur_lba; unlock();
            uint32_t t = 1;
            for (uint32_t i = 0; i < ntracks; i++) if (starts[i] <= cur) t = i + 1;
            uint32_t rel = cur - starts[t - 1];
            cb[1] = t == 1 ? 0x41 : 0x01; cb[2] = (uint8_t)(((t / 10) << 4) | (t % 10)); cb[3] = 1;
            cb[4] = (uint8_t)(rel / 4500); cb[5] = (uint8_t)((rel / 75) % 60); cb[6] = (uint8_t)(rel % 75); cb[7] = 0;
            uint32_t abs = cur + 150;
            cb[8] = (uint8_t)(abs / 4500); cb[9] = (uint8_t)((abs / 75) % 60); cb[10] = (uint8_t)(abs % 75);
            break; }
        case 0x0f: {
            lock();
            uint16_t st = cd_paused ? 1 : 0;
            uint32_t a = lba_to_rb(cd_start_lba), b = lba_to_rb(cd_end_lba);
            unlock();
            memcpy(cb + 1, &st, 2); memcpy(cb + 3, &a, 4); memcpy(cb + 7, &b, 4);
            break; }
        default:
            if (sp_debug) sp_log("MSCDEX IOCTL input %02x\n", cb[0]);
        }
        break; }
    case 12: {  /* IOCTL output */
        uint8_t *cb = far_ptr(req + 0x0e);
        if (cb[0] == 0x03) cd_volume = cb[2];
        break; }
    case 0x84: {    /* play audio */
        uint32_t start, count;
        memcpy(&start, req + 0x0e, 4);
        memcpy(&count, req + 0x12, 4);
        if (req[0x0d] == 1) start = rb_to_lba(start);
        lock();
        cd_start_lba = cd_cur_lba = start;
        cd_end_lba = start + count;
        cd_frame_valid = 0;
        cd_playing = count != 0; cd_paused = 0;
        unlock();
        sp_log("CD play %u + %u frames\n", start, count);
        break; }
    case 0x85:      /* stop audio */
        lock();
        if (cd_playing && !cd_paused) cd_paused = 1; else { cd_playing = 0; cd_paused = 0; }
        unlock();
        break;
    case 0x88:      /* resume audio */
        lock();
        if (cd_paused) cd_paused = 0;
        unlock();
        break;
    case 0x0d: case 0x0e: case 0x07: break;  /* open, close, flush */
    default:
        if (sp_debug) sp_log("MSCDEX request %u\n", cmd);
    }
    lock();
    if (cd_playing && !cd_paused) status |= 0x0200;
    unlock();
    memcpy(req + 3, &status, 2);
}

/* ------------------------------------------------------------------ SSI CD audio library (v1.2 entry points 0xB76EF-0xB8394)
 * The original talks to MSCDEX through DPMI with far pointers into DOS memory; these replace
 * its five entry points. Track numbers are CD track numbers (1 = data track). */

static int cdlib_ready;

uint32_t Game_cdlib_init(void)
{
    ail_init();
    cdlib_ready = dosfs_cd_present() && !audio_disabled;
    sp_log("CD audio %s\n", cdlib_ready ? "available" : "not available");
    return (uint32_t)cdlib_ready;
}

uint32_t Game_cdlib_shutdown(void)
{
    lock(); cd_playing = 0; cd_paused = 0; unlock();
    cdlib_ready = 0;
    return 0;
}

uint32_t Game_cdlib_is_playing(void)
{
    if (!cdlib_ready) return 0;
    lock(); uint32_t r = cd_playing && !cd_paused; unlock();
    return r;
}

uint32_t Game_cdlib_stop(void)
{
    if (!cdlib_ready) return 0;
    lock(); cd_playing = 0; cd_paused = 0; unlock();
    return 1;
}

uint32_t Game_cdlib_play_track(uint32_t track)
{
    uint32_t starts[100], leadout = 0;
    uint32_t n = dosfs_cd_tracks(starts, 100, &leadout);
    if (!cdlib_ready || track < 1 || track > n) return 0;
    uint32_t start = starts[track - 1], end = track < n ? starts[track] : leadout;
    lock();
    cd_start_lba = cd_cur_lba = start;
    cd_end_lba = end;
    cd_frame_valid = 0;
    cd_playing = 1; cd_paused = 0;
    unlock();
    sp_log("CD play track %u (%u frames)\n", track, end - start);
    return 1;
}
