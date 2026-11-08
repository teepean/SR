/**
 *
 *  Music: the engine's DirectMusic manager (CLTDirectMusicMgr, lithtech.exe) replaced. DirectMusic isn't there, so
 *  the engine's own manager fails in Init; music_install points its vtable (the 23 ILTDirectMusicMgr slots the game
 *  calls, SRW-*lithtech/x64/global_aliases.sci: lithtech_DirectMusicMgr_vtbl) at x64/music-asm.asm's thiscall stubs
 *  and these functions, which play the level's music with GothicKit's dmusic (a DirectMusic re-implementation:
 *  segments, styles, bands, DLS instruments) through the mixer.
 *
 *  As the engine: a level's control file (InitLevel: working directory and file, e.g. Music\M1Theme\ M1Control.txt)
 *  defines intensities - segment lists played in order, repeated a number of times (-1: forever), then followed by
 *  another intensity (0: stop) - and the transitions between them (when: immediately / beat / measure / grid /
 *  segment end; optionally a transition segment). The game changes the intensity with the action (ChangeIntensity).
 *  Segment ends come from dmusic's segment-end callback (on the mixer thread), which picks what follows.
 *  Secondary segments and motifs (no AvP2 control file uses them) are not played.
 *
 *  The files come from the resource archives (rezfs.c). Without dmusic (HAVE_DMUSIC) the engine's manager stays.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <SDL.h>
#include "platform.h"
#include "mixer.h"
#include "rezfs.h"
#include "config.h"
#include "music.h"
#include "asmsyms.h"

#ifdef HAVE_DMUSIC
#include <dmusic.h>
#endif

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

extern "C" int winapi_debug;

#define LT_OK 0
#define LT_ERROR 1

// LTDMEnactTypes
enum { ENACT_INVALID = 0, ENACT_DEFAULT, ENACT_IMMEDIATE, ENACT_BEAT, ENACT_MEASURE, ENACT_GRID, ENACT_SEGMENT };

#ifdef HAVE_DMUSIC

namespace {

struct intensity {
    int loops = 0;                      // times to play the list (0 and 1: once; -1: forever)
    int next = 0;                       // the intensity that follows (0: stop)
    std::vector<DmSegment *> segments;
};

struct transition {
    int enact = ENACT_MEASURE;
    DmSegment *segment = NULL;          // a manual transition's segment
};

struct music_state {
    bool initialized = false, level = false;
    DmPerformance *performance = NULL;
    DmLoader *loader = NULL;
    std::string directory;
    std::vector<std::pair<std::string, DmSegment *>> loaded;    // by name (lower case)
    int num_intensities = 0, initial_intensity = 0, initial_volume = 0, volume_offset = 0;
    std::vector<intensity> intensities;                         // [1 .. num]
    std::vector<transition> transitions;                        // [from * (num + 1) + to], from 0 = silence
    // playback (the lock: game thread and mixer thread)
    SDL_mutex *lock = NULL;
    int current = 0;                    // the intensity
    int index = -1;                     // the segment of its list playing (-1: its transition segment)
    int loops_left = 0;
    DmSegment *scheduled = NULL;        // started at a boundary later: keep what plays until then
    DmSegment *after_segment = NULL;    // to start when the playing segment ends (enact "segment")
    bool paused = false, playing = false;
    float gain = 1.0f;
    mixer_source source;
    bool source_added = false;
    std::vector<float> buffer;
};

music_state music;

std::string lower(const std::string &s)
{
    std::string r(s);
    for (char &c : r) c = (char)tolower((unsigned char)c);
    return r;
}

// the loader's files: the level's music directory first, then as given
void *resolve(void *ctx, char const *name, size_t *len)
{
    (void)ctx;
    void *data = rezfs_load((music.directory + name).c_str(), len);
    if (data == NULL) data = rezfs_load(name, len);
    if ((data == NULL) && winapi_debug) eprintf("music: %s not found\n", name);
    return data;
}

void dm_log(void *ctx, DmLogLevel lvl, char const *msg)
{
    (void)ctx;
    if ((lvl <= DmLogLevel_ERROR) || (winapi_debug >= 3)) eprintf("dmusic: %s\n", msg);
}

DmSegment *segment(const std::string &name)
{
    std::string key = lower(name);
    for (auto &l : music.loaded) if (l.first == key) return l.second;
    DmSegment *s = NULL;
    if (DmLoader_getSegment(music.loader, name.c_str(), &s) != DmResult_SUCCESS) s = NULL;
    if ((s == NULL) && winapi_debug) eprintf("music: segment %s not loaded\n", name.c_str());
    music.loaded.push_back({ key, s });
    return s;
}

DmTiming timing(int enact)
{
    switch (enact)
    {
        case ENACT_IMMEDIATE: return DmTiming_INSTANT;
        case ENACT_BEAT: return DmTiming_BEAT;
        case ENACT_GRID: return DmTiming_GRID;
        default: return DmTiming_MEASURE;
    }
}

int string_to_enact(const char *s)
{
    if (s == NULL) return ENACT_INVALID;
    if (!strcasecmp(s, "Invalid")) return ENACT_INVALID;
    if (!strcasecmp(s, "Default")) return ENACT_DEFAULT;
    if (!strcasecmp(s, "Immediatly") || !strcasecmp(s, "Immediately") || !strcasecmp(s, "Immediate")) return ENACT_IMMEDIATE;
    if (!strcasecmp(s, "NextBeat") || !strcasecmp(s, "Beat")) return ENACT_BEAT;
    if (!strcasecmp(s, "NextMeasure") || !strcasecmp(s, "Measure")) return ENACT_MEASURE;
    if (!strcasecmp(s, "NextGrid") || !strcasecmp(s, "Grid")) return ENACT_GRID;
    if (!strcasecmp(s, "NextSegment") || !strcasecmp(s, "Segment")) return ENACT_SEGMENT;
    return ENACT_INVALID;
}

transition &get_transition(int from, int to)
{
    return music.transitions[(size_t)from * (music.num_intensities + 1) + to];
}

// the intensity's first segment: what to start with when changing to it (its transition segment first, if any)
DmSegment *enter_intensity(int from, int to)
{
    transition &t = get_transition(from, to);
    music.current = to;
    music.loops_left = music.intensities[to].loops;
    if (t.segment != NULL)
    {
        music.index = -1;
        return t.segment;
    }
    music.index = 0;
    return music.intensities[to].segments.empty() ? NULL : music.intensities[to].segments[0];
}

// the mixer thread, dmusic locked: what follows the segment that ended (retained for dmusic)
DmSegment *segment_ended(void *ctx, DmSegment *ended)
{
    (void)ctx;
    DmSegment *next = NULL;
    SDL_LockMutex(music.lock);
    if (music.scheduled != NULL && ended != music.scheduled)
    {
        // a segment starts at the next boundary: keep the music going until then
        next = ended;
    }
    else if (music.after_segment != NULL)
    {
        next = music.after_segment;
        music.after_segment = NULL;
        music.scheduled = NULL;
    }
    else if ((music.current > 0) && (music.current <= music.num_intensities))
    {
        music.scheduled = NULL;
        intensity &in = music.intensities[music.current];
        music.index++;
        if (music.index < (int)in.segments.size()) next = in.segments[music.index];
        else if ((music.loops_left < 0) || (music.loops_left > 1))
        {
            if (music.loops_left > 1) music.loops_left--;
            music.index = 0;
            next = in.segments.empty() ? NULL : in.segments[0];
        }
        else if (in.next > 0 && in.next <= music.num_intensities)
        {
            int from = music.current;
            if (in.next == from)
            {
                music.loops_left = in.loops;
                music.index = 0;
                next = in.segments.empty() ? NULL : in.segments[0];
            }
            else next = enter_intensity(from, in.next);
            if (winapi_debug >= 2) eprintf("music: intensity %d -> %d\n", from, music.current);
        }
        else music.current = 0;
    }
    if (next == NULL) music.playing = false;
    SDL_UnlockMutex(music.lock);
    return (next != NULL) ? DmSegment_retain(next) : NULL;
}

void mix(mixer_source *source, float *out, int frames)
{
    (void)source;
    if (music.paused || (music.performance == NULL)) return;
    if ((int)music.buffer.size() < frames * 2) music.buffer.resize((size_t)frames * 2);
    memset(music.buffer.data(), 0, (size_t)frames * 2 * sizeof(float));
    DmPerformance_renderPcm(music.performance, music.buffer.data(), (size_t)frames * 2, (DmRenderOptions)(DmRender_FLOAT | DmRender_STEREO));
    float g = music.gain;
    for (int i = 0; i < frames * 2; i++) out[i] += music.buffer[i] * g;
}

// the control file: "KEY values..." lines, ';' comments; values separated by blanks or commas
std::vector<std::vector<std::string>> parse_control(const char *text)
{
    std::vector<std::vector<std::string>> lines;
    const char *p = text;
    while (*p)
    {
        const char *e = p + strcspn(p, "\r\n");
        std::string line(p, e);
        size_t c = line.find(';');
        if (c != std::string::npos) line.resize(c);
        std::vector<std::string> words;
        std::string w;
        for (char ch : line)
        {
            if (isspace((unsigned char)ch) || ch == ',')
            {
                if (!w.empty()) words.push_back(w);
                w.clear();
            }
            else w += ch;
        }
        if (!w.empty()) words.push_back(w);
        if (!words.empty()) lines.push_back(words);
        p = e;
        while (*p == '\r' || *p == '\n') p++;
    }
    return lines;
}

void stop_now(void)
{
    SDL_LockMutex(music.lock);
    music.current = 0;
    music.index = -1;
    music.scheduled = music.after_segment = NULL;
    music.playing = false;
    SDL_UnlockMutex(music.lock);
    if (music.performance != NULL) DmPerformance_playSegment(music.performance, NULL, DmTiming_INSTANT);
}

uint32_t change_intensity(int n, int enact)
{
    if (!music.level) return LT_ERROR;
    if (n == 0)
    {
        stop_now();
        return LT_ERROR;
    }
    if ((n < 0) || (n > music.num_intensities)) return LT_ERROR;

    SDL_LockMutex(music.lock);
    if (music.current == n)
    {
        SDL_UnlockMutex(music.lock);
        return LT_OK;
    }
    int from = music.current;
    transition &t = get_transition(from, n);
    if (enact == ENACT_INVALID) enact = t.enact;
    DmSegment *s = enter_intensity(from, n);
    if (winapi_debug) eprintf("music: intensity %d -> %d (enact %d)\n", from, n, enact);
    if ((enact == ENACT_SEGMENT) && music.playing)
    {
        music.after_segment = s;
        SDL_UnlockMutex(music.lock);
        return LT_OK;
    }
    bool was_playing = music.playing;
    music.after_segment = NULL;
    music.scheduled = (enact == ENACT_IMMEDIATE || !was_playing) ? NULL : s;
    music.playing = (s != NULL);
    SDL_UnlockMutex(music.lock);
    if (s != NULL) DmPerformance_playSegment(music.performance, s, was_playing ? timing(enact) : DmTiming_INSTANT);
    return LT_OK;
}

// each level has its own performance and loader (the synthesizer keeps the instruments it was given)
void term_level(void)
{
    if (!music.level) return;
    SDL_LockMutex(music.lock);
    music.current = 0;
    music.index = -1;
    music.scheduled = music.after_segment = NULL;
    music.playing = false;
    SDL_UnlockMutex(music.lock);
    mixer_lock();
    DmPerformance *p = music.performance;
    music.performance = NULL;
    mixer_unlock();
    if (p != NULL) DmPerformance_release(p);
    for (auto &l : music.loaded) if (l.second != NULL) DmSegment_release(l.second);
    music.loaded.clear();
    music.intensities.clear();
    music.transitions.clear();
    if (music.loader != NULL) DmLoader_release(music.loader);
    music.loader = NULL;
    music.level = false;
}

}

#endif

EXTERN_C_BEGIN

// music_<method>_asm2c: x64/music-asm.asm; lithtech_DirectMusicMgr_vtbl: the engine's vtable (asmsyms.h)

// the engine's CLTDirectMusicMgr vtable -> ours (it's in the recompiled engine's read-only data)
void music_install(void)
{
#ifdef HAVE_DMUSIC
    static char *const methods[23] = {
        music_Init_asm2c, music_Term_asm2c, music_InitLevel_asm2c, music_TermLevel_asm2c, music_Play_asm2c,
        music_Stop_asm2c, music_Pause_asm2c, music_UnPause_asm2c, music_SetVolume_asm2c, music_ChangeIntensity_asm2c,
        music_PlaySecondary_asm2c, music_StopSecondary_asm2c, music_PlayMotif3_asm2c, music_StopMotif3_asm2c,
        music_GetCurIntensity_asm2c, music_StringToEnactType_asm2c, music_EnactTypeToString_asm2c,
        music_GetNumIntensities_asm2c, music_GetInitialIntensity_asm2c, music_GetInitialVolume_asm2c,
        music_GetVolumeOffset_asm2c, music_PlayMotif2_asm2c, music_StopMotif2_asm2c
    };
    if (getenv("AVP2_NOMUSIC") != NULL || !config_get_int("music", 1)) return;
    uintptr_t start = (uintptr_t)lithtech_DirectMusicMgr_vtbl, end = start + sizeof(methods) / sizeof(methods[0]) * 4;
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect((void *)start, end - start, PAGE_READWRITE, &old)) return;
#else
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t first = start & ~(page - 1), last = (end + page - 1) & ~(page - 1);
    if (mprotect((void *)first, last - first, PROT_READ | PROT_WRITE) != 0) return;
#endif
    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) ((uint32_t *)lithtech_DirectMusicMgr_vtbl)[i] = (uint32_t)(uintptr_t)methods[i];
#ifdef _WIN32
    VirtualProtect((void *)start, end - start, old, &old);
#else
    mprotect((void *)first, last - first, PROT_READ);
#endif
#endif
}

#ifdef HAVE_DMUSIC

uint32_t CCALL music_Init_c(uint32_t self, const uint32_t *args)
{
    (void)self; (void)args;
    if (music.initialized) return LT_ERROR;
    Dm_setLogger((winapi_debug >= 3) ? DmLogLevel_DEBUG : DmLogLevel_ERROR, dm_log, NULL);
    if (music.lock == NULL) music.lock = SDL_CreateMutex();
    music.source.mix = mix;
    if (mixer_init())
    {
        mixer_add_source(&music.source);
        music.source_added = true;
    }
    music.initialized = true;
    if (winapi_debug) eprintf("music: Init\n");
    return LT_OK;
}

uint32_t CCALL music_Term_c(uint32_t self, const uint32_t *args)
{
    (void)self; (void)args;
    if (!music.initialized) return LT_OK;
    term_level();
    if (music.source_added) mixer_remove_source(&music.source);
    music.source_added = false;
    music.initialized = false;
    return LT_OK;
}

// (working directory, control file, three defines)
uint32_t CCALL music_InitLevel_c(uint32_t self, const uint32_t *args)
{
    const char *dir = (const char *)(uintptr_t)args[0], *file = (const char *)(uintptr_t)args[1];
    size_t size;
    (void)self;
    if (!music.initialized || (dir == NULL) || (file == NULL)) return LT_ERROR;
    if (music.level) return LT_ERROR;
    uint32_t t0 = SDL_GetTicks();

    music.directory = dir;
    if (!music.directory.empty() && music.directory.back() != '\\' && music.directory.back() != '/') music.directory += '\\';
    char *text = (char *)rezfs_load((music.directory + file).c_str(), &size);
    if (text == NULL) text = (char *)rezfs_load(file, &size);
    if (text == NULL)
    {
        eprintf("music: control file %s%s not found\n", music.directory.c_str(), file);
        return LT_ERROR;
    }
    auto lines = parse_control(text);
    free(text);

    if (DmLoader_create(&music.loader, (DmLoaderOptions)(DmLoader_DEFAULT | DmLoader_DOWNLOAD)) != DmResult_SUCCESS) return LT_ERROR;
    DmLoader_addResolver(music.loader, resolve, NULL);
    DmPerformance *p = NULL;
    if (DmPerformance_create(&p, MIXER_RATE) != DmResult_SUCCESS)
    {
        DmLoader_release(music.loader);
        music.loader = NULL;
        return LT_ERROR;
    }
    DmPerformance_setSegmentEndCallback(p, segment_ended, NULL);
    mixer_lock();
    music.performance = p;
    mixer_unlock();

    music.num_intensities = 0;
    music.initial_intensity = music.initial_volume = music.volume_offset = 0;
    for (auto &l : lines)
    {
        std::string key = lower(l[0]);
        int v = (l.size() > 1) ? atoi(l[1].c_str()) : 0;
        if (key == "numintensities") music.num_intensities = v;
        else if (key == "initialintensity") music.initial_intensity = v;
        else if (key == "initialvolume") music.initial_volume = v;
        else if (key == "volumeoffset") music.volume_offset = v;
    }
    if ((music.num_intensities <= 0) || (music.num_intensities > 256))
    {
        eprintf("music: %s: no intensities\n", file);
        music.level = true;
        term_level();
        return LT_ERROR;
    }
    music.intensities.assign((size_t)music.num_intensities + 1, intensity());
    music.transitions.assign((size_t)(music.num_intensities + 1) * (music.num_intensities + 1), transition());
    for (auto &l : lines)
    {
        std::string key = lower(l[0]);
        if ((key == "intensity") && (l.size() >= 4))
        {
            int n = atoi(l[1].c_str());
            if ((n < 1) || (n > music.num_intensities)) continue;
            intensity &in = music.intensities[n];
            in.loops = atoi(l[2].c_str());
            in.next = atoi(l[3].c_str());
            for (size_t i = 4; i < l.size(); i++)
            {
                DmSegment *s = segment(l[i]);
                if (s != NULL) in.segments.push_back(s);
            }
        }
        else if ((key == "transition") && (l.size() >= 5))
        {
            int from = atoi(l[1].c_str()), to = atoi(l[2].c_str());
            if ((from < 0) || (from > music.num_intensities) || (to < 1) || (to > music.num_intensities)) continue;
            transition &t = get_transition(from, to);
            int e = string_to_enact(l[3].c_str());
            t.enact = (e == ENACT_INVALID || e == ENACT_DEFAULT) ? ENACT_MEASURE : e;
            if ((lower(l[4]) == "manual") && (l.size() >= 6)) t.segment = segment(l[5]);
        }
    }
    music.level = true;
    if (winapi_debug) eprintf("music: InitLevel %s%s: %d intensities, %u segments (%u ms)\n", music.directory.c_str(), file, music.num_intensities, (unsigned)music.loaded.size(), (unsigned)(SDL_GetTicks() - t0));
    return LT_OK;
}

uint32_t CCALL music_TermLevel_c(uint32_t self, const uint32_t *args)
{
    (void)self; (void)args;
    term_level();
    return LT_OK;
}

uint32_t CCALL music_Play_c(uint32_t self, const uint32_t *args)
{
    (void)self; (void)args;
    if (!music.level) return LT_ERROR;
    // debugging: AVP2_MUSIC_INTENSITY=<n> starts with that intensity
    if (getenv("AVP2_MUSIC_INTENSITY") != NULL) return change_intensity(atoi(getenv("AVP2_MUSIC_INTENSITY")), ENACT_IMMEDIATE);
    return change_intensity(music.initial_intensity, ENACT_INVALID);
}

uint32_t CCALL music_Stop_c(uint32_t self, const uint32_t *args)
{
    (void)self;
    if (!music.level) return LT_ERROR;
    int enact = (int)args[0];
    SDL_LockMutex(music.lock);
    music.current = 0;
    music.index = -1;
    music.scheduled = music.after_segment = NULL;
    music.playing = false;
    SDL_UnlockMutex(music.lock);
    DmPerformance_playSegment(music.performance, NULL, (enact == ENACT_IMMEDIATE || enact == ENACT_INVALID) ? DmTiming_INSTANT : timing(enact));
    return LT_OK;
}

uint32_t CCALL music_Pause_c(uint32_t self, const uint32_t *args)
{
    (void)self; (void)args;
    music.paused = true;
    return LT_OK;
}

uint32_t CCALL music_UnPause_c(uint32_t self, const uint32_t *args)
{
    (void)self; (void)args;
    music.paused = false;
    return LT_OK;
}

// hundredths of a decibel (DirectMusic's master volume), plus the level's VOLUMEOFFSET
uint32_t CCALL music_SetVolume_c(uint32_t self, const uint32_t *args)
{
    (void)self;
    int mb = (int)args[0] + music.volume_offset;
    float g = (mb >= 0) ? 1.0f : powf(10.0f, (float)mb / 2000.0f);
    if (mb <= -9600) g = 0.0f;
    music.gain = g;
    if (winapi_debug >= 2) eprintf("music: SetVolume %d (%.3f)\n", (int)args[0], g);
    return LT_OK;
}

uint32_t CCALL music_ChangeIntensity_c(uint32_t self, const uint32_t *args)
{
    (void)self;
    return change_intensity((int)args[0], (int)args[1]);
}

uint32_t CCALL music_PlaySecondary_c(uint32_t self, const uint32_t *args)
{
    (void)self;
    if (winapi_debug) eprintf("music: PlaySecondary %s (not supported)\n", args[0] ? (const char *)(uintptr_t)args[0] : "");
    return LT_ERROR;
}

uint32_t CCALL music_StopSecondary_c(uint32_t self, const uint32_t *args) { (void)self; (void)args; return LT_OK; }

uint32_t CCALL music_PlayMotif2_c(uint32_t self, const uint32_t *args)
{
    (void)self;
    if (winapi_debug) eprintf("music: PlayMotif %s (not supported)\n", args[0] ? (const char *)(uintptr_t)args[0] : "");
    return LT_ERROR;
}

uint32_t CCALL music_PlayMotif3_c(uint32_t self, const uint32_t *args) { return music_PlayMotif2_c(self, args + 1); }
uint32_t CCALL music_StopMotif2_c(uint32_t self, const uint32_t *args) { (void)self; (void)args; return LT_OK; }
uint32_t CCALL music_StopMotif3_c(uint32_t self, const uint32_t *args) { (void)self; (void)args; return LT_OK; }
uint32_t CCALL music_GetCurIntensity_c(uint32_t self, const uint32_t *args) { (void)self; (void)args; return (uint32_t)music.current; }
uint32_t CCALL music_StringToEnactType_c(uint32_t self, const uint32_t *args) { (void)self; return (uint32_t)string_to_enact((const char *)(uintptr_t)args[0]); }

uint32_t CCALL music_EnactTypeToString_c(uint32_t self, const uint32_t *args)
{
    static const char *const names[7] = { "Invalid", "Default", "Immediate", "Beat", "Measure", "Grid", "Segment" };
    char *out = (char *)(uintptr_t)args[1];
    (void)self;
    if (out != NULL) strcpy(out, (args[0] < 7) ? names[args[0]] : "");
    return 0;
}

uint32_t CCALL music_GetNumIntensities_c(uint32_t self, const uint32_t *args) { (void)self; (void)args; return (uint32_t)music.num_intensities; }
uint32_t CCALL music_GetInitialIntensity_c(uint32_t self, const uint32_t *args) { (void)self; (void)args; return (uint32_t)music.initial_intensity; }
uint32_t CCALL music_GetInitialVolume_c(uint32_t self, const uint32_t *args) { (void)self; (void)args; return (uint32_t)music.initial_volume; }
uint32_t CCALL music_GetVolumeOffset_c(uint32_t self, const uint32_t *args) { (void)self; (void)args; return (uint32_t)music.volume_offset; }

#else

// built without dmusic: the stubs exist (music-asm.asm), music_install leaves the engine's manager
#define NO_MUSIC(name) uint32_t CCALL music_##name##_c(uint32_t self, const uint32_t *args) { (void)self; (void)args; return LT_ERROR; }
NO_MUSIC(Init) NO_MUSIC(Term) NO_MUSIC(InitLevel) NO_MUSIC(TermLevel) NO_MUSIC(Play) NO_MUSIC(Stop) NO_MUSIC(Pause)
NO_MUSIC(UnPause) NO_MUSIC(SetVolume) NO_MUSIC(ChangeIntensity) NO_MUSIC(PlaySecondary) NO_MUSIC(StopSecondary)
NO_MUSIC(PlayMotif3) NO_MUSIC(StopMotif3) NO_MUSIC(GetCurIntensity) NO_MUSIC(StringToEnactType) NO_MUSIC(EnactTypeToString)
NO_MUSIC(GetNumIntensities) NO_MUSIC(GetInitialIntensity) NO_MUSIC(GetInitialVolume) NO_MUSIC(GetVolumeOffset)
NO_MUSIC(PlayMotif2) NO_MUSIC(StopMotif2)

#endif

EXTERN_C_END
