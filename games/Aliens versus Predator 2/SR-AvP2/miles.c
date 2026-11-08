/**
 *
 *  Miles Sound System (mss32.dll) for the engine's sound manager (soundmgr.cpp, soundinstance.cpp,
 *  soundbuffer.cpp of the LithTech Talon engine), on the SDL mixer (mixer.c).
 *
 *  The engine drives Miles directly: a digital driver (AIL_waveOutOpen), "software" 2D samples whose volume and
 *  pan it computes itself from the listener (AvP2 doesn't name a 3D provider, so no 3D samples are used), streams
 *  opened from a file handle ("\\\\<handle>": a WAV file inside a REZ archive, positioned by the engine) and the
 *  decompressors that turn IMA ADPCM and MP3 (Miles' ASI codec) WAV data into PCM WAV file images.
 *
 *  Samples play 8/16-bit mono/stereo PCM (IMA ADPCM is decoded when it's set) at their playback rate, with volume
 *  and pan (0-127), loop blocks and counts (0 = forever), and the master volume. Handles are blocks of game memory
 *  (the engine keeps them in 32-bit fields).
 *
 *  A 3D provider ("Miles 2D Stereo Positional Audio") is listed for the engine's options, with 3D samples that are
 *  played like 2D ones with distance attenuation and pan from the listener.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include "platform.h"
#include "winapi.h"
#include "mixer.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

extern int winapi_debug;

uint32_t CCALL ReadFile_c(void *hFile, void *lpBuffer, uint32_t nNumberOfBytesToRead, uint32_t *lpNumberOfBytesRead, void *lpOverlapped);

// mss.h
#define DIG_MIXER_CHANNELS 1
#define SMP_FREE 0x0001
#define SMP_DONE 0x0002
#define SMP_PLAYING 0x0004
#define SMP_STOPPED 0x0008
#define DIG_F_MONO_8 0
#define DIG_F_MONO_16 1
#define DIG_F_STEREO_8 2
#define DIG_F_STEREO_16 3
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_IMA_ADPCM 0x11
#define WAVE_FORMAT_MPEGLAYER3 0x55

static int32_t preferences[64] = { 0, 64 };     // DIG_MIXER_CHANNELS: 64
static int mixer_ok = -1;
static int master_volume = 127;

/* ------------------------------------------------------------------ */
/* system                                                              */

uint32_t CCALL AIL_startup_c(void) { return 1; }
void CCALL AIL_shutdown_c(void) { }

// every engine call is bracketed by these: the audio thread waits meanwhile
void CCALL AIL_lock_c(void) { mixer_lock(); }
void CCALL AIL_unlock_c(void) { mixer_unlock(); }

int32_t CCALL AIL_set_preference_c(uint32_t number, int32_t value)
{
    int32_t old;
    if (number >= 64) return 0;
    old = preferences[number];
    preferences[number] = value;
    return old;
}

int32_t CCALL AIL_get_preference_c(uint32_t number)
{
    return (number < 64) ? preferences[number] : 0;
}

uint32_t CCALL AIL_ms_count_c(void) { return SDL_GetTicks(); }

void * CCALL AIL_mem_alloc_lock_c(uint32_t size) { return game_malloc((size != 0) ? size : 1); }
void CCALL AIL_mem_free_lock_c(void *ptr) { if (ptr != NULL) game_free(ptr); }

/* ------------------------------------------------------------------ */
/* sound data                                                          */

typedef struct
{
    int format;                 // WAVE_FORMAT_
    int channels, rate, bits, block_align;
    const uint8_t *data;
    uint32_t len;
} wav_info;

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// a RIFF WAVE file image: its format and data chunk
static int parse_wav(const uint8_t *p, uint32_t size, wav_info *w)
{
    uint32_t pos = 12;
    int have_fmt = 0;
    memset(w, 0, sizeof(*w));
    if ((size < 12) || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return 0;
    while (pos + 8 <= size)
    {
        uint32_t len = rd32(p + pos + 4);
        const uint8_t *c = p + pos + 8;
        if (len > size - pos - 8) len = size - pos - 8;
        if (!memcmp(p + pos, "fmt ", 4) && (len >= 16))
        {
            w->format = rd16(c);
            w->channels = rd16(c + 2);
            w->rate = (int)rd32(c + 4);
            w->block_align = rd16(c + 12);
            w->bits = rd16(c + 14);
            have_fmt = 1;
        }
        else if (!memcmp(p + pos, "data", 4))
        {
            w->data = c;
            w->len = len;
            if (have_fmt) return 1;
        }
        pos += 8 + ((len + 1) & ~1u);
    }
    return have_fmt && (w->data != NULL);
}

// Microsoft IMA ADPCM (WAVE_FORMAT_IMA_ADPCM) -> 16-bit PCM; returns the frames (malloc'ed *out)
static const int ima_index_table[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

static int ima_nibble(int *pred, int *index, int nib)
{
    int step = ima_step_table[*index], diff = step >> 3;
    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    if (nib & 8) diff = -diff;
    *pred += diff;
    if (*pred > 32767) *pred = 32767; else if (*pred < -32768) *pred = -32768;
    *index += ima_index_table[nib];
    if (*index < 0) *index = 0; else if (*index > 88) *index = 88;
    return *pred;
}

static uint32_t decode_ima(const uint8_t *in, uint32_t len, int channels, int block_align, int16_t **out)
{
    uint32_t nblocks, frames_per_block, frames = 0;
    int16_t *o;
    if ((channels < 1) || (channels > 2) || (block_align <= 4 * channels)) { *out = NULL; return 0; }
    frames_per_block = (uint32_t)((block_align - 4 * channels) * 8 / (4 * channels)) + 1;
    nblocks = (len + block_align - 1) / block_align;
    o = (int16_t *)malloc((size_t)nblocks * frames_per_block * channels * 2 + 4);
    *out = o;
    if (o == NULL) return 0;
    for (uint32_t b = 0; b < nblocks; b++)
    {
        const uint8_t *p = in + (size_t)b * block_align;
        uint32_t avail = len - b * block_align;
        if (avail > (uint32_t)block_align) avail = block_align;
        if (avail < (uint32_t)(4 * channels)) break;
        int pred[2], index[2];
        int16_t *f = o + (size_t)frames * channels;
        for (int c = 0; c < channels; c++)
        {
            pred[c] = (int16_t)rd16(p + 4 * c);
            index[c] = p[4 * c + 2];
            if (index[c] > 88) index[c] = 88;
            f[c] = (int16_t)pred[c];
        }
        uint32_t n = 1;     // frames in this block
        const uint8_t *d = p + 4 * channels, *e = p + avail;
        // groups of 4 bytes (8 samples) per channel, interleaved
        while (d + 4 * channels <= e)
        {
            for (int c = 0; c < channels; c++)
            {
                const uint8_t *g = d + 4 * c;
                for (int k = 0; k < 8; k++)
                {
                    int nib = (g[k >> 1] >> ((k & 1) * 4)) & 15;
                    f[(n + k) * channels + c] = (int16_t)ima_nibble(&pred[c], &index[c], nib);
                }
            }
            d += 4 * channels;
            n += 8;
        }
        frames += n;
    }
    return frames;
}

// MP3 data -> 16-bit PCM (malloc'ed *out); returns the frames
static uint32_t decode_mp3(const uint8_t *in, uint32_t len, int *channels, int *rate, int16_t **out)
{
    mp3dec_t dec;
    mp3dec_frame_info_t info;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t cap = 0, frames = 0;
    int16_t *o = NULL;
    uint32_t pos = 0;
    *channels = 0;
    *rate = 0;
    mp3dec_init(&dec);
    while (pos < len)
    {
        int n = mp3dec_decode_frame(&dec, in + pos, (int)(len - pos), pcm, &info);
        if (info.frame_bytes == 0) break;
        pos += info.frame_bytes;
        if (n <= 0) continue;
        if (*channels == 0) { *channels = info.channels; *rate = info.hz; }
        if (info.channels != *channels) continue;
        if (frames + n > cap)
        {
            cap = (frames + n) * 2 + 4096;
            int16_t *no = (int16_t *)realloc(o, cap * *channels * 2);
            if (no == NULL) break;
            o = no;
        }
        memcpy(o + frames * *channels, pcm, (size_t)n * *channels * 2);
        frames += n;
    }
    *out = o;
    return (uint32_t)frames;
}

// a PCM WAV file image of 16-bit samples, in game memory (AIL_mem_alloc_lock: the engine frees it)
static uint8_t *make_wav(const int16_t *pcm, uint32_t frames, int channels, int rate, uint32_t *size)
{
    uint32_t data = frames * channels * 2;
    uint8_t *w = (uint8_t *)game_malloc(44 + data);
    if (w == NULL) return NULL;
    memcpy(w, "RIFF", 4);
    uint32_t v = 36 + data; memcpy(w + 4, &v, 4);
    memcpy(w + 8, "WAVEfmt ", 8);
    v = 16; memcpy(w + 16, &v, 4);
    uint16_t s = WAVE_FORMAT_PCM; memcpy(w + 20, &s, 2);
    s = (uint16_t)channels; memcpy(w + 22, &s, 2);
    v = (uint32_t)rate; memcpy(w + 24, &v, 4);
    v = (uint32_t)rate * channels * 2; memcpy(w + 28, &v, 4);
    s = (uint16_t)(channels * 2); memcpy(w + 32, &s, 2);
    s = 16; memcpy(w + 34, &s, 2);
    memcpy(w + 36, "data", 4);
    memcpy(w + 40, &data, 4);
    if (data) memcpy(w + 44, pcm, data);
    *size = 44 + data;
    return w;
}

// AILSOUNDINFO
typedef struct
{
    int32_t format;
    uint32_t data_ptr;
    uint32_t data_len;
    uint32_t rate;
    int32_t bits;
    int32_t channels;
    uint32_t samples;
    uint32_t block_size;
    uint32_t initial_ptr;
} ail_soundinfo;

int32_t CCALL AIL_decompress_ADPCM_c(const ail_soundinfo *info, uint32_t *outdata, uint32_t *outsize)
{
    int16_t *pcm;
    uint32_t frames, size;
    uint8_t *w;
    if ((info == NULL) || (outdata == NULL) || (outsize == NULL)) return 0;
    frames = decode_ima((const uint8_t *)(uintptr_t)info->data_ptr, info->data_len, info->channels, (int)info->block_size, &pcm);
    if (pcm == NULL) return 0;
    if ((info->samples != 0) && (frames > info->samples)) frames = info->samples;
    w = make_wav(pcm, frames, info->channels, (int)info->rate, &size);
    free(pcm);
    if (w == NULL) return 0;
    *outdata = (uint32_t)(uintptr_t)w;
    *outsize = size;
    return 1;
}

int32_t CCALL AIL_decompress_ASI_c(const void *indata, uint32_t insize, const char *ext, uint32_t *outdata, uint32_t *outsize, uint32_t callback)
{
    int16_t *pcm;
    int channels, rate;
    uint32_t frames, size;
    uint8_t *w;
    if ((indata == NULL) || (outdata == NULL) || (outsize == NULL)) return 0;
    frames = decode_mp3((const uint8_t *)indata, insize, &channels, &rate, &pcm);
    if ((pcm == NULL) || (frames == 0)) { free(pcm); return 0; }
    w = make_wav(pcm, frames, channels, rate, &size);
    free(pcm);
    if (w == NULL) return 0;
    *outdata = (uint32_t)(uintptr_t)w;
    *outsize = size;
    return 1;
}

/* ------------------------------------------------------------------ */
/* samples                                                             */

#define SAMPLE_MAGIC 0x504d4153     // "SAMP"
#define STREAM_MAGIC 0x4d525453     // "STRM"

typedef struct ail_sample
{
    uint32_t magic;
    int is3d, stream;
    // the sound: 8 or 16 bit PCM, mono or stereo
    const uint8_t *data;
    uint32_t len;               // bytes
    int channels, bits, rate;
    void *owned;                // decoded data (ADPCM, streams)
    // playback
    int status;
    double pos;                 // frames
    int volume, pan;            // 0-127
    int loop_count;             // 0: forever, n: plays n times
    int loops_done;
    int32_t loop_start, loop_end;   // bytes (end -1: the end)
    int32_t user[8];
    // 3D
    float p[3], v[3], face[3], up[3];
    float min_dist, max_dist;
    struct ail_sample *next;    // in the playing list
} ail_sample;

static ail_sample *samples;     // every allocated sample and stream
static ail_sample listener_init;    // the 3D listener (position and orientation; the game sees it: LOW_OBJ)
LOW_SLOT(listener)
#define listener LOW_OBJ(listener)
static mixer_source source;

static uint32_t frame_bytes(const ail_sample *s) { return (uint32_t)(s->channels * (s->bits / 8)); }

// one sample into the mix (stereo float), with the mixer locked
static void mix_sample(ail_sample *s, float *out, int frames)
{
    uint32_t fb = frame_bytes(s), total;
    double step, end, start;
    float gl, gr, vol;
    if ((s->status != SMP_PLAYING) || (s->data == NULL) || (fb == 0) || (s->rate <= 0)) return;
    total = s->len / fb;
    if (total == 0) { s->status = SMP_DONE; return; }
    step = (double)s->rate / MIXER_RATE;
    vol = (s->volume / 127.0f) * (master_volume / 127.0f);
    if (s->is3d)
    {
        // distance attenuation (full inside min, silent at max) and pan from the listener
        float d[3] = { s->p[0] - listener.p[0], s->p[1] - listener.p[1], s->p[2] - listener.p[2] };
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float att = 1.0f;
        if (dist > s->min_dist) att = (dist >= s->max_dist) ? 0.0f : 1.0f - (dist - s->min_dist) / (s->max_dist - s->min_dist);
        vol *= att;
        // right = up x face (left-handed coordinates)
        float r[3] = { listener.up[1] * listener.face[2] - listener.up[2] * listener.face[1], listener.up[2] * listener.face[0] - listener.up[0] * listener.face[2],
                       listener.up[0] * listener.face[1] - listener.up[1] * listener.face[0] };
        float x = (dist > 0.001f) ? (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / dist : 0.0f;
        gl = vol * fminf(1.0f, 1.0f - x);
        gr = vol * fminf(1.0f, 1.0f + x);
    }
    else
    {
        gl = vol * fminf(1.0f, (127 - s->pan) / 64.0f);
        gr = vol * fminf(1.0f, s->pan / 64.0f);
    }
    // the loop range (frames)
    start = (s->loop_start > 0) ? (double)(s->loop_start / fb) : 0.0;
    end = ((s->loop_end < 0) || ((uint32_t)s->loop_end > s->len)) ? (double)total : (double)(s->loop_end / fb);
    if (end <= start) { start = 0.0; end = total; }
    for (int i = 0; i < frames; i++)
    {
        uint32_t k = (uint32_t)s->pos;
        float a, b;
        if (k >= total) k = total - 1;
        const uint8_t *f = s->data + (size_t)k * fb;
        if (s->bits == 16)
        {
            a = (int16_t)rd16(f) / 32768.0f;
            b = (s->channels == 2) ? (int16_t)rd16(f + 2) / 32768.0f : a;
        }
        else
        {
            a = (f[0] - 128) / 128.0f;
            b = (s->channels == 2) ? (f[1] - 128) / 128.0f : a;
        }
        out[2 * i] += a * gl;
        out[2 * i + 1] += b * gr;
        s->pos += step;
        // the end of the loop range: again (loop count 0 = forever), or past it to the end
        int loops_left = (s->loop_count == 0) || (s->loops_done + 1 < s->loop_count);
        if (loops_left && (s->pos >= end))
        {
            s->pos = start + (s->pos - end);
            if (s->loop_count != 0) s->loops_done++;
        }
        else if (s->pos >= total)
        {
            s->status = SMP_DONE;
            s->pos = 0.0;
            break;
        }
    }
}

static void mix_all(mixer_source *src, float *out, int frames)
{
    for (ail_sample *s = samples; s != NULL; s = s->next) mix_sample(s, out, frames);
    (void)src;
}

static int audio_ready(void)
{
    if (mixer_ok < 0)
    {
        mixer_ok = mixer_init();
        if (mixer_ok)
        {
            source.mix = mix_all;
            mixer_add_source(&source);
        }
    }
    return mixer_ok;
}

static ail_sample *new_sample(uint32_t magic, int is3d)
{
    ail_sample *s = (ail_sample *)game_malloc(sizeof(ail_sample));
    if (s == NULL) return NULL;
    memset(s, 0, sizeof(*s));
    s->magic = magic;
    s->is3d = is3d;
    s->stream = (magic == STREAM_MAGIC);
    s->status = SMP_DONE;
    s->volume = 127;
    s->pan = 64;
    s->loop_count = 1;
    s->loop_end = -1;
    s->min_dist = 1.0f;
    s->max_dist = 1000.0f;
    mixer_lock();
    s->next = samples;
    samples = s;
    mixer_unlock();
    return s;
}

static void free_sample(ail_sample *s)
{
    ail_sample **p;
    mixer_lock();
    for (p = &samples; *p != NULL; p = &(*p)->next)
    {
        if (*p == s) { *p = s->next; break; }
    }
    mixer_unlock();
    free(s->owned);
    s->magic = 0;
    game_free(s);
}

static ail_sample *sample(void *h, uint32_t magic)
{
    ail_sample *s = (ail_sample *)h;
    return ((s != NULL) && (s->magic == magic)) ? s : NULL;
}
#define SAMPLE(h) ail_sample *s = sample((h), SAMPLE_MAGIC); if (s == NULL) return
#define STREAM(h) ail_sample *s = sample((h), STREAM_MAGIC); if (s == NULL) return

// sets the sound of a sample from a WAV format: PCM is played in place, IMA ADPCM decoded
static int set_sound(ail_sample *s, int format, int channels, int rate, int bits, int block_align, const uint8_t *data, uint32_t len)
{
    mixer_lock();
    free(s->owned);
    s->owned = NULL;
    s->data = NULL;
    s->len = 0;
    s->pos = 0.0;
    s->loops_done = 0;
    if ((format == WAVE_FORMAT_PCM) && ((bits == 8) || (bits == 16)) && ((channels == 1) || (channels == 2)))
    {
        s->data = data;
        s->len = len;
        s->channels = channels;
        s->bits = bits;
        s->rate = rate;
    }
    else if ((format == WAVE_FORMAT_IMA_ADPCM) || (format == WAVE_FORMAT_MPEGLAYER3))
    {
        int16_t *pcm = NULL;
        uint32_t frames;
        if (format == WAVE_FORMAT_IMA_ADPCM) frames = decode_ima(data, len, channels, block_align, &pcm);
        else frames = decode_mp3(data, len, &channels, &rate, &pcm);
        if (pcm != NULL)
        {
            s->owned = pcm;
            s->data = (const uint8_t *)pcm;
            s->len = frames * channels * 2;
            s->channels = channels;
            s->bits = 16;
            s->rate = rate;
        }
    }
    else if (winapi_debug) eprintf("miles: sound format %d (%d bits, %d channels) not supported\n", format, bits, channels);
    mixer_unlock();
    return s->data != NULL;
}

/* ------------------------------------------------------------------ */
/* the digital driver                                                  */

static uint32_t *dig_driver;

// AIL_waveOutOpen(HDIGDRIVER *drvr, LPHWAVEOUT *lphWaveOut, int wDeviceID, LPWAVEFORMAT lpFormat): 0 = success
int32_t CCALL AIL_waveOutOpen_c(uint32_t *drvr, void *lphWaveOut, int32_t wDeviceID, void *lpFormat)
{
    if (drvr != NULL) *drvr = 0;
    if (!audio_ready())
    {
        eprintf("miles: no audio device, the game runs without sound\n");
        return -1;
    }
    if (dig_driver == NULL)
    {
        dig_driver = (uint32_t *)game_malloc(64);
        memcpy(dig_driver, "DIGD", 4);
    }
    if (drvr != NULL) *drvr = (uint32_t)(uintptr_t)dig_driver;
    if (winapi_debug) eprintf("miles: digital driver open\n");
    return 0;
}

void CCALL AIL_waveOutClose_c(void *drvr)
{
    // the samples stop (the engine releases them before)
    mixer_lock();
    for (ail_sample *s = samples; s != NULL; s = s->next) s->status = SMP_DONE;
    mixer_unlock();
}

int32_t CCALL AIL_digital_handle_release_c(void *drvr) { return 1; }
int32_t CCALL AIL_digital_handle_reacquire_c(void *drvr) { return 1; }
void CCALL AIL_set_digital_master_volume_c(void *drvr, int32_t volume) { master_volume = (volume < 0) ? 0 : ((volume > 127) ? 127 : volume); }
int32_t CCALL AIL_digital_master_volume_c(void *drvr) { return master_volume; }

/* ------------------------------------------------------------------ */
/* 2D samples                                                          */

void * CCALL AIL_allocate_sample_handle_c(void *drvr) { return new_sample(SAMPLE_MAGIC, 0); }
void CCALL AIL_release_sample_handle_c(void *h) { SAMPLE(h); free_sample(s); }

void CCALL AIL_init_sample_c(void *h)
{
    SAMPLE(h);
    mixer_lock();
    free(s->owned);
    s->owned = NULL;
    s->data = NULL;
    s->len = 0;
    s->status = SMP_DONE;
    s->pos = 0.0;
    s->volume = 127;
    s->pan = 64;
    s->loop_count = 1;
    s->loops_done = 0;
    s->loop_start = 0;
    s->loop_end = -1;
    s->channels = 1;
    s->bits = 16;
    s->rate = 22050;
    mixer_unlock();
}

int32_t CCALL AIL_set_sample_file_c(void *h, const uint8_t *file, int32_t block)
{
    wav_info w;
    SAMPLE(h) 0;
    if ((file == NULL) || memcmp(file, "RIFF", 4) || !parse_wav(file, rd32(file + 4) + 8, &w)) return 0;
    return set_sound(s, w.format, w.channels, w.rate, w.bits, w.block_align, w.data, w.len);
}

void CCALL AIL_set_sample_address_c(void *h, const uint8_t *start, uint32_t len)
{
    SAMPLE(h);
    mixer_lock();
    free(s->owned);
    s->owned = NULL;
    s->data = start;
    s->len = len;
    s->pos = 0.0;
    s->loops_done = 0;
    mixer_unlock();
}

void CCALL AIL_set_sample_type_c(void *h, int32_t format, uint32_t flags)
{
    SAMPLE(h);
    mixer_lock();
    s->channels = (format & 2) ? 2 : 1;
    s->bits = (format & 1) ? 16 : 8;
    mixer_unlock();
}

void CCALL AIL_set_sample_playback_rate_c(void *h, int32_t rate) { SAMPLE(h); s->rate = rate; }
void CCALL AIL_set_sample_volume_c(void *h, int32_t volume) { SAMPLE(h); s->volume = (volume < 0) ? 0 : ((volume > 127) ? 127 : volume); }
void CCALL AIL_set_sample_pan_c(void *h, int32_t pan) { SAMPLE(h); s->pan = (pan < 0) ? 0 : ((pan > 127) ? 127 : pan); }
int32_t CCALL AIL_sample_volume_c(void *h) { SAMPLE(h) 0; return s->volume; }
int32_t CCALL AIL_sample_pan_c(void *h) { SAMPLE(h) 64; return s->pan; }
void CCALL AIL_set_sample_user_data_c(void *h, uint32_t index, int32_t value) { SAMPLE(h); if (index < 8) s->user[index] = value; }
int32_t CCALL AIL_sample_user_data_c(void *h, uint32_t index) { SAMPLE(h) 0; return (index < 8) ? s->user[index] : 0; }

void CCALL AIL_stop_sample_c(void *h) { SAMPLE(h); mixer_lock(); if (s->status == SMP_PLAYING) s->status = SMP_STOPPED; mixer_unlock(); }

void CCALL AIL_resume_sample_c(void *h)
{
    SAMPLE(h);
    if ((winapi_debug >= 2) && (s->status != SMP_PLAYING))
        eprintf("miles: sample %p plays %u bytes, %d Hz, %d bits, %d channels, volume %d, pan %d, loop %d (%d..%d), from %.0f\n", (void *)s, s->len, s->rate, s->bits, s->channels,
                s->volume, s->pan, s->loop_count, s->loop_start, s->loop_end, s->pos);
    mixer_lock();
    if ((s->status == SMP_STOPPED) || (s->status == SMP_DONE)) s->status = SMP_PLAYING;
    mixer_unlock();
}

void CCALL AIL_set_sample_loop_count_c(void *h, int32_t count) { SAMPLE(h); mixer_lock(); s->loop_count = (count < 0) ? 1 : count; s->loops_done = 0; mixer_unlock(); }
void CCALL AIL_set_sample_loop_block_c(void *h, int32_t start, int32_t end) { SAMPLE(h); mixer_lock(); s->loop_start = start; s->loop_end = end; mixer_unlock(); }

void CCALL AIL_set_sample_ms_position_c(void *h, int32_t ms)
{
    SAMPLE(h);
    mixer_lock();
    s->pos = (ms > 0) ? (double)ms * s->rate / 1000.0 : 0.0;
    s->loops_done = 0;
    mixer_unlock();
}

void CCALL AIL_set_sample_reverb_c(void *h, uint32_t level, uint32_t reflect, uint32_t decay) { }

// no filters
uint32_t CCALL AIL_set_sample_processor_c(void *h, uint32_t stage, uint32_t provider) { return 0; }
int32_t CCALL AIL_enumerate_filters_c(uint32_t *next, uint32_t *dest, uint32_t *name) { return 0; }
int32_t CCALL AIL_enumerate_filter_sample_attributes_c(uint32_t lib, uint32_t *next, void *dest) { return 0; }
void CCALL AIL_filter_sample_attribute_c(void *h, const char *name, uint32_t *val) { if (val != NULL) *val = 0; }
void CCALL AIL_set_filter_sample_preference_c(void *h, const char *name, const void *val) { }

/* ------------------------------------------------------------------ */
/* streams                                                             */

// "\\\\<handle>": the engine's file handle, at the start of a WAV file (in a REZ archive); it's read whole
void * CCALL AIL_open_stream_c(void *drvr, const char *name, int32_t mem)
{
    uint8_t head[12];
    uint32_t got = 0, size;
    void *file;
    uint8_t *buf;
    wav_info w;
    ail_sample *s;

    if ((name == NULL) || (strncmp(name, "\\\\\\\\", 4) != 0))
    {
        if (winapi_debug) eprintf("miles: stream \"%s\" not supported\n", name ? name : "(null)");
        return NULL;
    }
    file = (void *)(uintptr_t)strtoul(name + 4, NULL, 10);
    if (!ReadFile_c(file, head, 12, &got, NULL) || (got != 12) || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) return NULL;
    size = rd32(head + 4) + 8;
    if ((size < 12) || (size > 0x10000000)) return NULL;
    buf = (uint8_t *)malloc(size);
    if (buf == NULL) return NULL;
    memcpy(buf, head, 12);
    got = 0;
    ReadFile_c(file, buf + 12, size - 12, &got, NULL);
    size = 12 + got;
    if (!parse_wav(buf, size, &w)) { free(buf); return NULL; }
    s = new_sample(STREAM_MAGIC, 0);
    if (s == NULL) { free(buf); return NULL; }
    if (!set_sound(s, w.format, w.channels, w.rate, w.bits, w.block_align, w.data, w.len)) { free(buf); free_sample(s); return NULL; }
    if (s->owned == NULL) s->owned = buf;    // PCM: played from the file data
    else free(buf);
    if (winapi_debug) eprintf("miles: stream %u bytes, format %d, %d Hz, %d channels\n", size, w.format, w.rate, w.channels);
    return s;
}

void CCALL AIL_close_stream_c(void *h) { STREAM(h); free_sample(s); }
void CCALL AIL_start_stream_c(void *h) { STREAM(h); mixer_lock(); s->pos = 0.0; s->loops_done = 0; s->status = SMP_PLAYING; mixer_unlock(); }
void CCALL AIL_pause_stream_c(void *h, int32_t onoff)
{
    STREAM(h);
    mixer_lock();
    if (onoff) { if (s->status == SMP_PLAYING) s->status = SMP_STOPPED; }
    else if ((s->status == SMP_STOPPED) || (s->status == SMP_DONE)) s->status = SMP_PLAYING;
    mixer_unlock();
}
void CCALL AIL_set_stream_loop_count_c(void *h, int32_t count) { STREAM(h); mixer_lock(); s->loop_count = (count < 0) ? 1 : count; s->loops_done = 0; mixer_unlock(); }
void CCALL AIL_set_stream_ms_position_c(void *h, int32_t ms) { STREAM(h); mixer_lock(); s->pos = (ms > 0) ? (double)ms * s->rate / 1000.0 : 0.0; mixer_unlock(); }
void CCALL AIL_set_stream_playback_rate_c(void *h, int32_t rate) { STREAM(h); s->rate = rate; }
void CCALL AIL_set_stream_volume_c(void *h, int32_t volume) { STREAM(h); s->volume = (volume < 0) ? 0 : ((volume > 127) ? 127 : volume); }
void CCALL AIL_set_stream_pan_c(void *h, int32_t pan) { STREAM(h); s->pan = (pan < 0) ? 0 : ((pan > 127) ? 127 : pan); }
int32_t CCALL AIL_stream_volume_c(void *h) { STREAM(h) 0; return s->volume; }
int32_t CCALL AIL_stream_pan_c(void *h) { STREAM(h) 64; return s->pan; }
void CCALL AIL_set_stream_user_data_c(void *h, uint32_t index, int32_t value) { STREAM(h); if (index < 8) s->user[index] = value; }

/* ------------------------------------------------------------------ */
/* the 3D provider                                                     */

#define PROVIDER 1
static char *provider_name;

int32_t CCALL AIL_enumerate_3D_providers_c(uint32_t *next, uint32_t *dest, uint32_t *name)
{
    if ((next == NULL) || (*next != 0)) return 0;
    if (provider_name == NULL)
    {
        provider_name = (char *)game_malloc(64);
        strcpy(provider_name, "Miles 2D Stereo Positional Audio");
    }
    *next = 1;
    if (dest != NULL) *dest = PROVIDER;
    if (name != NULL) *name = (uint32_t)(uintptr_t)provider_name;
    return 1;
}

int32_t CCALL AIL_open_3D_provider_c(uint32_t lib) { return (lib == PROVIDER) ? 0 : 1; }   // M3D_NOERR
void CCALL AIL_close_3D_provider_c(uint32_t lib) { }

void CCALL AIL_3D_provider_attribute_c(uint32_t lib, const char *name, int32_t *val)
{
    if (val == NULL) return;
    if ((name != NULL) && !strcmp(name, "Maximum supported samples")) *val = 64;
    else *val = -1;     // "EAX environment selection": none
}

void CCALL AIL_set_3D_room_type_c(uint32_t lib, int32_t type) { }
int32_t CCALL AIL_3D_room_type_c(uint32_t lib) { return -1; }

void * CCALL AIL_open_3D_listener_c(uint32_t lib)
{
    listener.magic = SAMPLE_MAGIC;
    listener.face[2] = 1.0f;
    listener.up[1] = 1.0f;
    return &listener;
}

void CCALL AIL_close_3D_listener_c(void *h) { }

void * CCALL AIL_allocate_3D_sample_handle_c(uint32_t lib) { return new_sample(SAMPLE_MAGIC, 1); }
void CCALL AIL_release_3D_sample_handle_c(void *h) { SAMPLE(h); if (s != &listener) free_sample(s); }

static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

void CCALL AIL_set_3D_position_c(void *h, uint32_t x, uint32_t y, uint32_t z) { SAMPLE(h); s->p[0] = f32(x); s->p[1] = f32(y); s->p[2] = f32(z); }
void CCALL AIL_set_3D_velocity_vector_c(void *h, uint32_t x, uint32_t y, uint32_t z) { SAMPLE(h); s->v[0] = f32(x); s->v[1] = f32(y); s->v[2] = f32(z); }
void CCALL AIL_set_3D_orientation_c(void *h, uint32_t fx, uint32_t fy, uint32_t fz, uint32_t ux, uint32_t uy, uint32_t uz)
{
    SAMPLE(h);
    s->face[0] = f32(fx); s->face[1] = f32(fy); s->face[2] = f32(fz);
    s->up[0] = f32(ux); s->up[1] = f32(uy); s->up[2] = f32(uz);
}
void CCALL AIL_3D_position_c(void *h, float *x, float *y, float *z) { SAMPLE(h); if (x) *x = s->p[0]; if (y) *y = s->p[1]; if (z) *z = s->p[2]; }
void CCALL AIL_3D_velocity_c(void *h, float *x, float *y, float *z) { SAMPLE(h); if (x) *x = s->v[0]; if (y) *y = s->v[1]; if (z) *z = s->v[2]; }
void CCALL AIL_3D_orientation_c(void *h, float *fx, float *fy, float *fz, float *ux, float *uy, float *uz)
{
    SAMPLE(h);
    if (fx) *fx = s->face[0];
    if (fy) *fy = s->face[1];
    if (fz) *fz = s->face[2];
    if (ux) *ux = s->up[0];
    if (uy) *uy = s->up[1];
    if (uz) *uz = s->up[2];
}
void CCALL AIL_set_3D_user_data_c(void *h, uint32_t index, int32_t value) { SAMPLE(h); if (index < 8) s->user[index] = value; }
int32_t CCALL AIL_3D_user_data_c(void *h, uint32_t index) { SAMPLE(h) 0; return (index < 8) ? s->user[index] : 0; }

int32_t CCALL AIL_set_3D_sample_info_c(void *h, const ail_soundinfo *info)
{
    SAMPLE(h) 0;
    if (info == NULL) return 0;
    return set_sound(s, info->format, info->channels, (int)info->rate, info->bits, (int)info->block_size, (const uint8_t *)(uintptr_t)info->data_ptr, info->data_len);
}

void CCALL AIL_start_3D_sample_c(void *h) { SAMPLE(h); mixer_lock(); s->pos = 0.0; s->loops_done = 0; s->status = SMP_PLAYING; mixer_unlock(); }
void CCALL AIL_stop_3D_sample_c(void *h) { AIL_stop_sample_c(h); }
void CCALL AIL_resume_3D_sample_c(void *h) { AIL_resume_sample_c(h); }
void CCALL AIL_end_3D_sample_c(void *h) { SAMPLE(h); mixer_lock(); s->status = SMP_DONE; s->pos = 0.0; mixer_unlock(); }
void CCALL AIL_set_3D_sample_volume_c(void *h, int32_t volume) { AIL_set_sample_volume_c(h, volume); }
int32_t CCALL AIL_3D_sample_volume_c(void *h) { return AIL_sample_volume_c(h); }
void CCALL AIL_set_3D_sample_playback_rate_c(void *h, int32_t rate) { AIL_set_sample_playback_rate_c(h, rate); }
void CCALL AIL_set_3D_sample_offset_c(void *h, uint32_t offset) { SAMPLE(h); mixer_lock(); s->pos = frame_bytes(s) ? (double)(offset / frame_bytes(s)) : 0.0; mixer_unlock(); }
void CCALL AIL_set_3D_sample_loop_count_c(void *h, uint32_t loops) { AIL_set_sample_loop_count_c(h, (int32_t)loops); }
void CCALL AIL_set_3D_sample_loop_block_c(void *h, int32_t start, int32_t end) { AIL_set_sample_loop_block_c(h, start, end); }
uint32_t CCALL AIL_3D_sample_status_c(void *h) { SAMPLE(h) SMP_FREE; return (uint32_t)s->status; }
void CCALL AIL_set_3D_sample_distances_c(void *h, uint32_t max_dist, uint32_t min_dist)
{
    SAMPLE(h);
    s->max_dist = f32(max_dist);
    s->min_dist = f32(min_dist);
    if (!(s->max_dist > s->min_dist)) s->max_dist = s->min_dist + 1.0f;
}
void CCALL AIL_set_3D_sample_preference_c(void *h, const char *name, const void *val) { }
void CCALL AIL_set_3D_sample_effects_level_c(void *h, uint32_t level) { }
void CCALL AIL_set_3D_sample_obstruction_c(void *h, uint32_t v) { }
void CCALL AIL_set_3D_sample_occlusion_c(void *h, uint32_t v) { }

EXTERN_C_END
