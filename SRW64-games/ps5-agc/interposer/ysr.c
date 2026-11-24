// libysr.so - counts the game's sceAgc* calls per GPU marker (LD_PRELOAD interposer)
//
// The trampolines (ysr_tramp.S, generated) call ysr_hook() with the original arguments and then jump to the real
// function (dlsym RTLD_NEXT), so the caller's stack is unchanged.
// ysr_hook() runs inside game calls: it is compiled with -mgeneral-regs-only (vector registers of the game stay
// intact) and uses no libc functions (only atomics and own tables).
//
// Every 10 s (YSR_INTERVAL) a reporter thread prints to stderr:
//   [ysr] frames N (per frame values below), then per marker: dispatches, draws, sync commands, other calls
//   [ysr] site ... - the most frequent call sites (return addresses) of dispatches/draws/syncs with their marker
//
// Environment: YSR_INTERVAL=<seconds>, YSR_MARKERS=<number of markers printed, default 30>,
//              YSR_SITES=<number of call sites printed, default 40>, YSR_OFF=1 (no reports)

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include "ysr.h"

static void missing_function(void)
{
    static const char message[] = "[ysr] called function which wasn't found by dlsym\n";
    if (write(2, message, sizeof(message) - 1) < 0) {}
    abort();
}

#define MAX_POKES 8
static struct { volatile uint8_t *address; uint8_t value; } pokes[MAX_POKES];
static unsigned int num_pokes;
static volatile uint64_t ysr_pokes_changed;

typedef struct { int index; uint64_t value; } sort_entry;

static int compare_desc(const void *a, const void *b)
{
    uint64_t x = ((const sort_entry *)a)->value, y = ((const sort_entry *)b)->value;
    return (x < y) ? 1 : ((x > y) ? -1 : 0);
}

// YSR_PUSHMAP=<file>: the complete marker push map (rewritten every interval): count, pushes per frame, depth,
// marker, parent marker, call site and three callers (rbp chain); eboot addresses as PS5 addresses (- 0x400000)
static const char *push_address(uint64_t address, char *text, size_t size)
{
    if (address == 0) snprintf(text, size, "-");
    else if (address >= 0x400000 && address < 0x10000000) snprintf(text, size, "%llx", (unsigned long long)(address - 0x400000));
    else snprintf(text, size, "!%llx", (unsigned long long)address);
    return text;
}

static void write_pushmap(const char *path, uint64_t frames)
{
    char temporary[512], a[4][32];
    unsigned int index;
    FILE *file;

    snprintf(temporary, sizeof(temporary), "%s.new", path);
    if ((file = fopen(temporary, "w")) == NULL) return;
    fprintf(file, "# frames %llu\n# count per_frame depth marker parent site(ps5) caller1 caller2 caller3\n", (unsigned long long)frames);
    for (index = 0; index < PUSH_SLOTS; index++)
    {
        const push_info *p = &pushes[index];
        uint64_t count = __atomic_load_n(&p->count, __ATOMIC_ACQUIRE);
        if (count == 0) continue;
        fprintf(file, "%llu\t%.2f\t%u\t%s\t%s\t%s\t%s\t%s\t%s\n", (unsigned long long)count, frames ? (double)count / frames : 0.0, p->depth,
                markers[p->pushed].name, markers[p->parent].name, push_address(p->address, a[0], 32), push_address(p->callers[0], a[1], 32),
                push_address(p->callers[1], a[2], 32), push_address(p->callers[2], a[3], 32));
    }
    fclose(file);
    rename(temporary, path);
}

static void *reporter(void *argument)
{
    static marker_info previous[MAX_MARKERS];
    static uint64_t previous_sites[SITE_SLOTS];
    static sort_entry order[SITE_SLOTS];
    unsigned int interval = 10, print_markers = 30, print_sites = 40, index, count;
    uint64_t previous_flips = 0;
    const char *env;

    (void)argument;
    if ((env = getenv("YSR_INTERVAL")) != NULL && atoi(env) > 0) interval = (unsigned int)atoi(env);
    if ((env = getenv("YSR_MARKERS")) != NULL) print_markers = (unsigned int)atoi(env);
    if ((env = getenv("YSR_SITES")) != NULL) print_sites = (unsigned int)atoi(env);

    for (;;)
    {
        marker_info current[MAX_MARKERS];
        uint64_t flips = 0, frames, total[CAT_COUNT];
        int n;

        sleep(interval);

        ysr_lock();
        n = num_markers;
        memcpy(current, markers, sizeof(marker_info) * (size_t)n);
        ysr_unlock();

        for (index = 0; index < YSR_NUM_FUNCS; index++)
        {
            if (ysr_categories[index] == CAT_FLIP) flips += __atomic_load_n(&function_counts[index], __ATOMIC_RELAXED);
        }
        frames = flips - previous_flips;
        previous_flips = flips;
        if (frames == 0) frames = 1;

        memset(total, 0, sizeof(total));
        count = 0;
        for (index = 0; index < (unsigned int)n; index++)
        {
            uint64_t value = 0;
            int c;
            for (c = 0; c < CAT_COUNT; c++)
            {
                uint64_t delta = current[index].counts[c] - previous[index].counts[c];
                total[c] += delta;
                if (c == CAT_DISPATCH || c == CAT_DRAW || c == CAT_SYNC) value += delta;
            }
            if (value != 0) { order[count].index = (int)index; order[count].value = value; count++; }
        }
        qsort(order, count, sizeof(sort_entry), compare_desc);

        fprintf(stderr, "[ysr] frames %llu in %us: per frame dispatch %.1f draw %.1f sync %.1f markers %.1f other %.1f; self-waits changed %llu; pokes changed %llu\n",
                (unsigned long long)(flips - (previous_flips - frames)), interval,
                (double)total[CAT_DISPATCH] / frames, (double)total[CAT_DRAW] / frames, (double)total[CAT_SYNC] / frames,
                (double)total[CAT_PUSH] / frames, (double)total[CAT_OTHER] / frames,
                (unsigned long long)__atomic_load_n(&ysr_selfwait_changed, __ATOMIC_RELAXED), (unsigned long long)ysr_pokes_changed);
        for (index = 0; index < count && index < print_markers; index++)
        {
            const marker_info *m = &current[order[index].index], *p = &previous[order[index].index];
            fprintf(stderr, "[ysr] marker %-40s dispatch %8.1f draw %8.1f sync %8.1f pushes %6.1f\n",
                    (order[index].index == 0) ? "(none)" : m->name,
                    (double)(m->counts[CAT_DISPATCH] - p->counts[CAT_DISPATCH]) / frames,
                    (double)(m->counts[CAT_DRAW] - p->counts[CAT_DRAW]) / frames,
                    (double)(m->counts[CAT_SYNC] - p->counts[CAT_SYNC]) / frames,
                    (double)(m->counts[CAT_PUSH] - p->counts[CAT_PUSH]) / frames);
        }
        memcpy(previous, current, sizeof(marker_info) * (size_t)n);
        if ((env = getenv("YSR_PUSHMAP")) != NULL) write_pushmap(env, flips);

        // sync commands by arguments
        {
            static uint64_t previous_details[DETAIL_SLOTS];
            unsigned int print_details = 40;
            if ((env = getenv("YSR_DETAILS")) != NULL) print_details = (unsigned int)atoi(env);
            count = 0;
            for (index = 0; index < DETAIL_SLOTS; index++)
            {
                uint64_t value = __atomic_load_n(&details[index].count, __ATOMIC_RELAXED);
                if (value != previous_details[index])
                {
                    order[count].index = (int)index;
                    order[count].value = value - previous_details[index];
                    previous_details[index] = value;
                    count++;
                }
            }
            qsort(order, count, sizeof(sort_entry), compare_desc);
            for (index = 0; index < count && index < print_details; index++)
            {
                const detail_info *dt = &details[order[index].index];
                fprintf(stderr, "[ysr] sync %-32s args 0x%016llx %8.1f/frame\n", ysr_names[dt->function], (unsigned long long)dt->key, (double)order[index].value / frames);
            }
        }

        // YSR_CLOTHDUMP=<file>: armed after YSR_DUMP_DELAY, written once done
        {
            static unsigned int cloth_elapsed;
            static int cloth_written;
            const char *path = getenv("YSR_CLOTHDUMP"), *delay = getenv("YSR_DUMP_DELAY");
            cloth_elapsed += interval;
            if (path != NULL && ysr_cloth_state == 0 && cloth_elapsed >= (unsigned int)((delay != NULL) ? atoi(delay) : 60))
            {
                ysr_cloth_state = 1;
                fprintf(stderr, "[ysr] cloth dump armed\n");
            }
            if (path != NULL && !cloth_written && __atomic_load_n(&ysr_cloth_state, __ATOMIC_ACQUIRE) == 3)
            {
                FILE *file = fopen(path, "wb");
                cloth_written = 1;
                if (file != NULL) { fwrite(&ysr_cloth, 1, sizeof(ysr_cloth), file); fclose(file); }
                fprintf(stderr, "[ysr] cloth dump: %d records, %d items, global table 0x%llx -> %s\n", ysr_cloth.records, ysr_cloth.items,
                        (unsigned long long)ysr_cloth.global_table, path);
            }
        }

        // YSR_DUMP_DELAY=<seconds> (default 60): sampling starts after this time
        {
            static unsigned int elapsed;
            const char *delay = getenv("YSR_DUMP_DELAY");
            elapsed += interval;
            if (ysr_dump_caller != 0 && !ysr_dump_enabled && elapsed >= (unsigned int)((delay != NULL) ? atoi(delay) : 60))
            {
                ysr_dump_enabled = 1;
                fprintf(stderr, "[ysr] dump sampling started\n");
            }
        }

        // YSR_DUMP: samples once all are collected; header and memory behind pointers in it to files (/proc/self/mem: no faults)
        {
            static int dumped;
            if (!dumped && ysr_dump_caller != 0 && __atomic_load_n(&dump_count, __ATOMIC_RELAXED) >= DUMP_SAMPLES)
            {
                const char *directory = getenv("YSR_DUMP_DIR");
                unsigned int s, w;
                int mem = open("/proc/self/mem", O_RDONLY);
                dumped = 1;
                for (s = 0; s < DUMP_SAMPLES; s++)
                {
                    const dump_sample *sample = &dump_samples[s];
                    fprintf(stderr, "[ysr] dump %2u groups %u %u %u tg %u %u %u object 0x%llx block 0x%llx pipeline 0x%llx header 0x%llx words",
                            s, sample->x, sample->y, sample->z, sample->tg[0], sample->tg[1], sample->tg[2], (unsigned long long)sample->object,
                            (unsigned long long)sample->block, (unsigned long long)sample->pipeline, (unsigned long long)sample->header);
                    for (w = 0; w < 16; w++) fprintf(stderr, " %08x", sample->words[w]);
                    fprintf(stderr, "\n");
                }
                if (mem >= 0 && directory != NULL && dump_samples[0].header > 0x10000)
                {
                    static uint8_t buffer[0x10000];
                    char name[512];
                    uint64_t header[0x20];
                    FILE *file;
                    if (pread(mem, header, sizeof(header), (off_t)dump_samples[0].header) == (ssize_t)sizeof(header))
                    {
                        snprintf(name, sizeof(name), "%s/header_%llx.bin", directory, (unsigned long long)dump_samples[0].header);
                        if ((file = fopen(name, "wb")) != NULL) { fwrite(header, 1, sizeof(header), file); fclose(file); }
                        for (w = 0; w < 0x20; w++)
                        {
                            ssize_t got;
                            if (header[w] < 0x100000 || header[w] > 0x7fffffffffffULL) continue;
                            got = pread(mem, buffer, sizeof(buffer), (off_t)header[w]);
                            if (got <= 0) continue;
                            snprintf(name, sizeof(name), "%s/ptr%02x_%llx.bin", directory, w * 8, (unsigned long long)header[w]);
                            if ((file = fopen(name, "wb")) != NULL) { fwrite(buffer, 1, (size_t)got, file); fclose(file); }
                        }
                        fprintf(stderr, "[ysr] dump: header and pointed memory written to %s\n", directory);
                    }
                }
                if (mem >= 0) close(mem);
            }
        }

        // call sites
        count = 0;
        for (index = 0; index < SITE_SLOTS; index++)
        {
            uint64_t value = __atomic_load_n(&sites[index].count, __ATOMIC_RELAXED);
            if (sites[index].address != 0 && value != previous_sites[index])
            {
                order[count].index = (int)index;
                order[count].value = value - previous_sites[index];
                previous_sites[index] = value;
                count++;
            }
        }
        qsort(order, count, sizeof(sort_entry), compare_desc);
        for (index = 0; index < count && index < print_sites; index++)
        {
            const site_info *s = &sites[order[index].index];
            Dl_info info;
            const char *object = "?";
            uint64_t base = 0;
            if (dladdr((void *)s->address, &info) && info.dli_fname != NULL) { object = info.dli_fname; base = (uint64_t)info.dli_fbase; }
            fprintf(stderr, "[ysr] site 0x%llx (%s+0x%llx) %-28s %-40s %8.1f/frame\n",
                    (unsigned long long)s->address, strrchr(object, '/') ? strrchr(object, '/') + 1 : object,
                    (unsigned long long)(s->address - base), ((s->key & 0xff) == YSR_HELPER_CALLER) ? "(caller of helper)" : ysr_names[s->key & 0xff],
                    (s->key >> 8) ? markers[s->key >> 8].name : "(none)", (double)order[index].value / frames);
        }
    }
    return NULL;
}

// YSR_POKE=<address>:<byte>[,<address>:<byte>...] - keeps bytes of the game's data set (every 20 ms, after
// YSR_POKE_DELAY seconds), e.g. YSR_POKE=0x71daf04:0 turns off the CPU occlusion against the GPU depth grid

static void *poker(void *argument)
{
    const char *env = getenv("YSR_POKE_DELAY");
    const char *interval_env = getenv("YSR_POKE_US");    // recheck interval in microseconds (default 20000)
    long interval_ns = (interval_env != NULL && atol(interval_env) > 0) ? atol(interval_env) * 1000 : 20000000;
    unsigned int index;

    (void)argument;
    if (env != NULL && atoi(env) > 0) sleep((unsigned int)atoi(env));
    fprintf(stderr, "[ysr] poke active (%u addresses, every %ld us)\n", num_pokes, interval_ns / 1000);
    for (;;)
    {
        struct timespec delay = { interval_ns / 1000000000, interval_ns % 1000000000 };
        for (index = 0; index < num_pokes; index++)
        {
            if (*pokes[index].address != pokes[index].value)
            {
                *pokes[index].address = pokes[index].value;
                ysr_pokes_changed++;
            }
        }
        nanosleep(&delay, NULL);
    }
    return NULL;
}

__attribute__((constructor)) static void ysr_init(void)
{
    unsigned int index, missing = 0;
    pthread_t thread;

    for (index = 0; index < YSR_NUM_FUNCS; index++)
    {
        ysr_real[index] = dlsym(RTLD_NEXT, ysr_nids[index]);
        if (ysr_real[index] == NULL)
        {
            ysr_real[index] = (void *)missing_function;
            missing++;
        }
    }
    strcpy(markers[0].name, "(none)");
    if (missing == YSR_NUM_FUNCS) return;   // other process (e.g. gamescope) - LD_PRELOAD is inherited
    {
        // YSR_SELFWAIT=1 (game barrier helper sub_172E0B0 of the AnyKyty eboot) or YSR_SELFWAIT=<start>-<end>
        const char *selfwait = getenv("YSR_SELFWAIT");
        if (selfwait != NULL && strcmp(selfwait, "1") == 0)
        {
            ysr_selfwait_start = 0x1b2e0b0;
            ysr_selfwait_end = 0x1b2e7c0;
        }
        else if (selfwait != NULL && strchr(selfwait, '-') != NULL)
        {
            ysr_selfwait_start = strtoull(selfwait, NULL, 0);
            ysr_selfwait_end = strtoull(strchr(selfwait, '-') + 1, NULL, 0);
        }
        for (index = 0; index < YSR_NUM_FUNCS; index++)
        {
            if (strcmp(ysr_names[index], "sceAgcDcbWaitRegMem") == 0) ysr_dcb_wait_reg_mem = (int)index;
            if (strcmp(ysr_names[index], "sceAgcCbDispatch") == 0) ysr_cb_dispatch = (int)index;
        }
        // YSR_WATCH=<start>-<end>: "[ysr] sync <function> args 0x8000..<return address>" lines for calls with an argument in the range
        selfwait = getenv("YSR_WATCH");
        if (selfwait != NULL && strchr(selfwait, '-') != NULL)
        {
            ysr_watch_start = strtoull(selfwait, NULL, 0);
            ysr_watch_end = strtoull(strchr(selfwait, '-') + 1, NULL, 0);
            fprintf(stderr, "[ysr] watch 0x%llx-0x%llx\n", (unsigned long long)ysr_watch_start, (unsigned long long)ysr_watch_end);
        }
        // YSR_DUMP=<caller return address>:<offset of the pipeline pointer below the caller's rbp> (needs YSR_HELPER=<range>)
        selfwait = getenv("YSR_DUMP");
        if (selfwait != NULL && strchr(selfwait, ':') != NULL)
        {
            ysr_dump_caller = strtoull(selfwait, NULL, 0);
            ysr_dump_rbp_offset = strtoull(strchr(selfwait, ':') + 1, NULL, 0);
            fprintf(stderr, "[ysr] dump dispatches for caller 0x%llx (pipeline at caller rbp-0x%llx)\n", (unsigned long long)ysr_dump_caller, (unsigned long long)ysr_dump_rbp_offset);
        }
        // YSR_HELPER=1 (barrier helper of the AnyKyty eboot, also with YSR_SELFWAIT=1) or YSR_HELPER=<start>-<end>
        selfwait = getenv("YSR_HELPER");
        if (selfwait != NULL && strchr(selfwait, '-') != NULL)
        {
            ysr_helper_start = strtoull(selfwait, NULL, 0);
            ysr_helper_end = strtoull(strchr(selfwait, '-') + 1, NULL, 0);
            ysr_helper_any = 1;
            fprintf(stderr, "[ysr] helper 0x%llx-0x%llx: callers of all calls from the helper\n", (unsigned long long)ysr_helper_start, (unsigned long long)ysr_helper_end);
        }
        else if (selfwait != NULL || ysr_selfwait_end != 0)
        {
            ysr_helper_start = (ysr_selfwait_end != 0) ? ysr_selfwait_start : 0x1b2e0b0;
            ysr_helper_end = (ysr_selfwait_end != 0) ? ysr_selfwait_end : 0x1b2e7c0;
        }
        if (ysr_selfwait_end != 0) fprintf(stderr, "[ysr] self-wait range 0x%llx-0x%llx: WaitRegMem compare function -> always\n", (unsigned long long)ysr_selfwait_start, (unsigned long long)ysr_selfwait_end);
    }
    if (getenv("YSR_CLOTHSITE") != NULL) ysr_cloth_site = strtoull(getenv("YSR_CLOTHSITE"), NULL, 0);
    fprintf(stderr, "[ysr] libysr loaded: %u functions interposed, %u not found\n", YSR_NUM_FUNCS - missing, missing);
    {
        const char *poke = getenv("YSR_POKE");
        while (poke != NULL && *poke != 0 && num_pokes < MAX_POKES)
        {
            char *end;
            uint64_t address = strtoull(poke, &end, 0);
            if (*end != ':' || address < 0x400000) break;
            pokes[num_pokes].address = (volatile uint8_t *)address;
            pokes[num_pokes].value = (uint8_t)strtoul(end + 1, &end, 0);
            fprintf(stderr, "[ysr] poke 0x%llx = %u\n", (unsigned long long)address, pokes[num_pokes].value);
            num_pokes++;
            poke = (*end == ',') ? end + 1 : end;
        }
        if (num_pokes != 0)
        {
            pthread_t poke_thread;
            if (pthread_create(&poke_thread, NULL, poker, NULL) == 0) pthread_detach(poke_thread);
        }
    }

    if (getenv("YSR_OFF") == NULL && pthread_create(&thread, NULL, reporter, NULL) == 0) pthread_detach(thread);
}
