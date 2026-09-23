/**
 *
 *  DirectSound emulation (IDirectSound, IDirectSoundBuffer, IDirectSound3DListener, IDirectSound3DBuffer)
 *  on the software mixer. The vtables come from com.spec (gen_imports.py -> x86/com-asm.asm).
 *
 *  Buffers are mixed directly from their memory (8/16 bit, mono/stereo, any rate) with linear
 *  interpolation; volume, pan, frequency and a simple DirectSound3D model (rolloff attenuation
 *  and left/right panning relative to the listener) are applied per buffer.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include "platform.h"
#include "winapi.h"
#include "mixer.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#define DS_OK 0
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_NODRIVER 0x88780078u
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_OUTOFMEMORY 0x8007000Eu
#define E_NOINTERFACE 0x80004002u

#define DSBCAPS_PRIMARYBUFFER 0x00000001
#define DSBCAPS_CTRL3D 0x00000010
#define DSBPLAY_LOOPING 0x00000001
#define DSBSTATUS_PLAYING 0x00000001
#define DSBSTATUS_LOOPING 0x00000004
#define DSBLOCK_FROMWRITECURSOR 0x00000001
#define DSBLOCK_ENTIREBUFFER 0x00000002

#define DS3DMODE_NORMAL 0
#define DS3DMODE_HEADRELATIVE 1
#define DS3DMODE_DISABLE 2

// IID Data1 values ({xxxxxxxx-4981-11CE-A521-0020AF0BE560})
#define IID_IDirectSoundBuffer_1 0x279AFA85u
#define IID_IDirectSound3DListener_1 0x279AFA84u
#define IID_IDirectSound3DBuffer_1 0x279AFA86u

extern uint32_t IDirectSoundVtbl_asm2c;
extern uint32_t IDirectSoundBufferVtbl_asm2c;
extern uint32_t IDirectSound3DListenerVtbl_asm2c;
extern uint32_t IDirectSound3DBufferVtbl_asm2c;

#pragma pack(push, 1)
typedef struct {
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;
} wave_format;

typedef struct {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwBufferBytes;
    uint32_t dwReserved;
    wave_format *lpwfxFormat;
} ds_buffer_desc;
#pragma pack(pop)

typedef struct { float x, y, z; } vec3;

typedef struct ds_data {
    int refs;
    uint32_t size;
    uint8_t *mem;
} ds_data;

struct ds_buffer;

typedef struct {
    void *lpVtbl;
    struct ds_buffer *owner;
} ds_3dbuffer;

typedef struct {
    void *lpVtbl;
    struct ds_buffer *owner;    // primary buffer
    vec3 position, velocity, front, top;
    float distance_factor, rolloff_factor, doppler_factor;
} ds_listener;

typedef struct ds_buffer {
    void *lpVtbl;
    uint32_t refs;
    ds_3dbuffer i3d;
    ds_listener listener;

    struct ds_buffer *prev, *next;
    struct ds_device *device;

    uint32_t flags;
    int primary;
    ds_data *data;
    wave_format format;
    uint32_t frames;            // buffer length in sample frames

    uint32_t frequency;
    int32_t volume, pan;
    int playing, looping;
    double position;            // play position in frames

    // 3D
    uint32_t mode;
    vec3 position3d, velocity3d;
    float min_distance, max_distance;

    // gains computed in the game thread (mixer reads them)
    float gain_left, gain_right;
} ds_buffer;

typedef struct ds_device {
    void *lpVtbl;
    uint32_t refs;
    ds_buffer *primary;
    ds_buffer *buffers;
    mixer_source source;
    float master;
} ds_device;

static ds_device *the_device;


/* ------------------------------------------------------------------ */
/* gain calculation                                                    */

static float db_to_gain(int32_t hundredths)
{
    if (hundredths <= -10000) return 0.0f;
    if (hundredths >= 0) return 1.0f;
    return powf(10.0f, hundredths / 2000.0f);
}

static vec3 v_sub(vec3 a, vec3 b) { vec3 r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static float v_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 v_cross(vec3 a, vec3 b) { vec3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; return r; }

static void update_gain(ds_buffer *b)
{
    float gain, left, right;

    if (b->primary) return;

    gain = db_to_gain(b->volume);
    left = (b->pan > 0) ? db_to_gain(-b->pan) : 1.0f;
    right = (b->pan < 0) ? db_to_gain(b->pan) : 1.0f;

    if ((b->flags & DSBCAPS_CTRL3D) && (b->mode != DS3DMODE_DISABLE) && (b->device != NULL) && (b->device->primary != NULL))
    {
        ds_listener *l = &b->device->primary->listener;
        vec3 rel, right_axis;
        float dist, att, x;

        rel = (b->mode == DS3DMODE_HEADRELATIVE) ? b->position3d : v_sub(b->position3d, l->position);
        dist = sqrtf(v_dot(rel, rel));

        att = 1.0f;
        if (dist > b->min_distance)
        {
            float d = (dist < b->max_distance) ? dist : b->max_distance;
            att = b->min_distance / (b->min_distance + l->rolloff_factor * (d - b->min_distance));
        }

        if ((dist > 1e-6f) && (b->mode != DS3DMODE_HEADRELATIVE))
        {
            // DirectSound is left-handed: right = top x front
            right_axis = v_cross(l->top, l->front);
            float len = sqrtf(v_dot(right_axis, right_axis));
            x = (len > 1e-6f) ? v_dot(rel, right_axis) / (len * dist) : 0.0f;
        }
        else if (dist > 1e-6f)
        {
            x = rel.x / dist;
        }
        else
        {
            x = 0.0f;
        }
        if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;

        gain *= att;
        // attenuate the opposite side up to ~-10 dB
        left *= 1.0f - 0.7f * ((x > 0.0f) ? x : 0.0f);
        right *= 1.0f - 0.7f * ((x < 0.0f) ? -x : 0.0f);
    }

    b->gain_left = gain * left;
    b->gain_right = gain * right;
}

static void update_all_gains(ds_device *dev)
{
    ds_buffer *b;
    for (b = dev->buffers; b != NULL; b = b->next) update_gain(b);
}


/* ------------------------------------------------------------------ */
/* mixing (audio thread, mixer locked)                                 */

static inline float sample_at(const ds_buffer *b, uint32_t frame, int channel)
{
    const uint8_t *p = b->data->mem + frame * b->format.nBlockAlign;
    if (b->format.nChannels < 2) channel = 0;
    if (b->format.wBitsPerSample == 8)
    {
        return (p[channel] - 128) * (1.0f / 128.0f);
    }
    else
    {
        return ((const int16_t *)p)[channel] * (1.0f / 32768.0f);
    }
}

static void mix_buffer(ds_buffer *b, float *out, int frames, float master)
{
    double step, pos;
    float gl, gr;
    uint32_t n;
    int i;

    n = b->frames;
    if (n == 0) { b->playing = 0; return; }

    step = (double)b->frequency / MIXER_RATE;
    pos = b->position;
    gl = b->gain_left * master;
    gr = b->gain_right * master;

    for (i = 0; i < frames; i++)
    {
        uint32_t i0 = (uint32_t)pos;
        uint32_t i1 = i0 + 1;
        float frac = (float)(pos - i0);
        float l, r;

        if (i1 >= n) i1 = b->looping ? 0 : i0;

        l = sample_at(b, i0, 0) + (sample_at(b, i1, 0) - sample_at(b, i0, 0)) * frac;
        r = sample_at(b, i0, 1) + (sample_at(b, i1, 1) - sample_at(b, i0, 1)) * frac;
        out[2 * i] += l * gl;
        out[2 * i + 1] += r * gr;

        pos += step;
        if (pos >= n)
        {
            if (b->looping)
            {
                pos = fmod(pos, n);
            }
            else
            {
                b->playing = 0;
                pos = 0;
                break;
            }
        }
    }

    b->position = pos;
}

static void ds_mix(mixer_source *source, float *out, int frames)
{
    ds_device *dev = (ds_device *)((uint8_t *)source - offsetof(ds_device, source));
    ds_buffer *b;
    static int counter;

    if ((winapi_debug >= 3) && (++counter % 43 == 0))
    {
        // about once a second: list the playing buffers
        for (b = dev->buffers; b != NULL; b = b->next)
        {
            if (b->playing && !b->primary)
                eprintf("dsound: %p %s%u bytes %u Hz vol %d pan %d gain %.3f/%.3f 3d mode %u pos %.1f,%.1f,%.1f min %.1f max %.1f\n",
                        b, b->looping ? "loop " : "", b->data->size, b->frequency, b->volume, b->pan, b->gain_left, b->gain_right,
                        b->mode, b->position3d.x, b->position3d.y, b->position3d.z, b->min_distance, b->max_distance);
        }
        if (dev->primary != NULL)
        {
            ds_listener *l = &dev->primary->listener;
            eprintf("dsound: listener %.1f,%.1f,%.1f front %.2f,%.2f,%.2f top %.2f,%.2f,%.2f rolloff %.2f master %.3f\n",
                    l->position.x, l->position.y, l->position.z, l->front.x, l->front.y, l->front.z, l->top.x, l->top.y, l->top.z, l->rolloff_factor, dev->master);
        }
    }

    for (b = dev->buffers; b != NULL; b = b->next)
    {
        if (b->playing && !b->primary && (b->data != NULL)) mix_buffer(b, out, frames, dev->master);
    }
}


/* ------------------------------------------------------------------ */
/* IDirectSound                                                        */

uint32_t CCALL DirectSoundCreate_c(void *lpGuid, ds_device **ppDS, void *pUnkOuter)
{
    ds_device *dev;

    if (ppDS == NULL) return DSERR_INVALIDPARAM;
    *ppDS = NULL;

    if (!mixer_init())
    {
        if (winapi_debug) eprintf("DirectSoundCreate: no audio device\n");
        return DSERR_NODRIVER;
    }

    dev = (ds_device *)calloc(1, sizeof(ds_device));
    if (dev == NULL) return DSERR_OUTOFMEMORY;
    dev->lpVtbl = &IDirectSoundVtbl_asm2c;
    dev->refs = 1;
    dev->master = 1.0f;
    dev->source.mix = ds_mix;
    mixer_add_source(&dev->source);
    the_device = dev;

    if (winapi_debug) eprintf("DirectSoundCreate: %p\n", dev);
    *ppDS = dev;
    return DS_OK;
}

uint32_t CCALL IDirectSound_QueryInterface_c(ds_device *lpThis, uint32_t *riid, void **ppvObj)
{
    if (ppvObj != NULL) *ppvObj = NULL;
    if (winapi_debug) eprintf("IDirectSound::QueryInterface %08x (not supported)\n", (riid != NULL) ? riid[0] : 0);
    return E_NOINTERFACE;
}

uint32_t CCALL IDirectSound_AddRef_c(ds_device *lpThis)
{
    return ++lpThis->refs;
}

static void release_buffer_now(ds_buffer *b);

uint32_t CCALL IDirectSound_Release_c(ds_device *lpThis)
{
    if (lpThis->refs > 1) return --lpThis->refs;

    mixer_remove_source(&lpThis->source);
    // buffers are owned by the device
    while (lpThis->buffers != NULL) release_buffer_now(lpThis->buffers);
    if (the_device == lpThis) the_device = NULL;
    lpThis->lpVtbl = NULL;
    free(lpThis);
    return 0;
}

static void link_buffer(ds_device *dev, ds_buffer *b)
{
    mixer_lock();
    b->device = dev;
    b->prev = NULL;
    b->next = dev->buffers;
    if (b->next != NULL) b->next->prev = b;
    dev->buffers = b;
    mixer_unlock();
}

static ds_buffer *new_buffer(ds_device *dev, uint32_t flags)
{
    ds_buffer *b = (ds_buffer *)calloc(1, sizeof(ds_buffer));
    if (b == NULL) return NULL;
    b->lpVtbl = &IDirectSoundBufferVtbl_asm2c;
    b->refs = 1;
    b->flags = flags;
    b->i3d.lpVtbl = &IDirectSound3DBufferVtbl_asm2c;
    b->i3d.owner = b;
    b->listener.lpVtbl = &IDirectSound3DListenerVtbl_asm2c;
    b->listener.owner = b;
    b->listener.front.z = 1.0f;
    b->listener.top.y = 1.0f;
    b->listener.distance_factor = 1.0f;
    b->listener.rolloff_factor = 1.0f;
    b->listener.doppler_factor = 1.0f;
    b->mode = DS3DMODE_NORMAL;
    b->min_distance = 1.0f;
    b->max_distance = 1000000000.0f;
    return b;
}

uint32_t CCALL IDirectSound_CreateSoundBuffer_c(ds_device *lpThis, const ds_buffer_desc *desc, ds_buffer **ppDSBuffer, void *pUnkOuter)
{
    ds_buffer *b;

    if ((desc == NULL) || (ppDSBuffer == NULL)) return DSERR_INVALIDPARAM;
    *ppDSBuffer = NULL;

    if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER)
    {
        if (lpThis->primary != NULL)
        {
            lpThis->primary->refs++;
            *ppDSBuffer = lpThis->primary;
            return DS_OK;
        }
        b = new_buffer(lpThis, desc->dwFlags);
        if (b == NULL) return DSERR_OUTOFMEMORY;
        b->primary = 1;
        b->format.wFormatTag = 1;
        b->format.nChannels = 2;
        b->format.nSamplesPerSec = 22050;
        b->format.wBitsPerSample = 8;
        b->format.nBlockAlign = 2;
        b->format.nAvgBytesPerSec = 44100;
        lpThis->primary = b;
        link_buffer(lpThis, b);
        if (winapi_debug) eprintf("CreateSoundBuffer: primary flags 0x%x -> %p\n", desc->dwFlags, b);
        *ppDSBuffer = b;
        return DS_OK;
    }

    if ((desc->lpwfxFormat == NULL) || (desc->dwBufferBytes == 0)) return DSERR_INVALIDPARAM;
    if ((desc->lpwfxFormat->wFormatTag != 1) || ((desc->lpwfxFormat->wBitsPerSample != 8) && (desc->lpwfxFormat->wBitsPerSample != 16)))
    {
        eprintf("CreateSoundBuffer: unsupported format %u/%u bits\n", desc->lpwfxFormat->wFormatTag, desc->lpwfxFormat->wBitsPerSample);
        return DSERR_INVALIDPARAM;
    }

    b = new_buffer(lpThis, desc->dwFlags);
    if (b == NULL) return DSERR_OUTOFMEMORY;
    memcpy(&b->format, desc->lpwfxFormat, 16);
    b->format.cbSize = 0;
    if (b->format.nBlockAlign == 0) b->format.nBlockAlign = b->format.nChannels * b->format.wBitsPerSample / 8;

    b->data = (ds_data *)calloc(1, sizeof(ds_data));
    b->data->refs = 1;
    b->data->size = desc->dwBufferBytes;
    b->data->mem = (uint8_t *)malloc(desc->dwBufferBytes);
    memset(b->data->mem, (b->format.wBitsPerSample == 8) ? 0x80 : 0, desc->dwBufferBytes);
    b->frames = desc->dwBufferBytes / b->format.nBlockAlign;
    b->frequency = b->format.nSamplesPerSec;
    update_gain(b);
    link_buffer(lpThis, b);

    if (winapi_debug >= 2) eprintf("CreateSoundBuffer: flags 0x%x, %u bytes, %u Hz %u ch %u bit -> %p\n", desc->dwFlags, desc->dwBufferBytes, b->format.nSamplesPerSec, b->format.nChannels, b->format.wBitsPerSample, b);
    *ppDSBuffer = b;
    return DS_OK;
}

uint32_t CCALL IDirectSound_GetCaps_c(ds_device *lpThis, uint32_t *pDSCaps)
{
    uint32_t size;
    if (pDSCaps == NULL) return DSERR_INVALIDPARAM;
    size = pDSCaps[0];
    if (size < 8 || size > 96) size = 96;
    memset(pDSCaps + 1, 0, size - 4);
    pDSCaps[1] = 0x0F00 | 0x0F;     // DSCAPS_PRIMARY*/SECONDARY* mono/stereo 8/16 bit
    pDSCaps[2] = 100;               // min secondary sample rate
    pDSCaps[3] = 100000;            // max
    pDSCaps[4] = 1;                 // primary buffers
    return DS_OK;
}

uint32_t CCALL IDirectSound_DuplicateSoundBuffer_c(ds_device *lpThis, ds_buffer *orig, ds_buffer **ppDup)
{
    ds_buffer *b;

    if ((orig == NULL) || (ppDup == NULL) || orig->primary || (orig->data == NULL)) return DSERR_INVALIDPARAM;
    *ppDup = NULL;

    b = new_buffer(lpThis, orig->flags);
    if (b == NULL) return DSERR_OUTOFMEMORY;
    b->format = orig->format;
    b->data = orig->data;
    b->data->refs++;
    b->frames = orig->frames;
    b->frequency = orig->frequency;
    b->volume = orig->volume;
    b->pan = orig->pan;
    b->mode = orig->mode;
    b->position3d = orig->position3d;
    b->min_distance = orig->min_distance;
    b->max_distance = orig->max_distance;
    update_gain(b);
    link_buffer(lpThis, b);

    *ppDup = b;
    return DS_OK;
}

uint32_t CCALL IDirectSound_SetCooperativeLevel_c(ds_device *lpThis, void *hwnd, uint32_t dwLevel) { return DS_OK; }
uint32_t CCALL IDirectSound_Compact_c(ds_device *lpThis) { return DS_OK; }
uint32_t CCALL IDirectSound_GetSpeakerConfig_c(ds_device *lpThis, uint32_t *pdwSpeakerConfig) { if (pdwSpeakerConfig) *pdwSpeakerConfig = 4; return DS_OK; } // DSSPEAKER_STEREO
uint32_t CCALL IDirectSound_SetSpeakerConfig_c(ds_device *lpThis, uint32_t dwSpeakerConfig) { return DS_OK; }
uint32_t CCALL IDirectSound_Initialize_c(ds_device *lpThis, void *pcGuidDevice) { return DS_OK; }


/* ------------------------------------------------------------------ */
/* IDirectSoundBuffer                                                  */

uint32_t CCALL IDirectSoundBuffer_QueryInterface_c(ds_buffer *lpThis, uint32_t *riid, void **ppvObj)
{
    if (ppvObj == NULL) return DSERR_INVALIDPARAM;
    *ppvObj = NULL;
    if (riid == NULL) return E_NOINTERFACE;

    if (lpThis->primary && (riid[0] == IID_IDirectSound3DListener_1))
    {
        lpThis->refs++;
        *ppvObj = &lpThis->listener;
        return DS_OK;
    }
    if (!lpThis->primary && (riid[0] == IID_IDirectSound3DBuffer_1) && (lpThis->flags & DSBCAPS_CTRL3D))
    {
        lpThis->refs++;
        *ppvObj = &lpThis->i3d;
        return DS_OK;
    }
    if (riid[0] == IID_IDirectSoundBuffer_1)
    {
        lpThis->refs++;
        *ppvObj = lpThis;
        return DS_OK;
    }
    if (winapi_debug) eprintf("IDirectSoundBuffer::QueryInterface %08x (not supported)\n", riid[0]);
    return E_NOINTERFACE;
}

uint32_t CCALL IDirectSoundBuffer_AddRef_c(ds_buffer *lpThis)
{
    return ++lpThis->refs;
}

static void release_buffer_now(ds_buffer *b)
{
    ds_device *dev = b->device;

    mixer_lock();
    if (b->prev != NULL) b->prev->next = b->next; else if (dev != NULL) dev->buffers = b->next;
    if (b->next != NULL) b->next->prev = b->prev;
    b->playing = 0;
    mixer_unlock();

    if ((dev != NULL) && (dev->primary == b)) dev->primary = NULL;
    if ((b->data != NULL) && (--b->data->refs == 0))
    {
        free(b->data->mem);
        free(b->data);
    }
    b->lpVtbl = NULL;
    free(b);
}

uint32_t CCALL IDirectSoundBuffer_Release_c(ds_buffer *lpThis)
{
    if (lpThis->refs > 1) return --lpThis->refs;
    release_buffer_now(lpThis);
    return 0;
}

uint32_t CCALL IDirectSoundBuffer_GetCaps_c(ds_buffer *lpThis, uint32_t *caps)
{
    if (caps == NULL) return DSERR_INVALIDPARAM;
    caps[1] = lpThis->flags;
    caps[2] = (lpThis->data != NULL) ? lpThis->data->size : 0;
    caps[3] = 0;
    caps[4] = 0;
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_GetCurrentPosition_c(ds_buffer *lpThis, uint32_t *pdwPlay, uint32_t *pdwWrite)
{
    uint32_t play, write;

    if (lpThis->primary || (lpThis->data == NULL))
    {
        play = write = 0;
    }
    else
    {
        mixer_lock();
        play = (uint32_t)lpThis->position * lpThis->format.nBlockAlign;
        mixer_unlock();
        write = play;
        if (lpThis->playing)
        {
            // write cursor ~15 ms ahead of the play cursor
            write += (lpThis->frequency * 15 / 1000) * lpThis->format.nBlockAlign;
            write %= lpThis->data->size;
        }
    }
    if (pdwPlay != NULL) *pdwPlay = play;
    if (pdwWrite != NULL) *pdwWrite = write;
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_GetFormat_c(ds_buffer *lpThis, wave_format *pwfx, uint32_t dwSizeAllocated, uint32_t *pdwSizeWritten)
{
    uint32_t n = sizeof(wave_format);
    if (pwfx != NULL)
    {
        if (dwSizeAllocated < n) n = dwSizeAllocated;
        memcpy(pwfx, &lpThis->format, n);
    }
    if (pdwSizeWritten != NULL) *pdwSizeWritten = n;
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_GetVolume_c(ds_buffer *lpThis, int32_t *plVolume)
{
    if (plVolume == NULL) return DSERR_INVALIDPARAM;
    *plVolume = lpThis->volume;
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_GetPan_c(ds_buffer *lpThis, int32_t *plPan)
{
    if (plPan == NULL) return DSERR_INVALIDPARAM;
    *plPan = lpThis->pan;
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_GetFrequency_c(ds_buffer *lpThis, uint32_t *pdwFrequency)
{
    if (pdwFrequency == NULL) return DSERR_INVALIDPARAM;
    *pdwFrequency = lpThis->primary ? lpThis->format.nSamplesPerSec : lpThis->frequency;
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_GetStatus_c(ds_buffer *lpThis, uint32_t *pdwStatus)
{
    if (pdwStatus == NULL) return DSERR_INVALIDPARAM;
    if (lpThis->primary)
    {
        *pdwStatus = DSBSTATUS_PLAYING | DSBSTATUS_LOOPING;
        return DS_OK;
    }
    mixer_lock();
    *pdwStatus = lpThis->playing ? (DSBSTATUS_PLAYING | (lpThis->looping ? DSBSTATUS_LOOPING : 0)) : 0;
    mixer_unlock();
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_Initialize_c(ds_buffer *lpThis, void *pDirectSound, void *pcDSBufferDesc) { return DSERR_INVALIDCALL; }

uint32_t CCALL IDirectSoundBuffer_Lock_c(ds_buffer *lpThis, uint32_t dwOffset, uint32_t dwBytes, void **ppv1, uint32_t *pb1, void **ppv2, uint32_t *pb2, uint32_t dwFlags)
{
    uint32_t size;

    if (lpThis->primary || (lpThis->data == NULL)) return DSERR_INVALIDCALL;
    size = lpThis->data->size;

    if (dwFlags & DSBLOCK_FROMWRITECURSOR) IDirectSoundBuffer_GetCurrentPosition_c(lpThis, NULL, &dwOffset);
    if (dwFlags & DSBLOCK_ENTIREBUFFER) dwBytes = size;
    if ((dwOffset >= size) || (dwBytes > size)) return DSERR_INVALIDPARAM;

    if (ppv1 == NULL || pb1 == NULL) return DSERR_INVALIDPARAM;
    *ppv1 = lpThis->data->mem + dwOffset;
    if (dwOffset + dwBytes <= size)
    {
        *pb1 = dwBytes;
        if (ppv2 != NULL) *ppv2 = NULL;
        if (pb2 != NULL) *pb2 = 0;
    }
    else
    {
        *pb1 = size - dwOffset;
        if (ppv2 != NULL) *ppv2 = lpThis->data->mem;
        if (pb2 != NULL) *pb2 = dwBytes - (size - dwOffset);
    }
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_Unlock_c(ds_buffer *lpThis, void *p1, uint32_t b1, void *p2, uint32_t b2) { return DS_OK; }

uint32_t CCALL IDirectSoundBuffer_Play_c(ds_buffer *lpThis, uint32_t dwReserved1, uint32_t dwPriority, uint32_t dwFlags)
{
    if (lpThis->primary) return DS_OK;
    if ((getenv("I76_DUMP_SOUND") != NULL) && (lpThis->data != NULL))
    {
        // debugging: save the buffer contents (raw PCM) when it starts playing
        char name[1024];
        FILE *f;
        snprintf(name, sizeof(name), "%s/%p_%u_%uHz_%ubit_%uch.raw", getenv("I76_DUMP_SOUND"), (void *)lpThis, lpThis->data->size,
                 lpThis->format.nSamplesPerSec, lpThis->format.wBitsPerSample, lpThis->format.nChannels);
        f = fopen(name, "wb");
        if (f != NULL) { fwrite(lpThis->data->mem, 1, lpThis->data->size, f); fclose(f); }
    }
    mixer_lock();
    lpThis->looping = (dwFlags & DSBPLAY_LOOPING) ? 1 : 0;
    lpThis->playing = 1;
    mixer_unlock();
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_SetCurrentPosition_c(ds_buffer *lpThis, uint32_t dwNewPosition)
{
    if (lpThis->primary || (lpThis->data == NULL)) return DSERR_INVALIDCALL;
    mixer_lock();
    lpThis->position = (dwNewPosition % lpThis->data->size) / lpThis->format.nBlockAlign;
    mixer_unlock();
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_SetFormat_c(ds_buffer *lpThis, const wave_format *pcfxFormat)
{
    if (!lpThis->primary) return DSERR_INVALIDCALL;
    if (pcfxFormat != NULL) memcpy(&lpThis->format, pcfxFormat, 16);
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_SetVolume_c(ds_buffer *lpThis, int32_t lVolume)
{
    if (lVolume > 0) lVolume = 0;
    if (lVolume < -10000) lVolume = -10000;
    lpThis->volume = lVolume;
    if (lpThis->primary)
    {
        if (lpThis->device != NULL) lpThis->device->master = db_to_gain(lVolume);
    }
    else
    {
        update_gain(lpThis);
    }
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_SetPan_c(ds_buffer *lpThis, int32_t lPan)
{
    if (lPan > 10000) lPan = 10000;
    if (lPan < -10000) lPan = -10000;
    lpThis->pan = lPan;
    update_gain(lpThis);
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_SetFrequency_c(ds_buffer *lpThis, uint32_t dwFrequency)
{
    if (lpThis->primary) return DSERR_INVALIDCALL;
    if (dwFrequency == 0) dwFrequency = lpThis->format.nSamplesPerSec;
    if (dwFrequency < 100) dwFrequency = 100;
    if (dwFrequency > 200000) dwFrequency = 200000;
    lpThis->frequency = dwFrequency;
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_Stop_c(ds_buffer *lpThis)
{
    mixer_lock();
    lpThis->playing = 0;
    mixer_unlock();
    return DS_OK;
}

uint32_t CCALL IDirectSoundBuffer_Restore_c(ds_buffer *lpThis) { return DS_OK; }


/* ------------------------------------------------------------------ */
/* IDirectSound3DListener (on the primary buffer)                      */

#define LISTENER_BUFFER(l) ((l)->owner)

uint32_t CCALL IDirectSound3DListener_QueryInterface_c(ds_listener *lpThis, uint32_t *riid, void **ppvObj) { return IDirectSoundBuffer_QueryInterface_c(LISTENER_BUFFER(lpThis), riid, ppvObj); }
uint32_t CCALL IDirectSound3DListener_AddRef_c(ds_listener *lpThis) { return IDirectSoundBuffer_AddRef_c(LISTENER_BUFFER(lpThis)); }
uint32_t CCALL IDirectSound3DListener_Release_c(ds_listener *lpThis) { return IDirectSoundBuffer_Release_c(LISTENER_BUFFER(lpThis)); }

static void listener_changed(ds_listener *l)
{
    if (LISTENER_BUFFER(l)->device != NULL) update_all_gains(LISTENER_BUFFER(l)->device);
}

uint32_t CCALL IDirectSound3DListener_GetAllParameters_c(ds_listener *lpThis, uint32_t *p)
{
    // DS3DLISTENER: dwSize, vPosition, vVelocity, vOrientFront, vOrientTop, flDistanceFactor, flRolloffFactor, flDopplerFactor
    if (p == NULL) return DSERR_INVALIDPARAM;
    memcpy(p + 1, &lpThis->position, 12);
    memcpy(p + 4, &lpThis->velocity, 12);
    memcpy(p + 7, &lpThis->front, 12);
    memcpy(p + 10, &lpThis->top, 12);
    memcpy(p + 13, &lpThis->distance_factor, 4);
    memcpy(p + 14, &lpThis->rolloff_factor, 4);
    memcpy(p + 15, &lpThis->doppler_factor, 4);
    return DS_OK;
}

uint32_t CCALL IDirectSound3DListener_SetAllParameters_c(ds_listener *lpThis, const uint32_t *p, uint32_t dwApply)
{
    if (p == NULL) return DSERR_INVALIDPARAM;
    memcpy(&lpThis->position, p + 1, 12);
    memcpy(&lpThis->velocity, p + 4, 12);
    memcpy(&lpThis->front, p + 7, 12);
    memcpy(&lpThis->top, p + 10, 12);
    memcpy(&lpThis->distance_factor, p + 13, 4);
    memcpy(&lpThis->rolloff_factor, p + 14, 4);
    memcpy(&lpThis->doppler_factor, p + 15, 4);
    listener_changed(lpThis);
    return DS_OK;
}

uint32_t CCALL IDirectSound3DListener_GetDistanceFactor_c(ds_listener *lpThis, float *v) { if (v) *v = lpThis->distance_factor; return DS_OK; }
uint32_t CCALL IDirectSound3DListener_GetDopplerFactor_c(ds_listener *lpThis, float *v) { if (v) *v = lpThis->doppler_factor; return DS_OK; }
uint32_t CCALL IDirectSound3DListener_GetRolloffFactor_c(ds_listener *lpThis, float *v) { if (v) *v = lpThis->rolloff_factor; return DS_OK; }
uint32_t CCALL IDirectSound3DListener_GetOrientation_c(ds_listener *lpThis, vec3 *front, vec3 *top) { if (front) *front = lpThis->front; if (top) *top = lpThis->top; return DS_OK; }
uint32_t CCALL IDirectSound3DListener_GetPosition_c(ds_listener *lpThis, vec3 *v) { if (v) *v = lpThis->position; return DS_OK; }
uint32_t CCALL IDirectSound3DListener_GetVelocity_c(ds_listener *lpThis, vec3 *v) { if (v) *v = lpThis->velocity; return DS_OK; }

uint32_t CCALL IDirectSound3DListener_SetDistanceFactor_c(ds_listener *lpThis, float v, uint32_t dwApply) { lpThis->distance_factor = v; return DS_OK; }
uint32_t CCALL IDirectSound3DListener_SetDopplerFactor_c(ds_listener *lpThis, float v, uint32_t dwApply) { lpThis->doppler_factor = v; return DS_OK; }
uint32_t CCALL IDirectSound3DListener_SetRolloffFactor_c(ds_listener *lpThis, float v, uint32_t dwApply) { lpThis->rolloff_factor = v; listener_changed(lpThis); return DS_OK; }

uint32_t CCALL IDirectSound3DListener_SetOrientation_c(ds_listener *lpThis, float xf, float yf, float zf, float xt, float yt, float zt, uint32_t dwApply)
{
    lpThis->front.x = xf; lpThis->front.y = yf; lpThis->front.z = zf;
    lpThis->top.x = xt; lpThis->top.y = yt; lpThis->top.z = zt;
    listener_changed(lpThis);
    return DS_OK;
}

uint32_t CCALL IDirectSound3DListener_SetPosition_c(ds_listener *lpThis, float x, float y, float z, uint32_t dwApply)
{
    lpThis->position.x = x; lpThis->position.y = y; lpThis->position.z = z;
    listener_changed(lpThis);
    return DS_OK;
}

uint32_t CCALL IDirectSound3DListener_SetVelocity_c(ds_listener *lpThis, float x, float y, float z, uint32_t dwApply)
{
    lpThis->velocity.x = x; lpThis->velocity.y = y; lpThis->velocity.z = z;
    return DS_OK;
}

uint32_t CCALL IDirectSound3DListener_CommitDeferredSettings_c(ds_listener *lpThis) { listener_changed(lpThis); return DS_OK; }


/* ------------------------------------------------------------------ */
/* IDirectSound3DBuffer                                                */

#define BUF3D(i) ((i)->owner)

uint32_t CCALL IDirectSound3DBuffer_QueryInterface_c(ds_3dbuffer *lpThis, uint32_t *riid, void **ppvObj) { return IDirectSoundBuffer_QueryInterface_c(BUF3D(lpThis), riid, ppvObj); }
uint32_t CCALL IDirectSound3DBuffer_AddRef_c(ds_3dbuffer *lpThis) { return IDirectSoundBuffer_AddRef_c(BUF3D(lpThis)); }
uint32_t CCALL IDirectSound3DBuffer_Release_c(ds_3dbuffer *lpThis) { return IDirectSoundBuffer_Release_c(BUF3D(lpThis)); }

uint32_t CCALL IDirectSound3DBuffer_GetAllParameters_c(ds_3dbuffer *lpThis, uint32_t *p)
{
    // DS3DBUFFER: dwSize, vPosition, vVelocity, dwInsideConeAngle, dwOutsideConeAngle, vConeOrientation,
    //             lConeOutsideVolume, flMinDistance, flMaxDistance, dwMode
    ds_buffer *b = BUF3D(lpThis);
    if (p == NULL) return DSERR_INVALIDPARAM;
    memcpy(p + 1, &b->position3d, 12);
    memcpy(p + 4, &b->velocity3d, 12);
    p[7] = 360; p[8] = 360;
    { const float one = 1.0f; p[9] = 0; p[10] = 0; memcpy(p + 11, &one, 4); }
    p[12] = 0;
    memcpy(p + 13, &b->min_distance, 4);
    memcpy(p + 14, &b->max_distance, 4);
    p[15] = b->mode;
    return DS_OK;
}

uint32_t CCALL IDirectSound3DBuffer_SetAllParameters_c(ds_3dbuffer *lpThis, const uint32_t *p, uint32_t dwApply)
{
    ds_buffer *b = BUF3D(lpThis);
    if (p == NULL) return DSERR_INVALIDPARAM;
    memcpy(&b->position3d, p + 1, 12);
    memcpy(&b->velocity3d, p + 4, 12);
    memcpy(&b->min_distance, p + 13, 4);
    memcpy(&b->max_distance, p + 14, 4);
    b->mode = p[15];
    update_gain(b);
    return DS_OK;
}

uint32_t CCALL IDirectSound3DBuffer_GetConeAngles_c(ds_3dbuffer *lpThis, uint32_t *in, uint32_t *out) { if (in) *in = 360; if (out) *out = 360; return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_GetConeOrientation_c(ds_3dbuffer *lpThis, vec3 *v) { if (v) { v->x = 0; v->y = 0; v->z = 1; } return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_GetConeOutsideVolume_c(ds_3dbuffer *lpThis, int32_t *v) { if (v) *v = 0; return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_GetMaxDistance_c(ds_3dbuffer *lpThis, float *v) { if (v) *v = BUF3D(lpThis)->max_distance; return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_GetMinDistance_c(ds_3dbuffer *lpThis, float *v) { if (v) *v = BUF3D(lpThis)->min_distance; return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_GetMode_c(ds_3dbuffer *lpThis, uint32_t *v) { if (v) *v = BUF3D(lpThis)->mode; return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_GetPosition_c(ds_3dbuffer *lpThis, vec3 *v) { if (v) *v = BUF3D(lpThis)->position3d; return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_GetVelocity_c(ds_3dbuffer *lpThis, vec3 *v) { if (v) *v = BUF3D(lpThis)->velocity3d; return DS_OK; }

uint32_t CCALL IDirectSound3DBuffer_SetConeAngles_c(ds_3dbuffer *lpThis, uint32_t in, uint32_t out, uint32_t dwApply) { return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_SetConeOrientation_c(ds_3dbuffer *lpThis, float x, float y, float z, uint32_t dwApply) { return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_SetConeOutsideVolume_c(ds_3dbuffer *lpThis, int32_t v, uint32_t dwApply) { return DS_OK; }

uint32_t CCALL IDirectSound3DBuffer_SetMaxDistance_c(ds_3dbuffer *lpThis, float v, uint32_t dwApply) { BUF3D(lpThis)->max_distance = v; update_gain(BUF3D(lpThis)); return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_SetMinDistance_c(ds_3dbuffer *lpThis, float v, uint32_t dwApply) { BUF3D(lpThis)->min_distance = (v > 0.0f) ? v : 0.0001f; update_gain(BUF3D(lpThis)); return DS_OK; }
uint32_t CCALL IDirectSound3DBuffer_SetMode_c(ds_3dbuffer *lpThis, uint32_t v, uint32_t dwApply) { BUF3D(lpThis)->mode = v; update_gain(BUF3D(lpThis)); return DS_OK; }

uint32_t CCALL IDirectSound3DBuffer_SetPosition_c(ds_3dbuffer *lpThis, float x, float y, float z, uint32_t dwApply)
{
    ds_buffer *b = BUF3D(lpThis);
    b->position3d.x = x; b->position3d.y = y; b->position3d.z = z;
    update_gain(b);
    return DS_OK;
}

uint32_t CCALL IDirectSound3DBuffer_SetVelocity_c(ds_3dbuffer *lpThis, float x, float y, float z, uint32_t dwApply)
{
    ds_buffer *b = BUF3D(lpThis);
    b->velocity3d.x = x; b->velocity3d.y = y; b->velocity3d.z = z;
    return DS_OK;
}

EXTERN_C_END
