/**
 *
 *  Software audio mixer on SDL2 audio.
 *  Output: 44100 Hz stereo S16; sources mix into a float buffer in the audio thread.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "mixer.h"
#include "config.h"

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

static SDL_AudioDeviceID device;
static int initialized;
static mixer_source *sources;
static float *mixbuf;
static int mixbuf_frames;

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    mixer_source *s;
    int i;

    if (frames > mixbuf_frames)
    {
        // does not happen normally (buffer allocated for the obtained size)
        memset(stream, 0, len);
        return;
    }

    memset(mixbuf, 0, frames * 2 * sizeof(float));
    for (s = sources; s != NULL; s = s->next)
    {
        s->mix(s, mixbuf, frames);
    }

    for (i = 0; i < frames * 2; i++)
    {
        float v = mixbuf[i] * 32767.0f;
        if (v > 32767.0f) v = 32767.0f;
        else if (v < -32768.0f) v = -32768.0f;
        out[i] = (int16_t)v;
    }
}

int mixer_init(void)
{
    SDL_AudioSpec want, have;

    if (initialized) return device != 0;
    initialized = 1;

    if ((getenv("I76_NOSOUND") != NULL) || !config_get_int("sound", 1)) return 0;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        eprintf("mixer: SDL audio init failed: %s\n", SDL_GetError());
        return 0;
    }

    memset(&want, 0, sizeof(want));
    want.freq = MIXER_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;

    device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (device == 0)
    {
        eprintf("mixer: can't open audio device: %s\n", SDL_GetError());
        return 0;
    }

    mixbuf_frames = (have.samples > 4096) ? have.samples : 4096;
    mixbuf = (float *)malloc(mixbuf_frames * 2 * sizeof(float));

    SDL_PauseAudioDevice(device, 0);
    return 1;
}

void mixer_add_source(mixer_source *source)
{
    mixer_lock();
    source->next = sources;
    sources = source;
    mixer_unlock();
}

void mixer_remove_source(mixer_source *source)
{
    mixer_source **p;

    mixer_lock();
    for (p = &sources; *p != NULL; p = &(*p)->next)
    {
        if (*p == source)
        {
            *p = source->next;
            break;
        }
    }
    mixer_unlock();
}

void mixer_lock(void)
{
    if (device != 0) SDL_LockAudioDevice(device);
}

void mixer_unlock(void)
{
    if (device != 0) SDL_UnlockAudioDevice(device);
}
