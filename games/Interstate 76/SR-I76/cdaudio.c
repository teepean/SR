/**
 *
 *  CD audio emulation (winmm MCI "cdaudio" device + aux volume) playing the GOG release's
 *  music/<track>.mp3 files, like GOG's win32.dll (a winmm replacement) does.
 *  Track 1 is the data track; audio tracks are 2..17.
 *
 *  The game uses: MCI_SYSINFO, MCI_OPEN (type), MCI_SET (TMSF), MCI_STATUS (mode, number of
 *  tracks, track positions, current position), MCI_PLAY from/to, MCI_STOP, MCI_CLOSE, and
 *  auxGetNumDevs/auxGetDevCapsA/auxSetVolume for the music volume.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "platform.h"
#include "winapi.h"
#include "mixer.h"
#include "vfs.h"

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#define MMSYSERR_BADDEVICEID 2

#define MCI_OPEN 0x0803
#define MCI_CLOSE 0x0804
#define MCI_PLAY 0x0806
#define MCI_SEEK 0x0807
#define MCI_STOP 0x0808
#define MCI_PAUSE 0x0809
#define MCI_SET 0x080D
#define MCI_SYSINFO 0x0810
#define MCI_STATUS 0x0814
#define MCI_RESUME 0x0855

#define MCI_FROM 0x04
#define MCI_TO 0x08
#define MCI_TRACK 0x10
#define MCI_STATUS_ITEM 0x100
#define MCI_SYSINFO_QUANTITY 0x100
#define MCI_SYSINFO_NAME 0x400

#define MCI_STATUS_LENGTH 1
#define MCI_STATUS_POSITION 2
#define MCI_STATUS_NUMBER_OF_TRACKS 3
#define MCI_STATUS_MODE 4
#define MCI_STATUS_MEDIA_PRESENT 5
#define MCI_STATUS_TIME_FORMAT 6
#define MCI_STATUS_READY 7
#define MCI_STATUS_CURRENT_TRACK 8
#define MCI_CDA_STATUS_TYPE_TRACK 0x4001

#define MCI_MODE_STOP 0x20D
#define MCI_MODE_PLAY 0x20E
#define MCI_MODE_PAUSE 0x211
#define MCI_CDA_TRACK_AUDIO 0x440
#define MCI_CDA_TRACK_OTHER 0x441

#define MCIERR_UNRECOGNIZED_COMMAND 261
#define MCIERR_INVALID_DEVICE_ID 257
#define MCIERR_OUTOFRANGE 263
#define MCIERR_UNSUPPORTED_FUNCTION 274

#define FIRST_TRACK 2
#define LAST_TRACK 17
#define DEVICE_ID 1

typedef struct {
    int track;                      // currently loaded track (0 = none)
    uint8_t *file;                  // whole mp3 file
    size_t file_size, file_pos;
    mp3dec_t dec;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int pcm_frames, pcm_pos, pcm_channels, pcm_rate;
    double frac;                    // resampling position between pcm frames
    uint64_t played_frames;         // output frames played in this track
} track_stream;

static int opened;
static volatile int playing, paused;
static int end_track;               // play until the start of this track (exclusive); LAST_TRACK+1 = end of disc
static track_stream stream;
static float volume = 1.0f;
static mixer_source source;
static int source_added;


static int load_track(track_stream *s, int track)
{
    char winpath[64], path[1024];
    FILE *f;
    long size;

    free(s->file);
    memset(s, 0, sizeof(*s));

    snprintf(winpath, sizeof(winpath), "music\\%d.mp3", track);
    if (!vfs_resolve(winpath, path, sizeof(path))) return 0;
    f = fopen(path, "rb");
    if (f == NULL) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    s->file = (uint8_t *)malloc(size);
    if ((s->file == NULL) || (fread(s->file, 1, size, f) != (size_t)size))
    {
        fclose(f);
        free(s->file);
        s->file = NULL;
        return 0;
    }
    fclose(f);
    s->file_size = size;
    s->track = track;
    mp3dec_init(&s->dec);
    return 1;
}

// decodes the next mp3 frame; returns 0 at the end of the file
static int decode_frame(track_stream *s)
{
    mp3dec_frame_info_t info;

    while (s->file_pos < s->file_size)
    {
        int samples = mp3dec_decode_frame(&s->dec, s->file + s->file_pos, (int)(s->file_size - s->file_pos), s->pcm, &info);
        if (info.frame_bytes == 0) break;
        s->file_pos += info.frame_bytes;
        if (samples > 0)
        {
            s->pcm_frames = samples;
            s->pcm_pos = 0;
            s->pcm_channels = info.channels;
            s->pcm_rate = info.hz;
            return 1;
        }
    }
    return 0;
}

static int advance_track(void)
{
    int next = stream.track + 1;
    while (next < end_track && next <= LAST_TRACK)
    {
        if (load_track(&stream, next)) return 1;
        next++;
    }
    return 0;
}

static void music_mix(mixer_source *src, float *out, int frames)
{
    int i;

    if (!playing || paused || (stream.file == NULL)) return;

    for (i = 0; i < frames; i++)
    {
        float l, r;

        while (stream.pcm_pos >= stream.pcm_frames)
        {
            if (!decode_frame(&stream))
            {
                if (!advance_track())
                {
                    playing = 0;
                    return;
                }
            }
        }

        if (stream.pcm_channels == 2)
        {
            l = stream.pcm[2 * stream.pcm_pos] * (1.0f / 32768.0f);
            r = stream.pcm[2 * stream.pcm_pos + 1] * (1.0f / 32768.0f);
        }
        else
        {
            l = r = stream.pcm[stream.pcm_pos] * (1.0f / 32768.0f);
        }
        out[2 * i] += l * volume;
        out[2 * i + 1] += r * volume;
        stream.played_frames++;

        // nearest-neighbour resampling (the files are 44.1 kHz)
        stream.frac += (double)stream.pcm_rate / MIXER_RATE;
        while (stream.frac >= 1.0)
        {
            stream.frac -= 1.0;
            stream.pcm_pos++;
        }
    }
}

static void stop_music(void)
{
    mixer_lock();
    playing = 0;
    paused = 0;
    mixer_unlock();
}

static uint32_t tmsf_track(uint32_t tmsf) { return tmsf & 0xFF; }

static uint32_t current_position(void)
{
    uint32_t seconds;
    if (stream.track == 0) return FIRST_TRACK;
    seconds = (uint32_t)(stream.played_frames / MIXER_RATE);
    return (uint32_t)stream.track | ((seconds / 60) << 8) | ((seconds % 60) << 16);
}

uint32_t CCALL mciSendCommandA_c(uint32_t IDDevice, uint32_t uMsg, uint32_t fdwCommand, uint32_t *dwParam)
{
    if (winapi_debug >= 2) eprintf("mciSendCommandA: dev %u msg 0x%x flags 0x%x\n", IDDevice, uMsg, fdwCommand);

    switch (uMsg)
    {
        case MCI_SYSINFO:
            // MCI_SYSINFO_PARMS: dwCallback, lpstrReturn, dwRetSize, dwNumber, wDeviceType
            if (dwParam == NULL) return MCIERR_OUTOFRANGE;
            if (fdwCommand & MCI_SYSINFO_QUANTITY)
            {
                if (dwParam[1] != 0) *(uint32_t *)dwParam[1] = 1;
                return 0;
            }
            if (fdwCommand & MCI_SYSINFO_NAME)
            {
                if ((dwParam[1] != 0) && (dwParam[2] > 8)) strcpy((char *)dwParam[1], "cdaudio");
                return 0;
            }
            return MCIERR_UNSUPPORTED_FUNCTION;

        case MCI_OPEN:
            // MCI_OPEN_PARMS: dwCallback, wDeviceID, lpstrDeviceType, lpstrElementName, lpstrAlias
            if (!mixer_init())
            {
                if (winapi_debug) eprintf("mciSendCommandA: no audio device\n");
                return MCIERR_INVALID_DEVICE_ID;
            }
            if (!source_added)
            {
                source.mix = music_mix;
                mixer_add_source(&source);
                source_added = 1;
            }
            opened = 1;
            if (dwParam != NULL) dwParam[1] = DEVICE_ID;
            if (winapi_debug) eprintf("mciSendCommandA: cdaudio opened\n");
            return 0;

        case MCI_CLOSE:
            stop_music();
            opened = 0;
            return 0;

        case MCI_SET:
            return 0;

        case MCI_STATUS:
            // MCI_STATUS_PARMS: dwCallback, dwReturn, dwItem, dwTrack
            if (!opened) return MCIERR_INVALID_DEVICE_ID;
            if ((dwParam == NULL) || !(fdwCommand & MCI_STATUS_ITEM)) return MCIERR_UNSUPPORTED_FUNCTION;
            switch (dwParam[2])
            {
                case MCI_STATUS_MODE:
                    dwParam[1] = paused ? MCI_MODE_PAUSE : (playing ? MCI_MODE_PLAY : MCI_MODE_STOP);
                    return 0;
                case MCI_STATUS_NUMBER_OF_TRACKS:
                    dwParam[1] = LAST_TRACK;
                    return 0;
                case MCI_STATUS_POSITION:
                    if (fdwCommand & MCI_TRACK)
                    {
                        if ((dwParam[3] < 1) || (dwParam[3] > LAST_TRACK)) return MCIERR_OUTOFRANGE;
                        dwParam[1] = dwParam[3];    // TMSF: track start
                    }
                    else
                    {
                        dwParam[1] = current_position();
                    }
                    return 0;
                case MCI_STATUS_LENGTH:
                    dwParam[1] = 3 << 0;            // MSF 3:00 (not used by the game)
                    return 0;
                case MCI_STATUS_CURRENT_TRACK:
                    dwParam[1] = (stream.track != 0) ? stream.track : FIRST_TRACK;
                    return 0;
                case MCI_STATUS_MEDIA_PRESENT:
                case MCI_STATUS_READY:
                    dwParam[1] = 1;
                    return 0;
                case MCI_STATUS_TIME_FORMAT:
                    dwParam[1] = 10;                // MCI_FORMAT_TMSF
                    return 0;
                case MCI_CDA_STATUS_TYPE_TRACK:
                    dwParam[1] = (dwParam[3] == 1) ? MCI_CDA_TRACK_OTHER : MCI_CDA_TRACK_AUDIO;
                    return 0;
                default:
                    if (winapi_debug) eprintf("mciSendCommandA: unsupported status item %u\n", dwParam[2]);
                    return MCIERR_UNSUPPORTED_FUNCTION;
            }

        case MCI_PLAY:
        {
            // MCI_PLAY_PARMS: dwCallback, dwFrom, dwTo
            int from, to;
            if (!opened) return MCIERR_INVALID_DEVICE_ID;
            from = ((fdwCommand & MCI_FROM) && (dwParam != NULL)) ? (int)tmsf_track(dwParam[1]) : ((stream.track != 0) ? stream.track : FIRST_TRACK);
            to = ((fdwCommand & MCI_TO) && (dwParam != NULL)) ? (int)tmsf_track(dwParam[2]) : (LAST_TRACK + 1);
            if (to <= from) to = from + 1;
            if (from < FIRST_TRACK) from = FIRST_TRACK;

            stop_music();
            mixer_lock();
            end_track = to;
            stream.track = from - 1;
            if (advance_track())
            {
                playing = 1;
            }
            mixer_unlock();
            if (winapi_debug) eprintf("mciSendCommandA: play tracks %d..%d%s\n", from, to - 1, playing ? "" : " (no music file)");
            return 0;
        }

        case MCI_STOP:
            stop_music();
            return 0;

        case MCI_PAUSE:
            mixer_lock();
            if (playing) paused = 1;
            mixer_unlock();
            return 0;

        case MCI_RESUME:
            mixer_lock();
            paused = 0;
            mixer_unlock();
            return 0;

        case MCI_SEEK:
            stop_music();
            return 0;

        default:
            if (winapi_debug) eprintf("mciSendCommandA: unsupported msg 0x%x\n", uMsg);
            return MCIERR_UNRECOGNIZED_COMMAND;
    }
}


/* aux device 0 = CD audio (controls the music volume) */

uint32_t CCALL auxGetNumDevs_c(void) { return 1; }

uint32_t CCALL auxGetDevCapsA_c(uint32_t uDeviceID, uint8_t *pac, uint32_t cbac)
{
    // AUXCAPSA: wMid, wPid, vDriverVersion, szPname[32], wTechnology, wReserved1, dwSupport
    if (uDeviceID != 0) return MMSYSERR_BADDEVICEID;
    if ((pac == NULL) || (cbac < 48)) return 11; // MMSYSERR_INVALPARAM
    memset(pac, 0, 48);
    strcpy((char *)pac + 8, "CD Audio");
    *(uint16_t *)(pac + 40) = 1;                // AUXCAPS_CDAUDIO
    *(uint32_t *)(pac + 44) = 1 | 2;            // AUXCAPS_VOLUME | AUXCAPS_LRVOLUME
    return 0;
}

uint32_t CCALL auxSetVolume_c(uint32_t uDeviceID, uint32_t dwVolume)
{
    if (uDeviceID != 0) return MMSYSERR_BADDEVICEID;
    volume = (dwVolume & 0xFFFF) / 65535.0f;
    if (winapi_debug) eprintf("auxSetVolume: %.2f\n", volume);
    return 0;
}
