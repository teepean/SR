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
#include <fcntl.h>
#include <sys/mman.h>

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


// SRH_QUERY_TRACE=1: per recorded query, CLOCK_MONOTONIC ns of the record and of the moment its label is visible to
// the CPU ("[srhooks-qtrace]" lines, joined with the emulator's label trace by label address and value). A tracer
// thread polls the labels of the pending records.
#define QT_RING 64
static int query_trace;
static struct { uint64_t addr, value, record_ns, record_end_ns, frame; int count, done; } qt_ring[QT_RING];
static uint64_t qt_head;

static void *query_tracer(void *argument)
{
    uint64_t tail = 0;
    (void)argument;
    for (;;)
    {
        uint64_t head = __atomic_load_n(&qt_head, __ATOMIC_ACQUIRE), k;
        if (head - tail > QT_RING - 2) tail = head - (QT_RING - 2);
        for (k = tail; k < head; k++)
        {
            typeof(&qt_ring[0]) r = &qt_ring[k % QT_RING];
            uint64_t t;
            if (r->done) continue;
            t = now_ns();
            if (*(volatile uint64_t *)r->addr == r->value || t - r->record_ns > 2000000000ULL)
            {
                fprintf(stderr, "[srhooks-qtrace] frame %llu record %llu-%llu %s %llu (+%.3f ms) label %#llx value %#llx entries %d\n",
                        (unsigned long long)r->frame, (unsigned long long)r->record_ns, (unsigned long long)r->record_end_ns,
                        *(volatile uint64_t *)r->addr == r->value ? "written" : "NOT-WRITTEN", (unsigned long long)t,
                        (double)(t - r->record_ns) / 1e6, (unsigned long long)r->addr, (unsigned long long)r->value, r->count);
                r->done = 1;
            }
        }
        while (tail < head && qt_ring[tail % QT_RING].done) tail++;
        for (k = 0; k < 64; k++) __builtin_ia32_pause();
    }
    return NULL;
}

static void query_trace_record(uint8_t *object, uint64_t start)
{
    uint64_t head = qt_head;
    typeof(&qt_ring[0]) r = &qt_ring[head % QT_RING];
    int32_t slot = *(int32_t *)(object + 436);
    if (slot < 0 || QUERY_TABLE == NULL) return;
    r->addr = (uint64_t)((uint8_t *)QUERY_TABLE + 32 * (int64_t)slot);
    r->value = *(uint32_t *)(object + 440);
    r->record_ns = start;
    r->record_end_ns = now_ns();
    r->frame = head;
    r->count = *(int32_t *)(object + 456);
    r->done = 0;
    __atomic_store_n(&qt_head, head + 1, __ATOMIC_RELEASE);
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
        uint64_t start = query_trace ? now_ns() : 0;
        srh_query_record_orig(object);
        if (query_trace) query_trace_record(object, start);
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

// ---------------------------------------------------------------------------------------------------------------
// srh_cloth_dispatch: replaces the dispatch loop of the cloth job (PS5 sub_AF18E0, loop 0xAF5A73..0xAF5C1E on the
// async compute buffer 0x462F240). Called after the game sorted its work items (phase << 61 | record index; record =
// {pipeline, 304-byte user data table}) with the item count; returns 1 when the native call was recorded (the game's
// loop is skipped), 0 to run the original loop. SRH_NATIVE contains "cloth": one marker "#native:cloth:<payload>"
// instead of one g1x1x1 dispatch per instance and a full drain (EOP label + WaitRegMem) between phases.
// Payload (game frame allocator), little endian:
//   +0 u32 'YNP1', +4 u32 version 1, +8 u32 count, +12 u32 entry bytes (24),
//   +16 u64 shader code address per pipeline slot [4], +48 u32 thread group size per slot [4],
//   +64 entries in the game's order (sorted by phase): u64 table (user data s[0:1]), u32 phase, u32 slot,
//       u32 vertex count (table +0x30), u32 0
// Slots: 0 = <= 64 vertices (tg 64), 1 = <= 512 (tg 512), 2 = <= 1152 (tg 512), 3 = more (tg 1024).

#define CLOTH_CB          ((uint8_t *)(0x462F240 + AKY))
#define CLOTH_RECORDS     (*(uint8_t *const volatile *)(0x462F360 + AKY))
#define CLOTH_ITEMS       (*(const uint64_t *const volatile *)(0x462F378 + AKY))
#define CLOTH_STATE_BYTE  (*(volatile uint8_t *)(0x462F2E0 + AKY))
static const uint64_t cloth_pipelines[4] = { 0x462C670 + AKY, 0x462C690 + AKY, 0x462C650 + AKY, 0x462C750 + AKY };

static int native_cloth;
static const char *cloth_dump_dir;
static int cloth_dumped;
static void *(*agc_acb_push_marker)(void *cb, const char *name, uint32_t color);
static void *(*agc_acb_pop_marker)(void *cb);
static uint64_t nc_calls, nc_native, nc_fallback, nc_items;

// SRH_CLOTH_DUMP=<dir>: the shader header (0x100 bytes) and code (up to 64 KB, ends early at the end of the mapping)
// of the 4 cloth pipelines, once, as <dir>/cloth-slot<N>-header.bin and -code.bin (for reverse engineering the
// solver; tools/disgcn.py disassembles the code)
static void dump_guest_bytes(const char *path, const void *data, size_t size)
{
    size_t done = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    while (done < size)
    {
        size_t chunk = 4096 - (((uintptr_t)data + done) & 4095);
        ssize_t written;
        if (chunk > size - done) chunk = size - done;
        written = write(fd, (const uint8_t *)data + done, chunk);   // EFAULT at the end of the mapping
        if (written <= 0) break;
        done += (size_t)written;
    }
    close(fd);
}

static void cloth_dump_shaders(void)
{
    char path[512];
    int slot;
    for (slot = 0; slot < 4; slot++)
    {
        const uint8_t *header = *(uint8_t *const *)(cloth_pipelines[slot] + 0x10);
        if (header == NULL) continue;
        snprintf(path, sizeof(path), "%s/cloth-slot%d-header.bin", cloth_dump_dir, slot);
        dump_guest_bytes(path, header, 0x100);
        snprintf(path, sizeof(path), "%s/cloth-slot%d-code.bin", cloth_dump_dir, slot);
        dump_guest_bytes(path, (const void *)(uintptr_t)*(const uint64_t *)(header + 0x38), 0x10000);
    }
    fprintf(stderr, "[srhooks] cloth shaders dumped to %s\n", cloth_dump_dir);
}

// SRH_CLOTH_CAPTURE=<dir>: when <dir>/capture-now appears (checked every 8th call; or at call SRH_CLOTH_CAPTURE_AT)
// of the cloth dispatch hook, the inputs of every cloth instance of the frame as <dir>/cloth-capture.bin, for an offline test of the
// solver programs (shader-sr/clothbench). The memory is read on the CPU (the emulator downloads GPU-written pages on
// the fault), following the instance table: T (304 B) and every V# in it, the global table *(T+0), the piece data
// *(T+8) and its V#s, the object block *(T+0x10), and through the T.V#(0x18) buffer the collider sets (struct, V#s,
// 168-byte records). File: 'CCAP' u32 version 1, u32 items, u32 ranges, u64 code[4], u32 tg[4],
// items {u64 table, u32 phase, u32 slot, u32 verts, u32 0}, ranges {u64 address, u64 size, bytes}.
#define CAP_MAX_RANGES 65536
static const char *cloth_capture_dir;
static uint64_t cloth_capture_at = ~0ULL, cloth_capture_calls;
static struct { uint64_t lo, hi; } cap_ranges[CAP_MAX_RANGES];
static int cap_count;

// the protection of guest memory from /proc/self/maps: readable, or protected (PROT_NONE: the emulator's GPU-written
// pages, which fault in on a CPU read, or a reservation, which crashes)
static struct { uint64_t lo, hi; int readable; } cap_maps[16384];
static int cap_map_count;

static void cap_read_maps(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    cap_map_count = 0;
    if (f == NULL) return;
    while (fgets(line, sizeof(line), f) != NULL && cap_map_count < 16384)
    {
        unsigned long long lo, hi;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        cap_maps[cap_map_count].lo = lo;
        cap_maps[cap_map_count].hi = hi;
        cap_maps[cap_map_count].readable = perms[0] == 'r';
        cap_map_count++;
    }
    fclose(f);
}

// 1: [address, address + size) is mapped and readable, 2: mapped, partly protected, 0: not entirely mapped
static int cap_access(uint64_t address, uint64_t size)
{
    uint64_t a = address, end = address + size;
    int result = 1, k;
    while (a < end)
    {
        for (k = 0; k < cap_map_count && !(cap_maps[k].lo <= a && a < cap_maps[k].hi); k++);
        if (k == cap_map_count) return 0;
        if (!cap_maps[k].readable) result = 2;
        a = cap_maps[k].hi;
    }
    return result;
}

// ranges: gpu = 1 for buffers the GPU writes (current / previous positions, the global scratch tables): read even
// when protected (the emulator downloads them on the fault); other protected ranges are skipped
static void cap_add(uint64_t address, uint64_t size, int gpu)
{
    int access;
    if (address < 0x10000 || address >= (1ULL << 44) || size == 0 || size > (16ULL << 20)) return;
    access = cap_access(address, size);
    if (access == 0 || (access == 2 && !gpu)) return;
    if (cap_count < CAP_MAX_RANGES)
    {
        cap_ranges[cap_count].lo = address;
        cap_ranges[cap_count].hi = address + size;
        cap_count++;
    }
}

// a buffer descriptor at block + off: base 48 bits, stride 14 bits (word 1 bits 16..29), num_records (word 2):
// records of stride bytes, or bytes when the stride is 0
static void cap_vsharp(const uint8_t *block, uint64_t off, int gpu)
{
    const uint8_t *v = block + off;
    uint32_t w0, w1, w2, w3;
    uint64_t base, stride;
    if (cap_access((uint64_t)(uintptr_t)v, 16) != 1) return;
    w0 = *(const uint32_t *)v; w1 = *(const uint32_t *)(v + 4); w2 = *(const uint32_t *)(v + 8); w3 = *(const uint32_t *)(v + 12);
    base = (uint64_t)w0 | ((uint64_t)(w1 & 0xffff) << 32);
    stride = (w1 >> 16) & 0x3fff;
    if (w3 == 0 || (w3 >> 30) != 0 || (w1 >> 30) != 0) return;     // not a plain buffer descriptor
    cap_add(base, stride ? stride * (uint64_t)w2 : w2, gpu);
}

static int cap_compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void cloth_capture(const uint64_t *items, const uint8_t *records, int32_t count)
{
    char path[512];
    FILE *f, *list;
    int32_t index;
    int k, merged, slot;
    uint32_t header[4] = { 0x50414343, 1, (uint32_t)count, 0 };
    cap_count = 0;
    cap_read_maps();
    for (index = 0; index < count; index++)
    {
        const uint8_t *record = records + 16 * (items[index] & 0xfffff);
        const uint8_t *t = *(uint8_t *const *)(record + 8), *g, *pd, *buffer;
        uint32_t sets, b, off;
        if (t == NULL || cap_access((uint64_t)(uintptr_t)t, 304) != 1) continue;
        cap_add((uint64_t)(uintptr_t)t, 304, 0);
        cap_vsharp(t, 0x18, 1);
        cap_vsharp(t, 0x28, 1);                                     // current positions
        cap_vsharp(t, 0x38, 1);                                     // previous positions
        g = *(uint8_t *const *)t;                                   // global scratch tables G0, G1, ...
        if (g != NULL && cap_access((uint64_t)(uintptr_t)g, 64) == 1)
        {
            cap_add((uint64_t)(uintptr_t)g, 64, 0);
            for (off = 0; off < 64; off += 16) cap_vsharp(g, off, 1);
        }
        pd = *(uint8_t *const *)(t + 8);                            // piece data: header and its V#s
        if (pd != NULL && cap_access((uint64_t)(uintptr_t)pd, 0x120) == 1)
        {
            cap_add((uint64_t)(uintptr_t)pd, 0x120, 0);
            for (off = 0x18; off <= 0x108; off += 16) cap_vsharp(pd, off, 1);
        }
        cap_add(*(const uint64_t *)(t + 0x10), 256, 0);              // object block
        // T.V#(0x18): qwords = the collider set structs (set 0 also holds the bone palette V# at +0x38)
        buffer = (const uint8_t *)(uintptr_t)((uint64_t)*(const uint32_t *)(t + 0x18) |
                                              ((uint64_t)(*(const uint32_t *)(t + 0x1c) & 0xffff) << 32));
        sets = t[0xa4] ? t[0xa4] : 1;
        for (b = 0; b < sets && b < 64; b++)
        {
            const uint8_t *set;
            if (cap_access((uint64_t)(uintptr_t)(buffer + 8 * b), 8) != 1) break;
            set = *(uint8_t *const *)(buffer + 8 * b);
            if (set == NULL || cap_access((uint64_t)(uintptr_t)set, 256) != 1) continue;
            cap_add((uint64_t)(uintptr_t)set, 256, 0);
            cap_vsharp(set, 0x38, 1);                               // bone palette (the cloth writes its bones there)
            cap_vsharp(set, 0x78, 1);                               // second palette (cloth bones, pass 2)
            cap_vsharp(set, 0x88, 1);                               // collider records
            cap_add(*(const uint64_t *)(set + 136) & 0xffffffffffffULL, 168ULL * *(const uint32_t *)(set + 152), 0);
        }
    }
    // merge (sorted by start)
    qsort(cap_ranges, (size_t)cap_count, sizeof(cap_ranges[0]), cap_compare);
    merged = 0;
    for (k = 0; k < cap_count; k++)
    {
        if (merged > 0 && cap_ranges[k].lo <= cap_ranges[merged - 1].hi)
        {
            if (cap_ranges[k].hi > cap_ranges[merged - 1].hi) cap_ranges[merged - 1].hi = cap_ranges[k].hi;
        }
        else cap_ranges[merged++] = cap_ranges[k];
    }
    header[3] = (uint32_t)merged;
    {
        uint64_t total = 0;
        for (k = 0; k < merged; k++) total += cap_ranges[k].hi - cap_ranges[k].lo;
        fprintf(stderr, "[srhooks] cloth capture: %llu bytes in %d ranges\n", (unsigned long long)total, merged);
        if (total > (512ULL << 20)) return;
    }
    // the range list first (a crash while reading leaves it), then the data, flushed per range
    snprintf(path, sizeof(path), "%s/cloth-capture-ranges.txt", cloth_capture_dir);
    list = fopen(path, "w");
    if (list != NULL)
    {
        for (k = 0; k < merged; k++)
            fprintf(list, "%#llx %#llx %d\n", (unsigned long long)cap_ranges[k].lo,
                    (unsigned long long)(cap_ranges[k].hi - cap_ranges[k].lo), cap_access(cap_ranges[k].lo, cap_ranges[k].hi - cap_ranges[k].lo));
        fclose(list);
    }
    snprintf(path, sizeof(path), "%s/cloth-capture.bin", cloth_capture_dir);
    f = fopen(path, "wb");
    if (f == NULL) return;
    fwrite(header, 4, 4, f);
    for (slot = 0; slot < 4; slot++)
    {
        const uint8_t *h = *(uint8_t *const *)(cloth_pipelines[slot] + 0x10);
        uint64_t code = h ? *(const uint64_t *)(h + 0x38) : 0;
        fwrite(&code, 8, 1, f);
    }
    for (slot = 0; slot < 4; slot++)
    {
        const uint8_t *h = *(uint8_t *const *)(cloth_pipelines[slot] + 0x10);
        uint32_t tg = h ? *(const uint32_t *)(h + 0x40) : 0;
        fwrite(&tg, 4, 1, f);
    }
    for (index = 0; index < count; index++)
    {
        uint64_t item = items[index];
        const uint8_t *record = records + 16 * (item & 0xfffff);
        uint64_t pipeline = *(const uint64_t *)record, table = *(const uint64_t *)(record + 8);
        uint32_t e[4] = { (uint32_t)(item >> 61), 4, 0, 0 };
        for (slot = 0; slot < 4 && cloth_pipelines[slot] != pipeline; slot++);
        e[1] = (uint32_t)slot;
        e[2] = table ? *(const uint32_t *)(uintptr_t)(table + 0x30) : 0;
        fwrite(&table, 8, 1, f);
        fwrite(e, 4, 4, f);
    }
    fflush(f);
    for (k = 0; k < merged; k++)
    {
        uint64_t size = cap_ranges[k].hi - cap_ranges[k].lo;
        // copied by a CPU read first: GPU-written pages fault in (the emulator downloads them); write() of the guest
        // pointer itself would get EFAULT from the kernel for a protected page and leave a short range in the file
        uint8_t *copy = (uint8_t *)malloc(size);
        if (copy == NULL) { fprintf(stderr, "[srhooks] cloth capture: out of memory\n"); break; }
        memcpy(copy, (const void *)(uintptr_t)cap_ranges[k].lo, size);
        if (fwrite(&cap_ranges[k].lo, 8, 1, f) != 1 || fwrite(&size, 8, 1, f) != 1 || fwrite(copy, 1, size, f) != size)
        {
            fprintf(stderr, "[srhooks] cloth capture: write failed at range %d\n", k);
            free(copy);
            break;
        }
        free(copy);
    }
    fclose(f);
    fprintf(stderr, "[srhooks] cloth capture: %d instances, %d ranges -> %s\n", count, merged, path);
}

int srh_cloth_dispatch(int32_t count)
{
    const uint64_t *items = CLOTH_ITEMS;
    const uint8_t *records = CLOTH_RECORDS;
    uint8_t *payload;
    char marker[64];
    int32_t index;
    int slot;

    __atomic_add_fetch(&nc_calls, 1, __ATOMIC_RELAXED);
    if (cloth_capture_dir != NULL && count > 0 && items != NULL && records != NULL)
    {
        // at call SRH_CLOTH_CAPTURE_AT, or when <dir>/capture-now exists (renamed to capture-done)
        char trigger[512], done[512];
        uint64_t call = __atomic_add_fetch(&cloth_capture_calls, 1, __ATOMIC_RELAXED);
        snprintf(trigger, sizeof(trigger), "%s/capture-now", cloth_capture_dir);
        snprintf(done, sizeof(done), "%s/capture-done", cloth_capture_dir);
        if (call == cloth_capture_at || (call % 8 == 0 && access(trigger, F_OK) == 0 && rename(trigger, done) == 0))
            cloth_capture(items, records, count);
    }
    if (cloth_dump_dir != NULL && count > 0 && !__atomic_exchange_n(&cloth_dumped, 1, __ATOMIC_RELAXED)) cloth_dump_shaders();
    if (!native_cloth || count <= 0 || items == NULL || records == NULL || agc_acb_push_marker == NULL || agc_acb_pop_marker == NULL) return 0;
    payload = frame_alloc(64 + 24 * (uint64_t)count);
    if (payload == NULL) { __atomic_add_fetch(&nc_fallback, 1, __ATOMIC_RELAXED); return 0; }

    memset(payload, 0, 64);
    *(uint32_t *)(payload + 0) = 0x31504e59;
    *(uint32_t *)(payload + 4) = 1;
    *(uint32_t *)(payload + 8) = (uint32_t)count;
    *(uint32_t *)(payload + 12) = 24;
    for (slot = 0; slot < 4; slot++)
    {
        const uint8_t *header = *(uint8_t *const *)(cloth_pipelines[slot] + 0x10);
        if (header == NULL) continue;
        *(uint64_t *)(payload + 16 + 8 * slot) = *(const uint64_t *)(header + 0x38);
        *(uint32_t *)(payload + 48 + 4 * slot) = *(const uint32_t *)(header + 0x40);
    }
    for (index = 0; index < count; index++)
    {
        uint64_t item = items[index];
        const uint8_t *record = records + 16 * (item & 0xfffff);
        uint64_t pipeline = *(const uint64_t *)record;
        const uint8_t *table = *(uint8_t *const *)(record + 8), *header;
        uint8_t *entry = payload + 64 + 24 * (size_t)index;
        for (slot = 0; slot < 4 && cloth_pipelines[slot] != pipeline; slot++);
        header = (slot < 4) ? *(uint8_t *const *)(pipeline + 0x10) : NULL;
        // unknown pipeline (e.g. the PS5 Pro variants) or one the game wouldn't dispatch: the original loop runs
        if (slot == 4 || table == NULL || header == NULL || (*(const uint32_t *)(header + 0x10) & 1) == 0)
        {
            __atomic_add_fetch(&nc_fallback, 1, __ATOMIC_RELAXED);
            return 0;
        }
        *(uint64_t *)(entry + 0) = (uint64_t)table;
        *(uint32_t *)(entry + 8) = (uint32_t)(item >> 61);
        *(uint32_t *)(entry + 12) = (uint32_t)slot;
        *(uint32_t *)(entry + 16) = *(const uint32_t *)(table + 0x30);
        *(uint32_t *)(entry + 20) = 0;
    }

    snprintf(marker, sizeof(marker), "#native:cloth:%llx", (unsigned long long)(uintptr_t)payload);
    agc_acb_push_marker(CLOTH_CB, marker, 0);
    agc_acb_pop_marker(CLOTH_CB);
    CLOTH_STATE_BYTE = 0;      // as after every item of the original loop
    __atomic_add_fetch(&nc_native, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&nc_items, (uint64_t)count, __ATOMIC_RELAXED);
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// srh_particle_draw_start_stub: replaces `mov eax, [rax + 8]` at PS5 0x1268F37 in the particle draw function
// (sub_1268630): the CPU read of an emitter counter block's prefix-sum start (block + 8, written by the GPU finalize)
// which goes into the draw's user data table (+0x20, rdx). With SRH_NATIVE containing "pdraw" the read is replaced
// by a GPU copy recorded in the command buffer (rbx) before the draw: sceAgcDcbDmaData(dst = table + 0x20 (memory
// through L2), src = block + 8 (memory through L2), 4 bytes); the CPU writes 0 as a placeholder. Without it: the
// original read.
// In the middle of a function: all general registers except rax and all ymm registers are preserved (xmm1 is live).

static int native_pdraw;
static uint64_t pd_calls, pd_native;
// sceAgcDcbDmaData(cb, engine, dst, dst cache policy, dst address, src, src cache policy, src address, bytes,
//                  wait for previous, write confirm, block engine); dst/src 3 = memory through L2
static void *(*agc_dcb_dma_data)(void *cb, uint8_t engine, uint8_t dst, uint8_t dst_cache, uint64_t dst_address, uint8_t src,
                                 uint8_t src_cache, uint64_t src_address, uint32_t bytes, uint8_t wait, uint8_t confirm, uint8_t block);

__attribute__((used)) uint32_t srh_particle_draw_start(uint8_t *cb, uint8_t *table, const uint8_t *block)
{
    __atomic_add_fetch(&pd_calls, 1, __ATOMIC_RELAXED);
    if (!native_pdraw || agc_dcb_dma_data == NULL) return *(const volatile uint32_t *)(block + 8);
    agc_dcb_dma_data(cb, 0, 3, 0, (uint64_t)(uintptr_t)(table + 0x20), 3, 0, (uint64_t)(uintptr_t)(block + 8), 4, 1, 1, 1);
    __atomic_add_fetch(&pd_native, 1, __ATOMIC_RELAXED);
    return 0;
}

__asm__(
    "\t.text\n"
    "\t.globl srh_particle_draw_start_stub\n"
    "\t.type srh_particle_draw_start_stub, @function\n"
    "srh_particle_draw_start_stub:\n"
    "\tpush %rbp\n\tmov %rsp, %rbp\n"
    "\tpush %rcx\n\tpush %rdx\n\tpush %rsi\n\tpush %rdi\n\tpush %r8\n\tpush %r9\n\tpush %r10\n\tpush %r11\n"
    "\tand $-32, %rsp\n\tsub $512, %rsp\n"
    "\tvmovdqu %ymm0, 0(%rsp)\n\tvmovdqu %ymm1, 32(%rsp)\n\tvmovdqu %ymm2, 64(%rsp)\n\tvmovdqu %ymm3, 96(%rsp)\n"
    "\tvmovdqu %ymm4, 128(%rsp)\n\tvmovdqu %ymm5, 160(%rsp)\n\tvmovdqu %ymm6, 192(%rsp)\n\tvmovdqu %ymm7, 224(%rsp)\n"
    "\tvmovdqu %ymm8, 256(%rsp)\n\tvmovdqu %ymm9, 288(%rsp)\n\tvmovdqu %ymm10, 320(%rsp)\n\tvmovdqu %ymm11, 352(%rsp)\n"
    "\tvmovdqu %ymm12, 384(%rsp)\n\tvmovdqu %ymm13, 416(%rsp)\n\tvmovdqu %ymm14, 448(%rsp)\n\tvmovdqu %ymm15, 480(%rsp)\n"
    "\tmov %rbx, %rdi\n"                  // command buffer
    "\tmov %rdx, %rsi\n"                  // user data table
    "\tmov %rax, %rdx\n"                  // counter block
    "\tcall srh_particle_draw_start\n"
    "\tvmovdqu 0(%rsp), %ymm0\n\tvmovdqu 32(%rsp), %ymm1\n\tvmovdqu 64(%rsp), %ymm2\n\tvmovdqu 96(%rsp), %ymm3\n"
    "\tvmovdqu 128(%rsp), %ymm4\n\tvmovdqu 160(%rsp), %ymm5\n\tvmovdqu 192(%rsp), %ymm6\n\tvmovdqu 224(%rsp), %ymm7\n"
    "\tvmovdqu 256(%rsp), %ymm8\n\tvmovdqu 288(%rsp), %ymm9\n\tvmovdqu 320(%rsp), %ymm10\n\tvmovdqu 352(%rsp), %ymm11\n"
    "\tvmovdqu 384(%rsp), %ymm12\n\tvmovdqu 416(%rsp), %ymm13\n\tvmovdqu 448(%rsp), %ymm14\n\tvmovdqu 480(%rsp), %ymm15\n"
    "\tlea -64(%rbp), %rsp\n"
    "\tpop %r11\n\tpop %r10\n\tpop %r9\n\tpop %r8\n\tpop %rdi\n\tpop %rsi\n\tpop %rdx\n\tpop %rcx\n"
    "\tpop %rbp\n"
    "\tret\n"
    "\t.size srh_particle_draw_start_stub, . - srh_particle_draw_start_stub\n"
);

// ---------------------------------------------------------------------------------------------------------------
// srh_spin_pause: the game's rdtsc wait loops (65 loops in 40 functions: job waits, GPU label waits, the ocean query
// readers) spin without a pause instruction. The SR build tests this byte in each loop and executes `pause` when it
// is set (SRH_SPIN_PAUSE=1): less pressure on the other hardware thread of the core and less power while waiting.
uint8_t srh_spin_pause;

// ---------------------------------------------------------------------------------------------------------------
// srh_wt_*: SRH_WAIT_TRACE=1 - when the game's two GPU-bound waits start and end, for lining them up with the
// emulator's traces (CLOCK_MONOTONIC):
//   site 1: the job wait PS5 sub_EA7570 (loc_12A7570) when it waits (*rsi == 0): from the waiting path (0x12A7598)
//           to the done path (0x12A7644);
//   site 2: the Ocean query label wait PS5 sub_BAF3E0 (loc_FAF3E0) when the label is not yet written: from 0xFAF44A to
//           the loop exit (0xFAF4BE jumps to 0xFAF511, the only way there).
//   site 3: the job group wait PS5 sub_EA73F0 (loc_12A73F0: all jobs of a group done, steals jobs meanwhile) when a job
//           is not done: from 0x12A7473 to the two loop exits (0x12A74F5, 0x12A7514 -> 0x12A7450);
//   site 4: a GPU label wait in the frame submit function loc_11E0FE0: label slot [obj+0xCC] >= value [obj+0xD8],
//           from 0x11E3E81 to the loop exit 0x11E3EEB;
//   site 5: the next label wait there: slot [0x4A6115C] >= value [0x4A61158], from 0x11E3F4B to the exit 0x11E3FBB.
//   site 6: the GPU-completion relay thread loc_1637740 (it releases the job of site 1) blocked in sceKernelWaitEqueue
//           (0x1637785 .. 0x1637791);
//   site 7: that thread from the event to the release: label slot [ctx+0x164] >= value [ctx+0x160] (spin if not yet),
//           0x1637791 .. 0x1637840 (the dependency at ctx+0x2D28 is released right after).
// The SR build calls srh_wt_enter<site> / srh_wt_exit<site> there (instruction_replacements.sci); they keep all
// registers and flags. A wait is keyed by the waiting function's frame pointer (waits nest: the job wait runs stolen
// jobs, which can wait again). Each completed wait becomes a record (rdtsc at entry and exit, thread id); a writer
// thread converts the times to CLOCK_MONOTONIC ns and appends "site tid entry_ns exit_ns [label value]" lines (sites 4, 5, 7: the
// label's address and the value waited for) to
// SRH_WAIT_TRACE_FILE (default srh-wait-trace.txt in the current directory) every 100 ms.
__attribute__((used)) static uint8_t wt_on;
#define WT_OPEN 4096                       // open waits (power of two)
#define WT_RECORDS (1u << 18)              // completed waits not yet written (power of two)
static struct { uint64_t key, enter, label, value; } wt_open[WT_OPEN];
static struct { uint64_t seq, enter, exit, label, value; uint32_t tid, site; } wt_records[WT_RECORDS];

// sites 4 / 5 / 7: the label the wait is for (label table *0x4A4AFE8, 32 bytes per slot) and the value it waits for
__attribute__((target("general-regs-only"))) static void wt_label(uint64_t site, uint64_t frame, uint64_t ctx,
                                                                 uint64_t *label, uint64_t *value)
{
    const uint64_t table = *(volatile const uint64_t *)(0x4A4AFE8ULL);
    if (site == 4) {
        const uint8_t *obj = *(const uint8_t *const *)(frame - 0x1890);
        *label = table + 32ull * (uint64_t)(int64_t)*(const int32_t *)(obj + 0xCC);
        *value = *(const uint64_t *)(obj + 0xD8);
    } else if (site == 5) {
        *label = table + 32ull * (uint64_t)(int64_t)*(volatile const int32_t *)(0x4A6115CULL);
        *value = *(volatile const uint32_t *)(0x4A61158ULL);
    } else if (site == 7) {
        *label = table + 32ull * (uint64_t)(int64_t)*(volatile const int32_t *)(ctx + 0x164);
        *value = *(volatile const uint32_t *)(ctx + 0x160);
    }
}
static uint64_t wt_head, wt_table_full;

__attribute__((visibility("hidden"), used, target("general-regs-only"))) void srh_wt_event(uint64_t site, uint64_t frame,
                                                                                            uint64_t exit, uint64_t ctx)
{
    uint64_t now = __builtin_ia32_rdtsc(), key = frame | site, k = (frame >> 4) * 0x9E3779B97F4A7C15ull >> 52, n;
    if (!exit)
    {
        for (n = 0; n < WT_OPEN; n++, k = (k + 1) & (WT_OPEN - 1))
        {
            uint64_t expected = 0;
            if (__atomic_compare_exchange_n(&wt_open[k].key, &expected, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            {
                uint64_t label = 0, value = 0;
                if (site == 4 || site == 5 || site == 7) wt_label(site, frame, ctx, &label, &value);
                wt_open[k].label = label;
                wt_open[k].value = value;
                __atomic_store_n(&wt_open[k].enter, now, __ATOMIC_RELEASE);
                return;
            }
        }
        __atomic_add_fetch(&wt_table_full, 1, __ATOMIC_RELAXED);
        return;
    }
    for (n = 0; n < WT_OPEN; n++, k = (k + 1) & (WT_OPEN - 1))
    {
        if (__atomic_load_n(&wt_open[k].key, __ATOMIC_ACQUIRE) != key) continue;
        uint64_t enter = __atomic_load_n(&wt_open[k].enter, __ATOMIC_ACQUIRE), index, tid;
        uint64_t label = wt_open[k].label, value = wt_open[k].value;
        __atomic_store_n(&wt_open[k].key, 0, __ATOMIC_RELEASE);
        __asm__ volatile("syscall" : "=a"(tid) : "a"(186ull) : "rcx", "r11", "memory");   // gettid
        index = __atomic_fetch_add(&wt_head, 1, __ATOMIC_RELAXED);
        n = index & (WT_RECORDS - 1);
        __atomic_store_n(&wt_records[n].seq, 0, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        wt_records[n].enter = enter;
        wt_records[n].exit = now;
        wt_records[n].tid = (uint32_t)tid;
        wt_records[n].site = (uint32_t)site;
        wt_records[n].label = label;
        wt_records[n].value = value;
        __atomic_store_n(&wt_records[n].seq, index + 1, __ATOMIC_RELEASE);
        return;
    }
}

// entry points: flags and all registers kept; only the general registers are used by srh_wt_event
#define WT_ENTRY(name, site, exit) \
    "\t.globl " name "\n\t.type " name ", @function\n" name ":\n" \
    "\tpushfq\n\tcmpb $0, wt_on(%rip)\n\tjne 1f\n\tpopfq\n\tret\n" \
    "1:\tpush %rdi\n\tpush %rsi\n\tpush %rdx\n\tmov $" #site ", %edi\n\tmov %rbp, %rsi\n\tmov $" #exit ", %edx\n" \
    "\tjmp srh_wt_common\n\t.size " name ", . - " name "\n"
__asm__(
    "\t.text\n"
    WT_ENTRY("srh_wt_enter1", 1, 0)
    WT_ENTRY("srh_wt_exit1", 1, 1)
    WT_ENTRY("srh_wt_enter2", 2, 0)
    WT_ENTRY("srh_wt_exit2", 2, 1)
    WT_ENTRY("srh_wt_enter3", 3, 0)
    WT_ENTRY("srh_wt_exit3", 3, 1)
    WT_ENTRY("srh_wt_enter4", 4, 0)
    WT_ENTRY("srh_wt_exit4", 4, 1)
    WT_ENTRY("srh_wt_enter5", 5, 0)
    WT_ENTRY("srh_wt_exit5", 5, 1)
    WT_ENTRY("srh_wt_enter6", 6, 0)
    WT_ENTRY("srh_wt_exit6", 6, 1)
    WT_ENTRY("srh_wt_enter7", 7, 0)
    WT_ENTRY("srh_wt_exit7", 7, 1)
    "srh_wt_common:\n"
    "\tpush %rbp\n\tmov %rsp, %rbp\n"
    "\tpush %rax\n\tpush %rcx\n\tpush %r8\n\tpush %r9\n\tpush %r10\n\tpush %r11\n"
    "\tand $-16, %rsp\n"
    "\tmov %rbx, %rcx\n"                  // the waiting function's rbx (site 7: the relay context)
    "\tcall srh_wt_event\n"
    "\tlea -48(%rbp), %rsp\n"
    "\tpop %r11\n\tpop %r10\n\tpop %r9\n\tpop %r8\n\tpop %rcx\n\tpop %rax\n"
    "\tpop %rbp\n\tpop %rdx\n\tpop %rsi\n\tpop %rdi\n\tpopfq\n"
    "\tret\n"
);

static uint64_t wt_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *wt_writer(void *argument)
{
    const char *path = (const char *)argument;
    FILE *file = fopen(path, "w");
    uint64_t tail = 0, tsc0, ns0, lost = 0, stalled = 0, reported_lost = 0;
    if (file == NULL)
    {
        fprintf(stderr, "[srhooks] wait trace: cannot open %s\n", path);
        return NULL;
    }
    tsc0 = __builtin_ia32_rdtsc();
    ns0 = wt_ns();
    fprintf(file, "# srhooks wait trace: site (1 job wait loc_12A7570, 2 Ocean label wait loc_FAF3E0, 3 job group wait loc_12A73F0, 4/5 label waits in loc_11E0FE0, 6/7 relay thread loc_1637740: event wait / label to release) tid entry_ns exit_ns"
            " (CLOCK_MONOTONIC)\n");
    for (;;)
    {
        usleep(100000);
        uint64_t tsc1 = __builtin_ia32_rdtsc(), ns1 = wt_ns(), head = __atomic_load_n(&wt_head, __ATOMIC_ACQUIRE);
        long double scale = (long double)(ns1 - ns0) / (long double)(tsc1 - tsc0);
        while (tail < head)
        {
            uint64_t n = tail & (WT_RECORDS - 1), seq = __atomic_load_n(&wt_records[n].seq, __ATOMIC_ACQUIRE);
            if (seq < tail + 1)                // not written yet: wait a little, then give it up
            {
                if (++stalled < 5) break;
                lost++, tail++, stalled = 0;
                continue;
            }
            stalled = 0;
            if (seq > tail + 1)                // overwritten by a newer wait (the writer fell behind)
            {
                lost++, tail++;
                continue;
            }
            uint64_t enter = wt_records[n].enter, exit = wt_records[n].exit;
            uint32_t tid = wt_records[n].tid, site = wt_records[n].site;
            uint64_t label = wt_records[n].label, value = wt_records[n].value;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&wt_records[n].seq, __ATOMIC_RELAXED) != seq)
            {
                lost++, tail++;
                continue;
            }
            fprintf(file, "%u %u %llu %llu", site, tid,
                    (unsigned long long)(ns0 + (int64_t)((long double)(int64_t)(enter - tsc0) * scale)),
                    (unsigned long long)(ns0 + (int64_t)((long double)(int64_t)(exit - tsc0) * scale)));
            if (label != 0) fprintf(file, " %#llx %llu", (unsigned long long)label, (unsigned long long)value);
            fputc('\n', file);
            tail++;
        }
        fflush(file);
        if (lost != reported_lost || wt_table_full != 0)
        {
            fprintf(stderr, "[srhooks] wait trace: %llu waits lost, %llu not started (table full)\n", (unsigned long long)lost,
                    (unsigned long long)wt_table_full);
            reported_lost = lost;
        }
    }
    return NULL;
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
        if (native_pdraw)
            fprintf(stderr, "[srhooks] native particle draw start: calls %llu native %llu\n", (unsigned long long)pd_calls, (unsigned long long)pd_native);
        if (native_cloth)
            fprintf(stderr, "[srhooks] native cloth: calls %llu native %llu fallback %llu items %llu\n", (unsigned long long)nc_calls,
                    (unsigned long long)nc_native, (unsigned long long)nc_fallback, (unsigned long long)nc_items);
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
    env = getenv("SRH_SPIN_PAUSE");
    srh_spin_pause = (env != NULL && atoi(env) != 0);
    env = getenv("SRH_QUERY");
    query_enabled = (env != NULL && atoi(env) != 0);
    env = getenv("SRH_QUERY_TRACE");
    query_trace = (env != NULL && atoi(env) != 0 && !query_enabled);
    if (query_trace)
    {
        pthread_t thread;
        if (pthread_create(&thread, NULL, query_tracer, NULL) == 0) pthread_detach(thread);
        fprintf(stderr, "[srhooks] query trace on (label table pointer at %#llx)\n", (unsigned long long)(0x464AFE8 + AKY));
    }
    fprintf(stderr, "[srhooks] loaded: cloth update rate 1/%u, GPU query copy-forward %s, pause in wait loops %s\n", cloth_rate,
            query_enabled ? "on" : "off", srh_spin_pause ? "on" : "off");
    env = getenv("SRH_NATIVE");
    native_particles = (env != NULL && strstr(env, "particles") != NULL);
    agc_dcb_push_marker = (void *(*)(void *, const char *, uint32_t))dlsym(RTLD_DEFAULT, "+kSrjIVxKFE");   // sceAgcDcbPushMarker
    agc_dcb_pop_marker = (void *(*)(void *))dlsym(RTLD_DEFAULT, "H7uZqCoNuWk");                          // sceAgcDcbPopMarker
    if (native_particles) fprintf(stderr, "[srhooks] native particles on (marker functions %s)\n", (agc_dcb_push_marker && agc_dcb_pop_marker) ? "found" : "MISSING");
    native_cloth = (env != NULL && strstr(env, "cloth") != NULL);
    cloth_dump_dir = getenv("SRH_CLOTH_DUMP");
    cloth_capture_dir = getenv("SRH_CLOTH_CAPTURE");
    env = getenv("SRH_CLOTH_CAPTURE_AT");
    if (env != NULL && atoi(env) > 0) cloth_capture_at = (uint64_t)atoi(env);
    if (cloth_capture_dir != NULL) fprintf(stderr, "[srhooks] cloth capture at call %llu to %s\n", (unsigned long long)cloth_capture_at, cloth_capture_dir);
    native_pdraw = (env != NULL && strstr(env, "pdraw") != NULL);
    agc_dcb_dma_data = (void *(*)(void *, uint8_t, uint8_t, uint8_t, uint64_t, uint8_t, uint8_t, uint64_t, uint32_t, uint8_t, uint8_t, uint8_t))
        dlsym(RTLD_DEFAULT, "WmAc2MEj6Io");                                                                 // sceAgcDcbDmaData
    if (native_pdraw) fprintf(stderr, "[srhooks] particle draw start by GPU copy on (DmaData %s)\n", agc_dcb_dma_data ? "found" : "MISSING");
    agc_acb_push_marker = (void *(*)(void *, const char *, uint32_t))dlsym(RTLD_DEFAULT, "cpCILPya5Zk");   // sceAgcAcbPushMarker
    agc_acb_pop_marker = (void *(*)(void *))dlsym(RTLD_DEFAULT, "6mFxkVqdmbQ");                          // sceAgcAcbPopMarker
    if (native_cloth) fprintf(stderr, "[srhooks] native cloth on (marker functions %s)\n", (agc_acb_push_marker && agc_acb_pop_marker) ? "found" : "MISSING");
    if (query_enabled || native_particles || native_cloth || native_pdraw)
    {
        pthread_t thread;
        if (pthread_create(&thread, NULL, query_reporter, NULL) == 0) pthread_detach(thread);
    }
    env = getenv("SRH_WAIT_TRACE");
    if (env != NULL && atoi(env) != 0)
    {
        const char *path = getenv("SRH_WAIT_TRACE_FILE");
        pthread_t thread;
        if (path == NULL) path = "srh-wait-trace.txt";
        if (pthread_create(&thread, NULL, wt_writer, (void *)path) == 0)
        {
            pthread_detach(thread);
            __atomic_store_n(&wt_on, 1, __ATOMIC_RELEASE);
            fprintf(stderr, "[srhooks] wait trace on: %s\n", path);
        }
    }
}

__attribute__((destructor)) static void srhooks_fini(void)
{
    fprintf(stderr, "[srhooks] cloth update calls %llu, runs %llu\n", (unsigned long long)cloth_calls, (unsigned long long)cloth_runs);
}
