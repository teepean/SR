// libsrhooks.so - game-side hooks for the SRW64 build of the game (linked as DT_NEEDED, rpath $ORIGIN/sr)
//
// srh_cloth_update: replaces the cloth update job thunk (PS5 sub_AF18D0 -> sub_AF18E0, AnyKyty loc_EF18D0);
// the original body is exported by the SR build as srh_cloth_update_orig (global_aliases.sci).
// Env: SRH_CLOTH_RATE=N - run the cloth update only every Nth call (default 1 = every call)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>

static unsigned int cloth_rate = 1;
static uint64_t cloth_calls, cloth_runs;

// called from the assembler entry with the argument registers saved - no vector registers (-mgeneral-regs-only)
__attribute__((visibility("hidden"), used, target("general-regs-only"))) int srh_cloth_should_run(void)
{
    uint64_t call = __atomic_fetch_add(&cloth_calls, 1, __ATOMIC_RELAXED);
    if (cloth_rate > 1 && (call % cloth_rate) != 0) return 0;
    __atomic_add_fetch(&cloth_runs, 1, __ATOMIC_RELAXED);
    return 1;
}

__asm__(
    "\t.text\n"
    "\t.globl srh_cloth_update\n"
    "\t.type srh_cloth_update, @function\n"
    "srh_cloth_update:\n"
    "\tpush %rdi\n\tpush %rsi\n\tpush %rdx\n\tpush %rcx\n\tpush %r8\n\tpush %r9\n\tpush %rax\n"
    "\tcall srh_cloth_should_run\n"
    "\ttest %eax, %eax\n"
    "\tpop %rax\n\tpop %r9\n\tpop %r8\n\tpop %rcx\n\tpop %rdx\n\tpop %rsi\n\tpop %rdi\n"
    "\tjz 1f\n"
    "\tjmp *srh_cloth_update_orig@GOTPCREL(%rip)\n"
    "1:\txor %eax, %eax\n"
    "\tret\n"
    "\t.size srh_cloth_update, . - srh_cloth_update\n"
);

// ---------------------------------------------------------------------------------------------------------------
// srh_query_record: wraps the per-frame recording of the GPU query object (PS5 sub_BAA760, object ymmword_4A60A80,
// call site PS5 0xF3F0F8 in the frame update sub_F3E2C0; original exported as srh_query_record_orig).
// The game records a compute dispatch on an async command buffer that writes an 84-byte result per active request,
// then a label; 15 reader functions spin (rdtsc, 100 us) until the label is written and read the results.
// SRH_QUERY=1: the readers get the previous frame's results instead (from a cache per request entry), the label
// check is redirected to a private label slot which is already "written" - the main thread no longer waits for
// the async queue. Frames with a request without a fresh cached result keep the original behaviour (wait).
// Object fields (PS5 offsets): +436 label slot, +440 label value, +448 entries (48 bytes: +0 byte active,
// +4 result index, +16 position xyz, +32 float), +456 entry count, +480 results (84 bytes each).

#define AKY 0x400000ULL
#define QUERY_TABLE   (*(volatile uint64_t *volatile *)(0x464AFE8 + AKY))   // label table, 32 bytes per slot
#define QUERY_FREE    (*(volatile uint32_t *)(0x464AFF0 + AKY))            // free list head of the label table
#define MAX_ENTRIES 4096
#define RESULT_SIZE 84

extern void srh_query_record_orig(void *object);

static int query_enabled;
static int32_t private_slot = -1;
static int installed;
static int32_t real_slot;
static uint32_t real_value;
static uint8_t *real_results;
static int32_t last_index[MAX_ENTRIES];        // result index of each entry in the last recorded frame (-1: inactive)
static float last_position[MAX_ENTRIES][3];
static int last_count;
static uint64_t frame_number, cache_frame[MAX_ENTRIES];
static uint8_t cache[MAX_ENTRIES][RESULT_SIZE];
static float cache_position[MAX_ENTRIES][3];
static uint8_t fake[2][MAX_ENTRIES * RESULT_SIZE] __attribute__((aligned(64)));
static uint64_t q_frames, q_installed, q_new_entry, q_too_many, q_no_results, q_waits, q_wait_ns, q_done_early, q_moved;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}


void srh_query_record(uint8_t *object)
{
    int32_t *slot_field = (int32_t *)(object + 436);
    uint32_t *value_field = (uint32_t *)(object + 440);
    uint8_t **results_field = (uint8_t **)(object + 480);
    uint8_t *entries;
    int count, index, all_cached;
    uint8_t *buffer;

    if (!query_enabled)
    {
        srh_query_record_orig(object);
        return;
    }

    // 1. the readers of the last frame are done: real label slot, value and results back
    if (installed)
    {
        *slot_field = real_slot;
        *value_field = real_value;
        *results_field = real_results;
        installed = 0;
    }

    // 2. results of the last frame into the cache (wait for the label - one frame later than the game did)
    if (last_count > 0 && real_results != NULL)
    {
        volatile uint64_t *label = (volatile uint64_t *)((uint8_t *)QUERY_TABLE + 32 * (int64_t)real_slot);
        if (*label != real_value)
        {
            uint64_t start = now_ns();
            q_waits++;
            while (*label != real_value) __builtin_ia32_pause();
            q_wait_ns += now_ns() - start;
        }
        else q_done_early++;
        for (index = 0; index < last_count; index++)
        {
            if (last_index[index] < 0) continue;
            memcpy(cache[index], real_results + (size_t)last_index[index] * RESULT_SIZE, RESULT_SIZE);
            memcpy(cache_position[index], last_position[index], sizeof(cache_position[index]));
            cache_frame[index] = frame_number;
        }
    }
    frame_number++;
    q_frames++;

    // 3. the game records this frame's queries
    srh_query_record_orig(object);

    // 4. mapping of this frame (for the cache refresh in the next frame)
    entries = *(uint8_t **)(object + 448);
    count = *(int32_t *)(object + 456);
    real_slot = *slot_field;
    real_value = *value_field;
    real_results = *results_field;
    last_count = 0;
    if (real_results == NULL || entries == NULL || count <= 0) { q_no_results++; return; }
    if (count > MAX_ENTRIES) { q_too_many++; return; }
    all_cached = 1;
    for (index = 0; index < count; index++)
    {
        const uint8_t *entry = entries + 48 * (size_t)index;
        int32_t result_index = (entry[0] == 1) ? *(const int32_t *)(entry + 4) : -1;
        float position[3];
        memcpy(position, entry + 16, sizeof(position));
        last_index[index] = result_index;
        memcpy(last_position[index], position, sizeof(position));
        if (result_index < 0) continue;
        if (cache_frame[index] != frame_number - 1) { all_cached = 0; continue; }
        {
            float dx = position[0] - cache_position[index][0], dy = position[1] - cache_position[index][1], dz = position[2] - cache_position[index][2];
            if (!(dx * dx + dy * dy + dz * dz < 4.0f)) { all_cached = 0; q_moved++; }     // reused entry or teleport: wait
        }
    }
    last_count = count;
    if (!all_cached) { q_new_entry++; return; }

    // 5. readers get the cached results through a private label slot which is already written
    if (private_slot < 0)
    {
        int32_t slot = (int32_t)QUERY_FREE;
        volatile uint64_t *table = QUERY_TABLE;
        QUERY_FREE = (uint32_t)*(volatile uint64_t *)((volatile uint8_t *)table + 32 * (int64_t)slot);
        private_slot = slot;
        fprintf(stderr, "[srhooks] query: private label slot %d (real slot %d)\n", slot, real_slot);
    }
    buffer = fake[frame_number & 1];
    for (index = 0; index < count; index++)
    {
        if (last_index[index] >= 0) memcpy(buffer + (size_t)last_index[index] * RESULT_SIZE, cache[index], RESULT_SIZE);
    }
    *(volatile uint64_t *)((uint8_t *)QUERY_TABLE + 32 * (int64_t)private_slot) = 1;
    *results_field = buffer;
    *slot_field = private_slot;
    *value_field = 1;
    installed = 1;
    q_installed++;
}

// ---------------------------------------------------------------------------------------------------------------
// srh_particle_finalize: replaces the particle binning/finalize recorder (PS5 sub_1257030(psys, dcb, view context),
// call sites in sub_C1BF60, sub_C26230, sub_13CCC20, sub_159BD90; original exported as srh_particle_finalize_orig).
// The original records 4 phases x N emitters of g1-sized RDNA2 dispatches (shaders da2b, d37c, aad7, c35b) with a
// table per dispatch. SRH_NATIVE contains "particles": one native call instead - a marker
// "#native:particles:<payload>" which AnyKyty turns into a host operation (native_pass["particles"](payload)).
// Payload (game frame allocator, lives for the frame), little endian:
//   +0 u32 'YNP1' (0x31504e59), +4 u32 version 1, +8 u32 count, +12 u32 entry bytes (48),
//   +16 u32 flags (bit 0: phases 3/4 run - an emitter with views >= 2 and a counter block exists), +20..31 0,
//   +32 64 B view data ([*view_context] + 0x4d0), +96 V# A (psys+0x68), +112 V# B (psys+0x98), +128 V# C (psys+0xb0),
//   +144 entries: +0 bins V# (16 B: base|stride qword, views|0x520400000000), +16 u64 counter block (node+0x120, 0 = none),
//   +24 u32 flag (e+164 == 1), +28 u32 flag (e+160 == 0 && e+164 < 2) (= phase 2 table +0), +32 u32 views,
//   +36 u32 phase mask (bit n: phase n+1 runs for this emitter), +40 u64 emitter node (debug)
// Phase n: 1 = R (all entries, views threads), 2 = D (counter block, live count threads), 3 = S (views >= 2),
// 4 = C (views >= 2 and counter block); the game's barriers stay before and after the call.

#define PSYS_EMITTERS 0x4b0
#define NODE_NEXT 0x108
#define NODE_PARAMS 0x30
#define NODE_BLOCK 0x120
#define NODE_BINS 0x138

extern int64_t srh_particle_finalize_orig(uint8_t *psys, uint8_t *cb, uint64_t *view_context);
extern int64_t srh_g_barrier(uint8_t *cb, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e);   // PS5 sub_172E0B0
extern int64_t srh_g_compute_reset(uint8_t *cb, int64_t a);                                         // PS5 sub_AC3250
#define FRAME_ALLOC_POS   (*(volatile uint64_t *)(0x5093608 + AKY))
#define FRAME_ALLOC_END   (*(volatile uint64_t *)(0x5093610 + AKY))

static int native_particles;
static void *(*agc_dcb_push_marker)(void *cb, const char *name, uint32_t color);
static void *(*agc_dcb_pop_marker)(void *cb);
static uint64_t np_calls, np_native, np_fallback, np_emitters;

static void *frame_alloc(uint64_t size)
{
    uint64_t position, start;
    do
    {
        position = FRAME_ALLOC_POS;
        start = (position + 15) & ~15ULL;
    }
    while (!__atomic_compare_exchange_n((uint64_t *)&FRAME_ALLOC_POS, &position, start + size, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    if (start == 0 || start + size > FRAME_ALLOC_END) return NULL;
    return (void *)start;
}

static inline int emitter_views(const uint8_t *params)
{
    const uint8_t *views = *(uint8_t *const *)(params + 0x208);
    return views ? *(const int32_t *)(views + 0x14) : 1;
}

int64_t srh_particle_finalize(uint8_t *psys, uint8_t *cb, uint64_t *view_context)
{
    const uint8_t *node;
    uint8_t *payload, *entry;
    uint32_t count = 0, flags = 0;
    const char *name;
    int pushed = 0;
    char marker[64];

    __atomic_add_fetch(&np_calls, 1, __ATOMIC_RELAXED);
    if (!native_particles || agc_dcb_push_marker == NULL || agc_dcb_pop_marker == NULL)
        return srh_particle_finalize_orig(psys, cb, view_context);

    for (node = *(uint8_t *const *)(psys + PSYS_EMITTERS); node != NULL; node = *(uint8_t *const *)(node + NODE_NEXT))
    {
        const uint8_t *params = *(uint8_t *const *)(node + NODE_PARAMS);
        int views = emitter_views(params);
        int filter = (*(const int32_t *)(params + 0xa0) == 0 && *(const uint32_t *)(params + 0xa4) < 2) || views >= 2;
        if (filter && *(const uint64_t *)(node + NODE_BINS) != 0) count++;
    }
    payload = frame_alloc(144 + 48 * (uint64_t)count);
    if (payload == NULL)
    {
        __atomic_add_fetch(&np_fallback, 1, __ATOMIC_RELAXED);
        return srh_particle_finalize_orig(psys, cb, view_context);
    }

    memset(payload, 0, 144);
    *(uint32_t *)(payload + 0) = 0x31504e59;
    *(uint32_t *)(payload + 4) = 1;
    *(uint32_t *)(payload + 8) = count;
    *(uint32_t *)(payload + 12) = 48;
    memcpy(payload + 32, (const uint8_t *)view_context[0] + 0x4d0, 64);
    memcpy(payload + 96, psys + 0x68, 16);
    memcpy(payload + 112, psys + 0x98, 16);
    memcpy(payload + 128, psys + 0xb0, 16);
    entry = payload + 144;
    for (node = *(uint8_t *const *)(psys + PSYS_EMITTERS); node != NULL; node = *(uint8_t *const *)(node + NODE_NEXT))
    {
        const uint8_t *params = *(uint8_t *const *)(node + NODE_PARAMS);
        int views = emitter_views(params);
        uint32_t e160 = *(const uint32_t *)(params + 0xa0), e164 = *(const uint32_t *)(params + 0xa4);
        int filter = (e160 == 0 && e164 < 2) || views >= 2;
        uint64_t bins = *(const uint64_t *)(node + NODE_BINS), block = *(const uint64_t *)(node + NODE_BLOCK);
        uint32_t mask;
        if (!filter || bins == 0) continue;
        mask = 1 | (block ? 2 : 0) | (views >= 2 ? 4 : 0) | ((views >= 2 && block) ? 8 : 0);
        if (views >= 2 && block) flags |= 1;
        *(uint64_t *)(entry + 0) = (bins & 0xffffffffffffULL) | 0x20000000000000ULL;
        *(uint64_t *)(entry + 8) = (uint32_t)views | 0x520400000000ULL;
        *(uint64_t *)(entry + 16) = block;
        *(uint32_t *)(entry + 24) = (e164 == 1);
        *(uint32_t *)(entry + 28) = (e160 == 0 && e164 < 2);
        *(uint32_t *)(entry + 32) = (uint32_t)views;
        *(uint32_t *)(entry + 36) = mask;
        *(uint64_t *)(entry + 40) = (uint64_t)node;
        entry += 48;
    }
    *(uint32_t *)(payload + 16) = flags;

    // as the original: marker of the pass, barrier, finalize state on the command buffer
    name = *(const char *const *)(psys + 0x20);
    if (name[2] != 0 && *(const int8_t *)(psys + 0x28) <= 1) { agc_dcb_push_marker(cb, name + 2, 0); pushed = 1; }
    srh_g_barrier(cb, 2, 0, 0, 0, 0);
    *(uint32_t *)(cb + 0x19fc) = 1;
    snprintf(marker, sizeof(marker), "#native:particles:%llx", (unsigned long long)(uintptr_t)payload);
    agc_dcb_push_marker(cb, marker, 0);
    agc_dcb_pop_marker(cb);
    srh_g_barrier(cb, 1, 19, 0, 0, 0);
    *(uint32_t *)(cb + 0x19fc) = 0;
    srh_g_compute_reset(cb, 0);
    *(uint8_t *)(cb + 0x1948) = 0;
    if (pushed) agc_dcb_pop_marker(cb);
    __atomic_add_fetch(&np_native, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&np_emitters, count, __ATOMIC_RELAXED);
    return 0;
}

static void *query_reporter(void *argument)
{
    (void)argument;
    for (;;)
    {
        sleep(10);
        if (native_particles)
            fprintf(stderr, "[srhooks] native particles: calls %llu native %llu fallback %llu emitters %llu\n", (unsigned long long)np_calls,
                    (unsigned long long)np_native, (unsigned long long)np_fallback, (unsigned long long)np_emitters);
        if (!query_enabled) continue;
        fprintf(stderr, "[srhooks] query frames %llu installed %llu (new entry %llu moved %llu too many %llu none %llu) "
                "cache refresh waited %llu (%.2f ms avg) done early %llu\n",
                (unsigned long long)q_frames, (unsigned long long)q_installed, (unsigned long long)q_new_entry,
                (unsigned long long)q_moved, (unsigned long long)q_too_many, (unsigned long long)q_no_results,
                (unsigned long long)q_waits, q_waits ? (double)q_wait_ns / q_waits / 1e6 : 0.0, (unsigned long long)q_done_early);
    }
    return NULL;
}

__attribute__((constructor)) static void srhooks_init(void)
{
    const char *env = getenv("SRH_CLOTH_RATE");
    if (env != NULL && atoi(env) > 0) cloth_rate = (unsigned int)atoi(env);
    env = getenv("SRH_QUERY");
    query_enabled = (env != NULL && atoi(env) != 0);
    fprintf(stderr, "[srhooks] loaded: cloth update rate 1/%u, GPU query copy-forward %s\n", cloth_rate, query_enabled ? "on" : "off");
    env = getenv("SRH_NATIVE");
    native_particles = (env != NULL && strstr(env, "particles") != NULL);
    agc_dcb_push_marker = (void *(*)(void *, const char *, uint32_t))dlsym(RTLD_DEFAULT, "+kSrjIVxKFE");   // sceAgcDcbPushMarker
    agc_dcb_pop_marker = (void *(*)(void *))dlsym(RTLD_DEFAULT, "H7uZqCoNuWk");                          // sceAgcDcbPopMarker
    if (native_particles) fprintf(stderr, "[srhooks] native particles on (marker functions %s)\n", (agc_dcb_push_marker && agc_dcb_pop_marker) ? "found" : "MISSING");
    if (query_enabled || native_particles)
    {
        pthread_t thread;
        if (pthread_create(&thread, NULL, query_reporter, NULL) == 0) pthread_detach(thread);
    }
}

__attribute__((destructor)) static void srhooks_fini(void)
{
    fprintf(stderr, "[srhooks] cloth update calls %llu, runs %llu\n", (unsigned long long)cloth_calls, (unsigned long long)cloth_runs);
}
