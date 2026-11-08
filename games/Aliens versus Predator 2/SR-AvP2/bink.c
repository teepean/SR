/**
 *
 *  binkw32.dll replacement: the engine's BinkVideoMgr (lithtech.exe) loads binkw32.dll and asks GetProcAddress for
 *  the functions below (the module table in winapi-kernel32.c maps them to their asm2c stubs). The movies are
 *  decoded with FFmpeg (libavformat/libavcodec: Bink video and audio); without FFmpeg BinkOpen fails and the
 *  engine skips them.
 *
 *  The engine reads the BINK structure itself (Width, Height, Frames, FrameNum); FrameNum counts from 1. Each frame
 *  it asks BinkWait whether it's time for the current frame, then BinkCopyToBuffer (the picture into a DirectDraw
 *  surface, render.c), BinkDoFrame, and BinkNextFrame; the movie is over at FrameNum == Frames - 1.
 *  The audio track is decoded when the movie opens and plays through the mixer; it is the clock for the frames.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "platform.h"
#include "winapi.h"
#include "vfs.h"
#include "mixer.h"

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
}
#endif

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

EXTERN_C_BEGIN

extern int winapi_debug;

// the SDK's BINK structure (the start of it: what the engine reads), in game memory
typedef struct {
    uint32_t Width;
    uint32_t Height;
    uint32_t Frames;
    uint32_t FrameNum;                  // the current frame, 1-based
    uint32_t LastFrameNum;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t ReadError;
    uint32_t OpenFlags;
    uint32_t BinkType;
    uint32_t Size;
    uint32_t FrameSize;
    uint32_t SndSize;
    uint8_t rest[0x200];                // FrameRects, NumRects, ... (not read by the engine)
    uint32_t movie_lo, movie_hi;        // ours: the host-side state
} bink_struct;

#ifdef HAVE_FFMPEG

typedef struct bink_movie {
    mixer_source source;                // first: the mixer callback gets it
    bink_struct *bink;
    AVFormatContext *fmt;
    AVCodecContext *video;
    int video_stream;
    AVPacket *packet;
    AVFrame *frame;
    uint32_t decoded;                   // the frame number in 'frame' (0: none yet)
    int eof;
    // audio: the whole track, stereo float at its own rate
    float *pcm;
    size_t pcm_frames;
    int pcm_rate;
    double pcm_pos;                     // playback position (source frames); the mixer advances it
    int playing, added;
    uint32_t start_ticks;               // without audio: the clock starts with the first frame
} bink_movie;

static bink_movie *movie_of(bink_struct *b)
{
    if (b == NULL) return NULL;
    return (bink_movie *)(uintptr_t)((uint64_t)b->movie_lo | ((uint64_t)b->movie_hi << 32));
}

// the mixer pulls the audio (linear interpolation to the mixer's rate)
static void movie_mix(mixer_source *source, float *out, int frames)
{
    bink_movie *m = (bink_movie *)source;
    double step = (double)m->pcm_rate / MIXER_RATE;
    int i;

    if (!m->playing) return;
    for (i = 0; i < frames; i++)
    {
        size_t p = (size_t)m->pcm_pos;
        float t = (float)(m->pcm_pos - (double)p);
        if (p + 1 >= m->pcm_frames)
        {
            m->pcm_pos = (double)m->pcm_frames;
            break;
        }
        out[2 * i] += m->pcm[2 * p] + (m->pcm[2 * p + 2] - m->pcm[2 * p]) * t;
        out[2 * i + 1] += m->pcm[2 * p + 1] + (m->pcm[2 * p + 3] - m->pcm[2 * p + 1]) * t;
        m->pcm_pos += step;
    }
}

static void append_audio(bink_movie *m, const AVFrame *f, size_t *capacity)
{
    int channels = f->ch_layout.nb_channels;
    int n = f->nb_samples, i;
    if ((channels < 1) || (n <= 0)) return;
    if (m->pcm_frames + n > *capacity)
    {
        size_t c = (*capacity != 0) ? *capacity * 2 : 65536;
        while (c < m->pcm_frames + n) c *= 2;
        float *p = (float *)realloc(m->pcm, c * 2 * sizeof(float));
        if (p == NULL) return;
        m->pcm = p;
        *capacity = c;
    }
    float *d = m->pcm + 2 * m->pcm_frames;
    for (i = 0; i < n; i++)
    {
        float l, r;
        switch (f->format)
        {
            case AV_SAMPLE_FMT_FLTP:
                l = ((const float *)f->extended_data[0])[i];
                r = (channels > 1) ? ((const float *)f->extended_data[1])[i] : l;
                break;
            case AV_SAMPLE_FMT_FLT:
                l = ((const float *)f->extended_data[0])[i * channels];
                r = (channels > 1) ? ((const float *)f->extended_data[0])[i * channels + 1] : l;
                break;
            case AV_SAMPLE_FMT_S16P:
                l = ((const int16_t *)f->extended_data[0])[i] / 32768.0f;
                r = (channels > 1) ? ((const int16_t *)f->extended_data[1])[i] / 32768.0f : l;
                break;
            case AV_SAMPLE_FMT_S16:
                l = ((const int16_t *)f->extended_data[0])[i * channels] / 32768.0f;
                r = (channels > 1) ? ((const int16_t *)f->extended_data[0])[i * channels + 1] / 32768.0f : l;
                break;
            default:
                l = r = 0.0f;
                break;
        }
        d[2 * i] = l;
        d[2 * i + 1] = r;
    }
    m->pcm_frames += n;
}

// the whole audio track (its own pass over the file)
static void decode_audio(bink_movie *m, const char *path)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *f = NULL;
    const AVCodec *codec;
    size_t capacity = 0;
    int stream;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return;
    if (avformat_find_stream_info(fmt, NULL) < 0) goto done;
    stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (stream < 0) goto done;
    codec = avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id);
    if (codec == NULL) goto done;
    dec = avcodec_alloc_context3(codec);
    if ((dec == NULL) || (avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) < 0) || (avcodec_open2(dec, codec, NULL) < 0)) goto done;
    m->pcm_rate = dec->sample_rate;
    pkt = av_packet_alloc();
    f = av_frame_alloc();
    while (av_read_frame(fmt, pkt) >= 0)
    {
        if (pkt->stream_index == stream && avcodec_send_packet(dec, pkt) >= 0)
        {
            while (avcodec_receive_frame(dec, f) >= 0) append_audio(m, f, &capacity);
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(dec, NULL);
    while (avcodec_receive_frame(dec, f) >= 0) append_audio(m, f, &capacity);
done:
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

static void free_movie(bink_movie *m)
{
    if (m->added) mixer_remove_source(&m->source);
    free(m->pcm);
    av_frame_free(&m->frame);
    av_packet_free(&m->packet);
    avcodec_free_context(&m->video);
    avformat_close_input(&m->fmt);
    free(m);
}

// decode up to frame number 'n' (1-based)
static void decode_to(bink_movie *m, uint32_t n)
{
    while ((m->decoded < n) && !m->eof)
    {
        int r = avcodec_receive_frame(m->video, m->frame);
        if (r >= 0)
        {
            m->decoded++;
            continue;
        }
        if (r != AVERROR(EAGAIN))
        {
            m->eof = 1;
            break;
        }
        // more input
        for (;;)
        {
            if (av_read_frame(m->fmt, m->packet) < 0)
            {
                avcodec_send_packet(m->video, NULL);
                break;
            }
            if (m->packet->stream_index == m->video_stream)
            {
                avcodec_send_packet(m->video, m->packet);
                av_packet_unref(m->packet);
                break;
            }
            av_packet_unref(m->packet);
        }
    }
}

// seconds since the movie started: the audio's position, or the time since the first frame
static double movie_clock(bink_movie *m)
{
    if (m->pcm_frames > 0)
    {
        double pos;
        mixer_lock();
        pos = m->pcm_pos;
        mixer_unlock();
        if (pos < (double)m->pcm_frames) return pos / m->pcm_rate;
        // the audio is over: continue from its end
        if (m->start_ticks == 0) m->start_ticks = SDL_GetTicks() - (uint32_t)(1000.0 * m->pcm_frames / m->pcm_rate);
    }
    return (SDL_GetTicks() - m->start_ticks) / 1000.0;
}

static void start_movie(bink_movie *m)
{
    if (m->playing || (m->start_ticks != 0)) return;
    m->start_ticks = SDL_GetTicks();
    if (m->start_ticks == 0) m->start_ticks = 1;
    if ((m->pcm_frames > 1) && mixer_init())
    {
        m->start_ticks = 0;
        m->source.mix = movie_mix;
        m->playing = 1;
        m->added = 1;
        mixer_add_source(&m->source);
    }
    else m->pcm_frames = 0;
}

#endif

uint32_t CCALL BinkOpen_c(const char *name, uint32_t flags)
{
#ifdef HAVE_FFMPEG
    char path[1024];
    bink_movie *m;
    bink_struct *b;
    AVStream *st;
    const AVCodec *codec;

    if (name == NULL) return 0;
    vfs_resolve(name, path, sizeof(path));
    m = (bink_movie *)calloc(1, sizeof(bink_movie));
    if (m == NULL) return 0;
    if ((avformat_open_input(&m->fmt, path, NULL, NULL) < 0) || (avformat_find_stream_info(m->fmt, NULL) < 0))
    {
        eprintf("BinkOpen: can't open %s\n", path);
        free_movie(m);
        return 0;
    }
    m->video_stream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (m->video_stream < 0)
    {
        free_movie(m);
        return 0;
    }
    st = m->fmt->streams[m->video_stream];
    codec = avcodec_find_decoder(st->codecpar->codec_id);
    m->video = (codec != NULL) ? avcodec_alloc_context3(codec) : NULL;
    if ((m->video == NULL) || (avcodec_parameters_to_context(m->video, st->codecpar) < 0) || (avcodec_open2(m->video, codec, NULL) < 0))
    {
        eprintf("BinkOpen: %s: no decoder\n", path);
        free_movie(m);
        return 0;
    }
    m->packet = av_packet_alloc();
    m->frame = av_frame_alloc();
    decode_audio(m, path);

    b = (bink_struct *)game_calloc(1, sizeof(bink_struct));
    if (b == NULL)
    {
        free_movie(m);
        return 0;
    }
    m->bink = b;
    b->movie_lo = (uint32_t)(uintptr_t)m;
    b->movie_hi = (uint32_t)((uint64_t)(uintptr_t)m >> 32);
    b->Width = (uint32_t)st->codecpar->width;
    b->Height = (uint32_t)st->codecpar->height;
    b->Frames = (st->nb_frames > 0) ? (uint32_t)st->nb_frames : (st->duration > 0) ? (uint32_t)st->duration : 1;
    AVRational rate = (st->avg_frame_rate.num > 0) ? st->avg_frame_rate : st->r_frame_rate;
    if ((rate.num <= 0) || (rate.den <= 0)) { rate.num = 15; rate.den = 1; }
    b->FrameRate = (uint32_t)rate.num;
    b->FrameRateDiv = (uint32_t)rate.den;
    b->FrameNum = 1;
    b->OpenFlags = flags;
    b->Size = (uint32_t)avio_size(m->fmt->pb);
    if (winapi_debug) eprintf("BinkOpen: %s: %ux%u, %u frames at %u/%u, audio %.1f s at %d Hz\n", name, b->Width, b->Height, b->Frames, b->FrameRate, b->FrameRateDiv, m->pcm_rate ? (double)m->pcm_frames / m->pcm_rate : 0.0, m->pcm_rate);
    decode_to(m, 1);
    return H2G(b);
#else
    (void)flags;
    if (winapi_debug) eprintf("BinkOpen: %s: no video support (built without FFmpeg)\n", (name != NULL) ? name : "");
    return 0;
#endif
}

void CCALL BinkClose_c(bink_struct *b)
{
#ifdef HAVE_FFMPEG
    bink_movie *m = movie_of(b);
    if (m == NULL) return;
    free_movie(m);
    b->movie_lo = b->movie_hi = 0;
    game_free(b);
#else
    (void)b;
#endif
}

// nonzero while it's too early for the current frame
uint32_t CCALL BinkWait_c(bink_struct *b)
{
#ifdef HAVE_FFMPEG
    bink_movie *m = movie_of(b);
    if (m == NULL) return 0;
    start_movie(m);
    double due = (double)(b->FrameNum - 1) * b->FrameRateDiv / b->FrameRate;
    return (movie_clock(m) < due) ? 1 : 0;
#else
    (void)b;
    return 0;
#endif
}

uint32_t CCALL BinkDoFrame_c(bink_struct *b)
{
#ifdef HAVE_FFMPEG
    bink_movie *m = movie_of(b);
    if (m == NULL) return 0;
    start_movie(m);
    decode_to(m, b->FrameNum);
#else
    (void)b;
#endif
    return 0;
}

void CCALL BinkNextFrame_c(bink_struct *b)
{
#ifdef HAVE_FFMPEG
    if (movie_of(b) == NULL) return;
    b->LastFrameNum = b->FrameNum;
    if (b->FrameNum < b->Frames) b->FrameNum++;
#else
    (void)b;
#endif
}

// the current frame into a buffer: BINKSURFACE32 (B, G, R, X), 32R (R, G, B, X), 555, 565
uint32_t CCALL BinkCopyToBuffer_c(bink_struct *b, uint8_t *dest, int32_t pitch, uint32_t height, uint32_t x, uint32_t y, uint32_t flags)
{
#ifdef HAVE_FFMPEG
    bink_movie *m = movie_of(b);
    uint32_t type = flags & 15, w, h, i, j;
    int full;
    if ((m == NULL) || (dest == NULL)) return 0;
    decode_to(m, b->FrameNum);
    if (m->decoded == 0) return 0;
    AVFrame *f = m->frame;
    w = (uint32_t)f->width;
    h = (uint32_t)f->height;
    if (h > height) h = height;
    full = (f->color_range == AVCOL_RANGE_JPEG);
    for (j = 0; j < h; j++)
    {
        const uint8_t *py = f->data[0] + j * f->linesize[0];
        const uint8_t *pu = f->data[1] + (j >> 1) * f->linesize[1];
        const uint8_t *pv = f->data[2] + (j >> 1) * f->linesize[2];
        uint8_t *d = dest + (y + j) * pitch;
        for (i = 0; i < w; i++)
        {
            int Y = py[i], U = pu[i >> 1] - 128, V = pv[i >> 1] - 128, r, g, bl;
            if (full)
            {
                r = Y + ((91881 * V) >> 16);
                g = Y - ((22554 * U + 46802 * V) >> 16);
                bl = Y + ((116130 * U) >> 16);
            }
            else
            {
                int c = 76309 * (Y - 16);
                r = (c + 104597 * V) >> 16;
                g = (c - 25675 * U - 53279 * V) >> 16;
                bl = (c + 132201 * U) >> 16;
            }
            r = (r < 0) ? 0 : (r > 255) ? 255 : r;
            g = (g < 0) ? 0 : (g > 255) ? 255 : g;
            bl = (bl < 0) ? 0 : (bl > 255) ? 255 : bl;
            switch (type)
            {
                case 3:     // BINKSURFACE32
                case 5:
                    ((uint32_t *)d)[x + i] = 0xFF000000u | (r << 16) | (g << 8) | bl;
                    break;
                case 4:     // BINKSURFACE32R
                case 6:
                    ((uint32_t *)d)[x + i] = 0xFF000000u | (bl << 16) | (g << 8) | r;
                    break;
                case 9:     // BINKSURFACE555
                    ((uint16_t *)d)[x + i] = (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (bl >> 3));
                    break;
                default:    // BINKSURFACE565
                    ((uint16_t *)d)[x + i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
                    break;
            }
        }
    }
#else
    (void)b; (void)dest; (void)pitch; (void)height; (void)x; (void)y; (void)flags;
#endif
    return 0;
}

// the sound goes to the mixer whatever the engine chooses
uint32_t CCALL BinkSetSoundSystem_c(uint32_t open, uint32_t param) { (void)open; (void)param; return 1; }
uint32_t CCALL BinkOpenMiles_c(uint32_t param) { (void)param; return 0; }
uint32_t CCALL BinkOpenDirectSound_c(uint32_t param) { (void)param; return 0; }
void CCALL BinkSetSoundTrack_c(uint32_t track) { (void)track; }
uint32_t CCALL BinkIsSoftwareCursor_c(uint32_t surface, uint32_t cursor) { (void)surface; (void)cursor; return 0; }
uint32_t CCALL BinkCheckCursor_c(uint32_t wnd, int32_t x, int32_t y, int32_t w, int32_t h) { (void)wnd; (void)x; (void)y; (void)w; (void)h; return 0; }
void CCALL BinkRestoreCursor_c(int32_t count) { (void)count; }

EXTERN_C_END
