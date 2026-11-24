// ysr_hook - runs inside the game's sceAgc* calls: compiled with -mgeneral-regs-only (the game's vector registers
// stay intact), no libc calls
#include "ysr.h"

void *ysr_real[YSR_NUM_FUNCS];
uint64_t ysr_selfwait_start, ysr_selfwait_end, ysr_selfwait_changed;   // YSR_SELFWAIT: WaitRegMem calls from this range wait "always"
uint64_t ysr_watch_start, ysr_watch_end;                                // YSR_WATCH: call sites of calls with an argument register inside the range
int ysr_helper_any;
uint64_t ysr_dump_caller, ysr_dump_rbp_offset;                          // YSR_DUMP
dump_sample dump_samples[DUMP_SAMPLES];
volatile uint32_t dump_count;
volatile int ysr_dump_enabled;
int ysr_cb_dispatch = -1;                                                     // YSR_HELPER=<start>-<end>: callers of all calls from the helper (YSR_HELPER=1: only WaitRegMem)
uint64_t ysr_helper_start, ysr_helper_end;                              // YSR_HELPER: record callers of this function (rbp frame)
int ysr_dcb_wait_reg_mem = -1;                                          // index of sceAgcDcbWaitRegMem
marker_info markers[MAX_MARKERS];    // marker 0 = no marker
volatile int num_markers = 1;
static buffer_state buffers[BUFFER_SLOTS];
static struct { uint64_t pointer; int marker; } pointers[POINTER_SLOTS];
site_info sites[SITE_SLOTS];
push_info pushes[PUSH_SLOTS];
cloth_dump ysr_cloth;
table_dump ysr_table_dump;
volatile int ysr_table_dump_state;
volatile int ysr_cloth_state;
uint64_t ysr_cloth_site = 0xAF5C19 + 0x400000;

static void copy_bytes(uint8_t *to, uint64_t from, unsigned int size)
{
    unsigned int i;
    if (from < 0x10000 || from > 0x7fffffffffffULL) return;
    for (i = 0; i < size; i++) to[i] = ((const volatile uint8_t *)from)[i];
}

// first cloth dispatch of a frame after arming: all records are built before the dispatch loop
static void dump_cloth(void)
{
    const uint64_t base = 0x400000;
    uint64_t records = *(const uint64_t *)(0x462F360 + base), items = *(const uint64_t *)(0x462F378 + base);
    int n = *(const int32_t *)(0x462F368 + base), m = *(const int32_t *)(0x462F380 + base), i;

    if (n > CLOTH_RECORDS) n = CLOTH_RECORDS;
    if (m > CLOTH_RECORDS) m = CLOTH_RECORDS;
    ysr_cloth.records = n;
    ysr_cloth.items = m;
    ysr_cloth.global_table = *(const uint64_t *)(0x462C7F0 + base);
    copy_bytes(ysr_cloth.global_bytes, ysr_cloth.global_table, 64);
    for (i = 0; i < m && items > 0x10000; i++) ysr_cloth.item[i] = ((const uint64_t *)items)[i];
    for (i = 0; i < n && records > 0x10000; i++)
    {
        cloth_record *r = &ysr_cloth.record[i];
        uint64_t header;
        r->pipeline = ((const uint64_t *)records)[2 * i];
        r->table = ((const uint64_t *)records)[2 * i + 1];
        copy_bytes(r->pipeline_bytes, r->pipeline, 64);
        header = (r->pipeline > 0x10000) ? *(const uint64_t *)(r->pipeline + 0x10) : 0;
        copy_bytes(r->header_bytes, header, 96);
        copy_bytes(r->table_bytes, r->table, CLOTH_TABLE);
    }
}
uint64_t function_counts[YSR_NUM_FUNCS];
static volatile int lock_flag;

void ysr_lock(void) { while (__atomic_test_and_set(&lock_flag, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
void ysr_unlock(void) { __atomic_clear(&lock_flag, __ATOMIC_RELEASE); }

static inline uint64_t hash64(uint64_t value)
{
    value ^= value >> 33; value *= 0xff51afd7ed558ccdULL; value ^= value >> 33;
    return value;
}

// marker number of name (pointer cached, names copied) - called with lock
static int intern_marker(uint64_t pointer)
{
    unsigned int slot, index, length;
    const char *name = (const char *)pointer;

    if (pointer == 0) return 0;
    for (slot = hash64(pointer) & (POINTER_SLOTS - 1); pointers[slot].pointer != 0; slot = (slot + 1) & (POINTER_SLOTS - 1))
    {
        if (pointers[slot].pointer == pointer)
        {
            // the same pointer can point to another name later (buffers) - compare the start
            const char *known = markers[pointers[slot].marker].name;
            for (length = 0; length < MARKER_NAME - 1 && known[length] != 0 && known[length] == name[length]; length++);
            if (known[length] == name[length] || length == MARKER_NAME - 1) return pointers[slot].marker;
            break;
        }
    }

    for (index = 1; index < (unsigned int)num_markers; index++)
    {
        for (length = 0; length < MARKER_NAME - 1 && markers[index].name[length] == name[length] && name[length] != 0; length++);
        if (markers[index].name[length] == name[length] || (length == MARKER_NAME - 1)) break;
    }
    if (index == (unsigned int)num_markers)
    {
        if (num_markers >= MAX_MARKERS) return 0;
        for (length = 0; length < MARKER_NAME - 1 && name[length] != 0; length++) markers[index].name[length] = name[length];
        markers[index].name[length] = 0;
        __atomic_store_n(&num_markers, num_markers + 1, __ATOMIC_RELEASE);
    }

    for (slot = hash64(pointer) & (POINTER_SLOTS - 1); pointers[slot].pointer != 0 && pointers[slot].pointer != pointer; slot = (slot + 1) & (POINTER_SLOTS - 1));
    pointers[slot].pointer = pointer;
    pointers[slot].marker = (int)index;
    return (int)index;
}

// state of command buffer - called with lock
static buffer_state *find_buffer(uint64_t buffer)
{
    unsigned int slot, tries;

    for (slot = hash64(buffer) & (BUFFER_SLOTS - 1), tries = 0; tries < BUFFER_SLOTS; slot = (slot + 1) & (BUFFER_SLOTS - 1), tries++)
    {
        if (buffers[slot].buffer == buffer) return &buffers[slot];
        if (buffers[slot].buffer == 0)
        {
            buffers[slot].buffer = buffer;
            buffers[slot].depth = 0;
            return &buffers[slot];
        }
    }
    return NULL;
}

detail_info details[DETAIL_SLOTS];   // sync commands by AMD specific arguments (event type, cache control)

static void count_detail(int function, uint64_t key)
{
    unsigned int slot, tries;

    for (slot = hash64(key * 131 + (uint64_t)function) & (DETAIL_SLOTS - 1), tries = 0; tries < 64; slot = (slot + 1) & (DETAIL_SLOTS - 1), tries++)
    {
        if (details[slot].count != 0 && details[slot].key == key && details[slot].function == (uint32_t)function)
        {
            details[slot].count++;
            return;
        }
        if (details[slot].count == 0)
        {
            details[slot].key = key;
            details[slot].function = (uint32_t)function;
            details[slot].count = 1;
            return;
        }
    }
}

static void count_site(uint64_t address, int marker, int function)
{
    unsigned int slot, tries;
    uint32_t key = ((uint32_t)marker << 8) | (uint32_t)function;

    for (slot = hash64(address ^ ((uint64_t)key << 40)) & (SITE_SLOTS - 1), tries = 0; tries < 64; slot = (slot + 1) & (SITE_SLOTS - 1), tries++)
    {
        if (sites[slot].address == address && sites[slot].key == key)
        {
            sites[slot].count++;
            return;
        }
        if (sites[slot].address == 0)
        {
            sites[slot].address = address;
            sites[slot].key = key;
            sites[slot].count = 1;
            return;
        }
    }
}

// frame pointer plausible: above the hook's stack, within 1 MB, aligned
static inline int frame_valid(uint64_t frame, uint64_t below)
{
    return frame > below && frame - below < 0x100000 && (frame & 7) == 0;
}

// marker push with its callers - called with lock
static void count_push(uint64_t address, uint64_t frame, uint64_t below, int pushed, int parent, int depth)
{
    uint64_t callers[PUSH_CALLERS];
    unsigned int slot, tries, level;

    for (level = 0; level < PUSH_CALLERS; level++)
    {
        if (frame_valid(frame, below))
        {
            callers[level] = *(const uint64_t *)(frame + 8);
            below = frame;
            frame = (callers[level] != 0) ? *(const uint64_t *)frame : 0;
        }
        else
        {
            callers[level] = 0;
            frame = 0;
        }
    }
    for (slot = hash64(address ^ callers[0] ^ ((uint64_t)pushed << 48) ^ ((uint64_t)parent << 32)) & (PUSH_SLOTS - 1), tries = 0; tries < 128; slot = (slot + 1) & (PUSH_SLOTS - 1), tries++)
    {
        push_info *p = &pushes[slot];
        if (p->count != 0 && p->address == address && p->pushed == (uint16_t)pushed && p->parent == (uint16_t)parent &&
            p->callers[0] == callers[0] && p->callers[1] == callers[1] && p->callers[2] == callers[2])
        {
            p->count++;
            return;
        }
        if (p->count == 0)
        {
            p->address = address;
            for (level = 0; level < PUSH_CALLERS; level++) p->callers[level] = callers[level];
            p->pushed = (uint16_t)pushed;
            p->parent = (uint16_t)parent;
            p->depth = (uint32_t)depth;
            __atomic_store_n(&p->count, 1, __ATOMIC_RELEASE);
            return;
        }
    }
}

__attribute__((used)) void ysr_hook(int function, uint64_t arg0, uint64_t arg1, uint64_t return_address, uint64_t *saved, uint64_t frame)
{
    int category = ysr_categories[function];
    buffer_state *state;
    int marker;

    __atomic_add_fetch(&function_counts[function], 1, __ATOMIC_RELAXED);

    // YSR_SELFWAIT: the game's barrier helper waits until its own end-of-pipe label is written (full drain);
    // compare function 0 (always) keeps the packet (the game patches its address later) but doesn't wait
    if (ysr_selfwait_end != 0 && return_address >= ysr_selfwait_start && return_address < ysr_selfwait_end &&
        function == ysr_dcb_wait_reg_mem)
    {
        saved[4] = 0;   // rdx = compareFunction (sceAgcDcbWaitRegMem)
        __atomic_add_fetch(&ysr_selfwait_changed, 1, __ATOMIC_RELAXED);
    }

    // YSR_DUMP: helper frame (rbp) -> saved caller rbp at [rbp], saved caller r14 (object) at [rbp-0x10]
    if (ysr_dump_enabled && function == ysr_cb_dispatch && return_address >= ysr_helper_start && return_address < ysr_helper_end &&
        frame > (uint64_t)saved && frame - (uint64_t)saved < 0x100000 && (frame & 7) == 0 &&
        *(const uint64_t *)(frame + 8) == ysr_dump_caller && dump_count < DUMP_SAMPLES)
    {
        uint32_t index = __atomic_fetch_add(&dump_count, 1, __ATOMIC_RELAXED);
        if (index < DUMP_SAMPLES)
        {
            dump_sample *sample = &dump_samples[index];
            uint64_t caller_rbp = *(const uint64_t *)frame;
            int w;
            sample->x = (uint32_t)saved[5];   // rsi
            sample->y = (uint32_t)saved[4];   // rdx
            sample->z = (uint32_t)saved[3];   // rcx
            sample->object = *(const uint64_t *)(frame - 0x10);
            sample->block = (sample->object > 0x10000) ? *(const uint64_t *)(sample->object + 0x120) : 0;
            sample->pipeline = *(const uint64_t *)(caller_rbp - ysr_dump_rbp_offset);
            sample->header = (sample->pipeline > 0x10000) ? *(const uint64_t *)(sample->pipeline + 0x10) : 0;
            if (sample->header > 0x10000)
            {
                sample->tg[0] = *(const uint32_t *)(sample->header + 0x40);
                sample->tg[1] = *(const uint32_t *)(sample->header + 0x44);
                sample->tg[2] = *(const uint32_t *)(sample->header + 0x48);
            }
            if (sample->block > 0x10000)
            {
                for (w = 0; w < 16; w++) sample->words[w] = ((const volatile uint32_t *)sample->block)[w];
            }
        }
    }

    if (ysr_cloth_state == 1 && function == ysr_cb_dispatch && return_address == ysr_cloth_site &&
        __atomic_compare_exchange_n(&ysr_cloth_state, &(int){1}, 2, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        dump_cloth();
        __atomic_store_n(&ysr_cloth_state, 3, __ATOMIC_RELEASE);
    }

    if (ysr_table_dump_state == 1 && function == ysr_cb_dispatch && return_address == ysr_table_dump.site &&
        frame > (uint64_t)saved && frame - (uint64_t)saved < 0x100000 &&
        __atomic_compare_exchange_n(&ysr_table_dump_state, &(int){1}, 2, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        ysr_table_dump.table = *(const uint64_t *)(frame - ysr_table_dump.rbp_offset);
        ysr_table_dump.header = (ysr_table_dump.header_pointer > 0x10000) ? *(const uint64_t *)ysr_table_dump.header_pointer : 0;
        copy_bytes(ysr_table_dump.table_bytes, ysr_table_dump.table, (unsigned int)ysr_table_dump.bytes);
        __atomic_store_n(&ysr_table_dump_state, 3, __ATOMIC_RELEASE);
    }

    if (ysr_watch_end != 0)
    {
        int r;
        for (r = 1; r <= 6; r++)
        {
            if (saved[r] >= ysr_watch_start && saved[r] < ysr_watch_end)
            {
                count_detail(function, return_address | (1ULL << 63));
                break;
            }
        }
    }

    ysr_lock();
    state = (category == CAT_SUBMIT || category == CAT_OTHER) ? NULL : find_buffer(arg0);
    marker = (state != NULL && state->depth > 0) ? state->stack[(state->depth <= MAX_DEPTH ? state->depth : MAX_DEPTH) - 1] : 0;

    switch (category)
    {
        case CAT_PUSH:
            if (state != NULL)
            {
                int pushed = intern_marker(arg1);
                count_push(return_address, frame, (uint64_t)saved, pushed, marker, state->depth);
                if (state->depth < MAX_DEPTH) state->stack[state->depth] = (uint16_t)pushed;
                state->depth++;
                markers[pushed].counts[CAT_PUSH]++;
            }
            break;
        case CAT_POP:
            if (state != NULL && state->depth > 0) state->depth--;
            break;
        case CAT_DISPATCH:
        case CAT_DRAW:
        case CAT_SYNC:
            markers[marker].counts[category]++;
            count_site(return_address, marker, function);
            {
                // saved registers: rax r9 r8 rcx rdx rsi rdi
                const char *name = ysr_names[function];
                uint64_t key = 0;
                if (name[9] == 'E' && name[10] == 'v') key = saved[5] & 0xff;                                       // EventWrite: event type
                else if (name[6] == 'D' && name[9] == 'A' && name[10] == 'c') key = ((saved[4] & 0xffffffff) << 32) | (saved[3] & 0xffffffff);   // DcbAcquireMem: cbDbOp, gcrControl
                else if (name[6] == 'A' && name[9] == 'A' && name[10] == 'c') key = saved[5] & 0xffffffff;          // AcbAcquireMem: gcrControl
                else if (name[8] == 'R' && name[9] == 'e' && name[10] == 'l') key = ((saved[5] & 0xff) << 32) | (saved[4] & 0xffff);    // CbReleaseMem: action, gcrControl
                count_detail(function, key);
            }
            // caller of the game's helper function (the helper keeps an rbp frame): once per WaitRegMem
            if ((function == ysr_dcb_wait_reg_mem || ysr_helper_any) && return_address >= ysr_helper_start && return_address < ysr_helper_end &&
                frame > (uint64_t)saved && frame - (uint64_t)saved < 0x100000 && (frame & 7) == 0)
            {
                count_site(*(const uint64_t *)(frame + 8), marker, YSR_HELPER_CALLER);
            }
            break;
        default:
            markers[marker].counts[category]++;
            break;
    }
    ysr_unlock();
}

