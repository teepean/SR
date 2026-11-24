// shared declarations of libysr (hook: ysr_hook.c, reporter: ysr.c)
#include <stddef.h>
#include <stdint.h>

enum { CAT_OTHER, CAT_PUSH, CAT_POP, CAT_DISPATCH, CAT_DRAW, CAT_SYNC, CAT_SUBMIT, CAT_FLIP, CAT_COUNT };

#include "ysr_names.h"

extern void *ysr_real[YSR_NUM_FUNCS] __attribute__((visibility("hidden")));
void ysr_hook(int function, uint64_t arg0, uint64_t arg1, uint64_t return_address, uint64_t *saved, uint64_t frame) __attribute__((visibility("hidden")));
extern uint64_t ysr_selfwait_start, ysr_selfwait_end, ysr_selfwait_changed, ysr_helper_start, ysr_helper_end;
#define YSR_HELPER_CALLER 0xff
#define DETAIL_SLOTS 4096
typedef struct { uint64_t key; uint32_t function; uint64_t count; } detail_info;
extern detail_info details[DETAIL_SLOTS];
extern int ysr_dcb_wait_reg_mem;
extern uint64_t ysr_watch_start, ysr_watch_end;
extern int ysr_helper_any;
// YSR_DUMP=<caller>:<pipeline rbp offset>: samples of dispatches made by the helper (YSR_HELPER range) for this caller
#define DUMP_SAMPLES 64
typedef struct { uint32_t x, y, z, tg[3]; uint64_t object, block, pipeline, header; uint32_t words[16]; } dump_sample;
extern uint64_t ysr_dump_caller, ysr_dump_rbp_offset;
extern dump_sample dump_samples[DUMP_SAMPLES];
extern volatile uint32_t dump_count;
extern volatile int ysr_dump_enabled;
extern int ysr_cb_dispatch;

#define MAX_MARKERS 1024
#define MARKER_NAME 64
#define MAX_DEPTH 32
#define BUFFER_SLOTS 4096
#define POINTER_SLOTS 16384
#define SITE_SLOTS 65536

typedef struct {
    char name[MARKER_NAME];
    uint64_t counts[CAT_COUNT];
} marker_info;

typedef struct {
    uint64_t buffer;                // command buffer (first argument)
    int depth;
    uint16_t stack[MAX_DEPTH];
} buffer_state;

typedef struct {
    uint64_t address;               // call site (return address)
    uint32_t key;                   // marker << 8 | function
    uint64_t count;
} site_info;


// marker pushes: call site, callers (rbp chain of the game code) and the parent marker
#define PUSH_SLOTS 8192
#define PUSH_CALLERS 3
typedef struct {
    uint64_t address;               // return address of the PushMarker call
    uint64_t callers[PUSH_CALLERS]; // return addresses from the rbp chain (0: not available)
    uint16_t pushed, parent;
    uint32_t depth;
    uint64_t count;
} push_info;
extern push_info pushes[PUSH_SLOTS];

// YSR_CLOTHDUMP=<file>: one frame of the cloth job's dispatch records (PS5 sub_AF18E0, dispatch site PS5 0xAF5C19)
#define CLOTH_RECORDS 512
#define CLOTH_TABLE 304
typedef struct {
    uint64_t pipeline, table;
    uint8_t pipeline_bytes[64], header_bytes[96], table_bytes[CLOTH_TABLE];
} cloth_record;
typedef struct {
    int32_t records, items;
    uint64_t global_table;
    uint8_t global_bytes[64];
    uint64_t item[CLOTH_RECORDS];
    cloth_record record[CLOTH_RECORDS];
} cloth_dump;
extern cloth_dump ysr_cloth;
extern volatile int ysr_cloth_state;   // 0 off, 1 armed, 2 copying, 3 done
extern uint64_t ysr_cloth_site;        // return address of the dispatch call (YSR_CLOTHSITE; recompiled programs differ)

// YSR_TABLEDUMP=<dispatch return address>:<rbp offset of the user data table pointer>:<table bytes>:<address of the
// shader header pointer> - one dispatch's user data table and shader header (copied in the hook, the memory behind
// their pointers is written by the reporter thread through /proc/self/mem)
typedef struct { uint64_t site, rbp_offset, bytes, header_pointer, table, header; uint8_t table_bytes[256]; } table_dump;
extern table_dump ysr_table_dump;
extern volatile int ysr_table_dump_state;   // 0 off, 1 armed, 2 copying, 3 done

extern marker_info markers[MAX_MARKERS];
extern volatile int num_markers;
extern site_info sites[SITE_SLOTS];
extern uint64_t function_counts[YSR_NUM_FUNCS];
void ysr_lock(void);
void ysr_unlock(void);
