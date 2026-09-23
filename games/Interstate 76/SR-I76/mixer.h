/**
 *
 *  Software audio mixer on SDL2 audio (used by DirectSound, CD audio music and Smacker).
 *
 */

#if !defined(_MIXER_H_INCLUDED_)
#define _MIXER_H_INCLUDED_

#ifdef __cplusplus
extern "C" {
#endif

#define MIXER_RATE 44100

// a source adds 'frames' stereo float frames (L,R interleaved) into 'out'; called with the mixer locked
typedef struct mixer_source {
    void (*mix)(struct mixer_source *source, float *out, int frames);
    struct mixer_source *next;
} mixer_source;

int mixer_init(void);           // opens the audio device on first use, returns 0 if audio is unavailable
void mixer_add_source(mixer_source *source);
void mixer_remove_source(mixer_source *source);
void mixer_lock(void);
void mixer_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* _MIXER_H_INCLUDED_ */
