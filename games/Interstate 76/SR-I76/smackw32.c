/**
 *
 *  SMACKW32.DLL (RAD Smacker 3.x API) emulation, using the Smacker decoder from Albion (smack.c).
 *
 *  The game reads fields of the Smack and SmackBuf structures directly, so their layout
 *  (at least of the used fields) matches the original structures.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <SDL.h>
#include "platform.h"
#include "config.h"
#include "smack.h"
#include "vfs.h"
#include "winapi.h"
#include "winapi-gdi32.h"
#include "mixer.h"
#include "ptr32.h"
#include "Game-Memory.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#pragma pack(push, 4)

// RAD Smack structure (smack.h 3.x)
typedef struct {
    uint32_t Version;           // 0
    uint32_t Width;             // 4
    uint32_t Height;            // 8
    uint32_t Frames;            // 12
    uint32_t MSPerFrame;        // 16
    uint32_t SmackerType;       // 20
    uint32_t LargestInTrack[7]; // 24
    uint32_t tablesize;         // 52
    uint32_t codesize;          // 56
    uint32_t absize;            // 60
    uint32_t detailsize;        // 64
    uint32_t typesize;          // 68
    uint32_t TrackType[7];      // 72
    uint32_t extra;             // 100
    uint32_t NewPalette;        // 104
    uint8_t Palette[772];       // 108
    uint32_t PalType;           // 880
    uint32_t FrameNum;          // 884
    uint32_t FrameSize;         // 888
    uint32_t SndSize;           // 892
    int32_t LastRectx;          // 896
    int32_t LastRecty;          // 900
    int32_t LastRectw;          // 904
    int32_t LastRecth;          // 908
    uint32_t OpenFlags;         // 912
    uint32_t LeftOfs;           // 916
    uint32_t TopOfs;            // 920
    uint32_t LargestFrameSize;  // 924
    uint32_t Highest1SecRate;   // 928
    uint32_t Highest1SecFrame;  // 932
    uint32_t ReadError;         // 936
    uint32_t addr32;            // 940

    // private data
    FILE *file;
    SmackStruct *decoder;
    SmackFrame *frame;
    uint8_t *dest;              // SmackToBuffer destination
    int32_t dest_left, dest_top, dest_pitch, dest_height;
    int rect_pending;
    uint32_t next_frame_time;
    uint32_t ms_per_frame_x100;
    uint8_t prev_palette[768];
    struct smk_audio *audio;    // NULL = no sound
} rad_smack;

// SmackBuf: only the fields used by the game are at their original offsets
typedef struct {
    uint32_t Reversed;          // 0
    uint32_t SurfaceType;       // 4
    uint32_t BlitType;          // 8
    uint32_t FullScreen;        // 12
    uint32_t Width;             // 16
    uint32_t Height;            // 20
    uint32_t Pitch;             // 24  (used as pitch by the game: SmackToBuffer(..., +24, +20, +1096, +0))
    uint32_t reserved1[4];      // 28
    uint32_t PalColorsInUse;    // 44
    uint32_t StartPalColor;     // 48
    uint32_t EndPalColor;       // 52
    uint32_t Palette[256];      // 56  (XRGB)
    uint32_t PalType;           // 1080
    uint32_t forceredraw;       // 1084
    uint32_t didapalette;       // 1088
    uint32_t reserved2;         // 1092
    PTR32(uint8_t) Buffer;      // 1096
    uint32_t reserved3[16];
} rad_smackbuf;

#pragma pack(pop)

_Static_assert(offsetof(rad_smack, NewPalette) == 104, "Smack layout");
_Static_assert(offsetof(rad_smack, FrameNum) == 884, "Smack layout");
_Static_assert(offsetof(rad_smack, LastRectx) == 896, "Smack layout");
_Static_assert(offsetof(rad_smackbuf, PalColorsInUse) == 44, "SmackBuf layout");
_Static_assert(offsetof(rad_smackbuf, forceredraw) == 1084, "SmackBuf layout");
_Static_assert(offsetof(rad_smackbuf, Buffer) == 1096, "SmackBuf layout");


static void update_palette(rad_smack *s)
{
    int i;
    uint8_t pal[768];

    for (i = 0; i < 256; i++)
    {
        pal[i * 3] = (*s->frame->Palette)[i].r;
        pal[i * 3 + 1] = (*s->frame->Palette)[i].g;
        pal[i * 3 + 2] = (*s->frame->Palette)[i].b;
    }
    if ((s->FrameNum == 0) || (memcmp(pal, s->prev_palette, 768) != 0))
    {
        memcpy(s->prev_palette, pal, 768);
        memcpy(s->Palette, pal, 768);
        s->NewPalette = 1;
    }
    else
    {
        s->NewPalette = 0;
    }
}

/* audio: the decoded PCM of audio track 0 goes into a ring buffer that a mixer source plays */

typedef struct smk_audio {
    mixer_source source;
    uint32_t rate, channels, bits, block;
    uint8_t *ring;
    uint32_t size, rd, wr, fill;    // bytes
    double frac;
    int on;
} smk_audio;

static void smk_audio_mix(mixer_source *source, float *out, int frames)
{
    smk_audio *a = (smk_audio *)source;
    double step = (double)a->rate / MIXER_RATE;
    int i;

    if (!a->on) return;
    for (i = 0; i < frames; i++)
    {
        float l, r;
        const uint8_t *p;

        if (a->fill < a->block) break;
        p = a->ring + a->rd;
        if (a->bits == 16)
        {
            l = ((const int16_t *)p)[0] * (1.0f / 32768.0f);
            r = (a->channels == 2) ? ((const int16_t *)p)[1] * (1.0f / 32768.0f) : l;
        }
        else
        {
            l = (p[0] - 128) * (1.0f / 128.0f);
            r = (a->channels == 2) ? (p[1] - 128) * (1.0f / 128.0f) : l;
        }
        out[2 * i] += l;
        out[2 * i + 1] += r;

        a->frac += step;
        while ((a->frac >= 1.0) && (a->fill >= a->block))
        {
            a->frac -= 1.0;
            a->rd = (a->rd + a->block) % a->size;
            a->fill -= a->block;
        }
    }
}

static void smk_audio_open(rad_smack *s)
{
    uint32_t info = s->decoder->AudioRate[0];
    smk_audio *a;

    if ((s->decoder->AudioSize[0] == 0) || !(info & 0x40000000)) return;
    if ((getenv("I76_NOSOUND") != NULL) || !config_get_int("sound", 1)) return;
    if (!mixer_init()) return;

    a = (smk_audio *)calloc(1, sizeof(smk_audio));
    a->rate = info & 0xFFFFFF;
    a->channels = (info & 0x10000000) ? 2 : 1;
    a->bits = (info & 0x20000000) ? 16 : 8;
    a->block = a->channels * a->bits / 8;
    a->size = a->rate * a->block * 8;           // 8 seconds
    a->size -= a->size % a->block;
    a->ring = (uint8_t *)malloc(a->size);
    a->on = 1;
    if ((a->rate == 0) || (a->ring == NULL))
    {
        free(a->ring);
        free(a);
        return;
    }
    a->source.mix = smk_audio_mix;
    s->audio = a;
    mixer_add_source(&a->source);
}

static void smk_audio_close(rad_smack *s)
{
    if (s->audio == NULL) return;
    mixer_remove_source(&s->audio->source);
    free(s->audio->ring);
    free(s->audio);
    s->audio = NULL;
}

static void smk_audio_push(rad_smack *s)
{
    smk_audio *a = s->audio;
    const uint8_t *src = s->frame->Audio;
    uint32_t len = s->frame->AudioLength;

    if ((a == NULL) || (src == NULL) || (len == 0)) return;
    mixer_lock();
    if (len > a->size - a->fill) len = a->size - a->fill;   // overflow: drop the rest
    len -= len % a->block;
    while (len > 0)
    {
        uint32_t n = a->size - a->wr;
        if (n > len) n = len;
        memcpy(a->ring + a->wr, src, n);
        a->wr = (a->wr + n) % a->size;
        a->fill += n;
        src += n;
        len -= n;
    }
    mixer_unlock();
}

static void decode_current(rad_smack *s)
{
    SmackDecodeFrame(s->decoder, s->frame, s->frame, (s->audio != NULL) ? 0 : -1);
    update_palette(s);
    smk_audio_push(s);
}

rad_smack * CCALL SmackOpen_c(const char *name, uint32_t flags, uint32_t extrabuf)
{
    char path[1024];
    rad_smack *s;
    FILE *f;

    if (name == NULL) return NULL;
    if ((flags & 0x1000) != 0)
    {
        eprintf("SmackOpen: opening by file handle is not supported\n");
        return NULL;
    }

    if (!vfs_resolve(name, path, sizeof(path)))
    {
        if (winapi_debug) eprintf("SmackOpen: %s not found\n", name);
        return NULL;
    }
    {
        struct stat st;
        if ((stat(path, &st) != 0) || S_ISDIR(st.st_mode)) return NULL;
    }
    f = fopen(path, "rb");
    if (f == NULL) return NULL;

    s = (rad_smack *) game_calloc(1, sizeof(rad_smack));
    s->file = f;
    s->decoder = SmackOpen(f);
    if (s->decoder == NULL)
    {
        eprintf("SmackOpen: %s: error %d\n", path, SmackError());
        fclose(f);
        game_free(s);
        return NULL;
    }
    smk_audio_open(s);
    s->frame = SmackAllocateFrame(s->decoder, NULL, 0, 0, (s->audio != NULL) ? 1 : 0, 0, 0, 0);
    if (s->frame == NULL)
    {
        smk_audio_close(s);
        SmackClose(s->decoder);
        fclose(f);
        game_free(s);
        return NULL;
    }

    s->Version = s->decoder->Signature;
    s->Width = s->decoder->OriginalWidth;
    s->Height = s->decoder->OriginalHeight;
    s->Frames = s->decoder->Frames;
    s->OpenFlags = flags;

    // frame rate: > 0 ms per frame, < 0 in 1/100000 s, 0 = 10 fps
    if (s->decoder->FrameRate > 0) s->ms_per_frame_x100 = s->decoder->FrameRate * 100;
    else if (s->decoder->FrameRate < 0) s->ms_per_frame_x100 = -s->decoder->FrameRate;
    else s->ms_per_frame_x100 = 10000;
    s->MSPerFrame = s->ms_per_frame_x100 / 100;

    // decode the first frame (for the palette)
    decode_current(s);
    s->next_frame_time = SDL_GetTicks();

    if (winapi_debug) eprintf("SmackOpen: %s %ux%u, %u frames, %u.%02u ms/frame\n", name, s->Width, s->Height, s->Frames, s->ms_per_frame_x100 / 100, s->ms_per_frame_x100 % 100);
    return s;
}

void CCALL SmackClose_c(rad_smack *s)
{
    if (s == NULL) return;
    smk_audio_close(s);
    SmackDeallocateFrame(s->frame);
    SmackClose(s->decoder);
    fclose(s->file);
    game_free(s);
}

void CCALL SmackToBuffer_c(rad_smack *s, uint32_t left, uint32_t top, uint32_t pitch, uint32_t destheight, void *buf, uint32_t flags)
{
    if (s == NULL) return;
    s->dest = (uint8_t *) buf;
    s->dest_left = left;
    s->dest_top = top;
    s->dest_pitch = pitch;
    s->dest_height = destheight;
}

uint32_t CCALL SmackDoFrame_c(rad_smack *s)
{
    uint32_t y, w, h;

    if (s == NULL) return 0;
    if (winapi_debug >= 2) eprintf("SmackDoFrame: frame %u dest %p newpal %u\n", s->FrameNum, s->dest, s->NewPalette);

    if (s->dest != NULL)
    {
        w = s->Width;
        h = s->Height;
        if (s->dest_left + w > (uint32_t)s->dest_pitch) w = s->dest_pitch - s->dest_left;
        if (s->dest_top + h > (uint32_t)s->dest_height) h = s->dest_height - s->dest_top;
        for (y = 0; y < h; y++)
        {
            memcpy(s->dest + (size_t)(s->dest_top + y) * s->dest_pitch + s->dest_left, s->frame->Video + (size_t)y * s->decoder->PaddedWidth, w);
        }
        s->LastRectx = s->dest_left;
        s->LastRecty = s->dest_top;
        s->LastRectw = w;
        s->LastRecth = h;
        s->rect_pending = 1;
    }
    return 0;
}

uint32_t CCALL SmackToBufferRect_c(rad_smack *s, uint32_t SmackSurface)
{
    if ((s == NULL) || !s->rect_pending) return 0;
    s->rect_pending = 0;
    return 1;
}

void CCALL SmackNextFrame_c(rad_smack *s)
{
    if (s == NULL) return;

    s->next_frame_time += (s->ms_per_frame_x100 + 50) / 100;
    s->FrameNum++;
    if (s->FrameNum >= s->Frames)
    {
        // loop to the beginning
        s->FrameNum = 0;
    }
    SmackNextFrame(s->decoder);
    decode_current(s);
}

// returns nonzero if it's not yet time for the next frame
uint32_t CCALL SmackWait_c(rad_smack *s)
{
    uint32_t now;

    if (s == NULL) return 0;
    now = SDL_GetTicks();
    if ((int32_t)(s->next_frame_time - now) > 0)
    {
        // don't burn the CPU in the game's wait loop
        if ((int32_t)(s->next_frame_time - now) > 2) SDL_Delay(1);
        return 1;
    }
    if ((int32_t)(now - s->next_frame_time) > 500) s->next_frame_time = now; // don't try to catch up after a stall
    return 0;
}

uint32_t CCALL SmackSoundOnOff_c(rad_smack *s, uint32_t on)
{
    if ((s != NULL) && (s->audio != NULL))
    {
        mixer_lock();
        s->audio->on = on ? 1 : 0;
        mixer_unlock();
    }
    return 1;
}
uint32_t CCALL SmackSoundUseDirectSound_c(void *dd) { return 0; }
void CCALL SmackColorRemap_c(rad_smack *s, const void *remappal, uint32_t numcolors, uint32_t paltype) {}


rad_smackbuf * CCALL SmackBufferOpen_c(void *hwnd, uint32_t BlitType, uint32_t width, uint32_t height, uint32_t ZoomW, uint32_t ZoomH)
{
    rad_smackbuf *b;

    b = (rad_smackbuf *) game_calloc(1, sizeof(rad_smackbuf));
    b->BlitType = BlitType;
    b->Width = width;
    b->Height = height;
    b->Pitch = width;
    b->PalColorsInUse = 256;
    b->StartPalColor = 0;
    b->EndPalColor = 255;
    b->Buffer = (uint8_t *) game_calloc(1, (size_t)width * height);
    if (winapi_debug) eprintf("SmackBufferOpen: %ux%u blit type %u\n", width, height, BlitType);
    return b;
}

void CCALL SmackBufferClose_c(rad_smackbuf *b)
{
    if (b == NULL) return;
    game_free(b->Buffer);
    game_free(b);
}

void CCALL SmackBufferNewPalette_c(rad_smackbuf *b, const uint8_t *pal, uint32_t paltype)
{
    int i;

    if (winapi_debug >= 2) eprintf("SmackBufferNewPalette: type %u\n", paltype);

    if ((b == NULL) || (pal == NULL)) return;
    for (i = 0; i < 256; i++)
    {
        b->Palette[i] = ((uint32_t)pal[i * 3] << 16) | ((uint32_t)pal[i * 3 + 1] << 8) | pal[i * 3 + 2];
    }
    b->PalType = paltype;
    b->didapalette = 1;
}

uint32_t CCALL SmackBufferSetPalette_c(rad_smackbuf *b) { return 1; }

// returns 0 on success (the game continues blitting the next rectangle)
uint32_t CCALL SmackBufferBlit_c(rad_smackbuf *b, void *hdc, int32_t hwndx, int32_t hwndy, int32_t subx, int32_t suby, int32_t subw, int32_t subh)
{
    static uint32_t *rgb;
    static size_t rgb_size;
    int32_t x, y;

    // the game passes 16-bit values with garbage in the upper halves
    subx = (int16_t) subx;
    suby = (int16_t) suby;
    subw = (int16_t) subw;
    subh = (int16_t) subh;
    hwndx = (int16_t) hwndx;
    hwndy = (int16_t) hwndy;

    if (winapi_debug >= 2) eprintf("SmackBufferBlit: %d,%d %d,%d %dx%d pal[1]=%06x\n", hwndx, hwndy, subx, suby, subw, subh, (b != NULL) ? b->Palette[1] : 0);
    if ((b == NULL) || (hdc == NULL)) return 1;
    if ((subw <= 0) || (subh <= 0))
    {
        subx = 0;
        suby = 0;
        subw = b->Width;
        subh = b->Height;
    }
    if (subx + subw > (int32_t)b->Width) subw = b->Width - subx;
    if (suby + subh > (int32_t)b->Height) subh = b->Height - suby;
    if ((subw <= 0) || (subh <= 0)) return 0;

    if (rgb_size < (size_t)subw * subh)
    {
        rgb_size = (size_t)subw * subh;
        rgb = (uint32_t *) realloc(rgb, rgb_size * 4);
    }
    for (y = 0; y < subh; y++)
    {
        const uint8_t *src = b->Buffer + (size_t)(suby + y) * b->Pitch + subx;
        uint32_t *dst = rgb + (size_t)y * subw;
        for (x = 0; x < subw; x++) dst[x] = b->Palette[src[x]];
    }

    gdi_blit_rgb(hdc, hwndx + subx, hwndy + suby, subw, subh, rgb, subw);
    return 0;
}

EXTERN_C_END
