/**
 *
 *  KERNEL32 emulation.
 *
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "compat.h"
#include <sys/stat.h>
#include "ptr32.h"
#include "Game-Memory.h"
#include "platform.h"
#include "camera.h"
#include "config.h"
#include "vfs.h"
#include "winapi.h"
#include "display.h"
#include "sysmem.h"
#include "peimage.h"
#include "printf_x86.h"
#include "asmsyms.h"
#include <pthread.h>

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)


/* ------------------------------------------------------------------ */
/* errors                                                              */

static uint32_t last_error;

uint32_t CCALL GetLastError_c(void) { return last_error; }
void CCALL SetLastError_c(uint32_t err) { last_error = err; }
void winapi_set_last_error(uint32_t err) { last_error = err; }

static uint32_t errno_to_win(int err)
{
    switch (err)
    {
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
        case EEXIST: return ERROR_FILE_EXISTS;
        case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
        default: return ERROR_INVALID_PARAMETER;
    }
}


/* ------------------------------------------------------------------ */
/* handles                                                             */

enum { HT_FILE = 1, HT_FIND, HT_MAPPING, HT_HEAP, HT_PROCESS, HT_THREAD };

typedef struct {
    uint32_t type;
    int fd;
} win_file;

typedef struct {
    uint32_t type;
    DIR *dir;
    char dirpath[1024];
    char pattern[260];
} find_handle;

typedef struct {
    uint32_t type;
    int fd;           // -1 for pagefile backed mappings
    uint32_t size;
    int writable;
} mapping_handle;

// the pseudo handles (the game sees their addresses: below 2 GB, LOW_OBJ)
static uint32_t process_handle_obj_init = HT_PROCESS;
static uint32_t thread_handle_obj_init = HT_THREAD;
LOW_SLOT(process_handle_obj) LOW_SLOT(thread_handle_obj)
#define process_handle_obj LOW_OBJ(process_handle_obj)
#define thread_handle_obj LOW_OBJ(thread_handle_obj)

static uint32_t handle_type(void *h)
{
    // small values: the standard handles of GetStdHandle (1-3), not objects
    if ((H2G(h) < 0x10000) || (H2G(h) == 0xFFFFFFFFu)) return 0;     // INVALID_HANDLE_VALUE is a 32-bit -1
    return *(uint32_t *)h;
}

uint32_t CCALL CloseHandle_c(void *hObject)
{
    switch (handle_type(hObject))
    {
        case HT_FILE:
            close(((win_file *)hObject)->fd);
            game_free(hObject);
            return 1;
        case HT_MAPPING:
            if (((mapping_handle *)hObject)->fd >= 0) close(((mapping_handle *)hObject)->fd);
            game_free(hObject);
            return 1;
        case HT_PROCESS:
        case HT_THREAD:
        case 0x45564e54:    // HT_EVENT
        case 0x54485244:    // HT_WTHREAD: kept (a running thread still uses it)
            return 1;
        default:
            last_error = ERROR_INVALID_HANDLE;
            return 0;
    }
}


/* ------------------------------------------------------------------ */
/* files                                                               */

#define GENERIC_READ  0x80000000
#define GENERIC_WRITE 0x40000000
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5

void * CCALL CreateFileA_c(const char *lpFileName, uint32_t dwDesiredAccess, uint32_t dwShareMode, void *lpSecurityAttributes, uint32_t dwCreationDisposition, uint32_t dwFlagsAndAttributes, void *hTemplateFile)
{
    char path[1024];
    int flags, fd, exists;
    win_file *h;

    if (lpFileName == NULL)
    {
        last_error = ERROR_INVALID_PARAMETER;
        return INVALID_HANDLE_VALUE;
    }

    exists = vfs_resolve(lpFileName, path, sizeof(path));

    if ((dwDesiredAccess & GENERIC_READ) && (dwDesiredAccess & GENERIC_WRITE)) flags = O_RDWR;
    else if (dwDesiredAccess & GENERIC_WRITE) flags = O_WRONLY;
    else flags = O_RDONLY;

    switch (dwCreationDisposition)
    {
        case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
        case OPEN_EXISTING: break;
        case OPEN_ALWAYS: flags |= O_CREAT; break;
        case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    }

    fd = open(path, flags | O_BINARY, 0644);
    if (winapi_debug)
    {
        eprintf("CreateFileA: %s -> %s (%s): %s\n", lpFileName, path, exists ? "exists" : "new", (fd >= 0) ? "ok" : "failed");
    }
    if (fd < 0)
    {
        last_error = errno_to_win(errno);
        return INVALID_HANDLE_VALUE;
    }

    h = (win_file *) game_malloc(sizeof(win_file));
    h->type = HT_FILE;
    h->fd = fd;

    last_error = ((dwCreationDisposition == OPEN_ALWAYS || dwCreationDisposition == CREATE_ALWAYS) && exists) ? ERROR_ALREADY_EXISTS : 0;
    return h;
}

uint32_t CCALL ReadFile_c(void *hFile, void *lpBuffer, uint32_t nNumberOfBytesToRead, uint32_t *lpNumberOfBytesRead, void *lpOverlapped)
{
    ssize_t res;

    if (lpNumberOfBytesRead != NULL) *lpNumberOfBytesRead = 0;
    if (handle_type(hFile) != HT_FILE)
    {
        last_error = ERROR_INVALID_HANDLE;
        return 0;
    }

    res = read(((win_file *)hFile)->fd, lpBuffer, nNumberOfBytesToRead);
    if (res < 0)
    {
        last_error = ERROR_READ_FAULT;
        return 0;
    }
    if (lpNumberOfBytesRead != NULL) *lpNumberOfBytesRead = (uint32_t) res;
    return 1;
}

uint32_t CCALL WriteFile_c(void *hFile, const void *lpBuffer, uint32_t nNumberOfBytesToWrite, uint32_t *lpNumberOfBytesWritten, void *lpOverlapped)
{
    ssize_t res;

    if (lpNumberOfBytesWritten != NULL) *lpNumberOfBytesWritten = 0;
    if (handle_type(hFile) != HT_FILE)
    {
        last_error = ERROR_INVALID_HANDLE;
        return 0;
    }

    res = write(((win_file *)hFile)->fd, lpBuffer, nNumberOfBytesToWrite);
    if (res < 0)
    {
        last_error = ERROR_WRITE_FAULT;
        return 0;
    }
    if (lpNumberOfBytesWritten != NULL) *lpNumberOfBytesWritten = (uint32_t) res;
    return 1;
}

uint32_t CCALL SetFilePointer_c(void *hFile, int32_t lDistanceToMove, int32_t *lpDistanceToMoveHigh, uint32_t dwMoveMethod)
{
    off_t res;
    int64_t dist;

    if (handle_type(hFile) != HT_FILE)
    {
        last_error = ERROR_INVALID_HANDLE;
        return 0xffffffff;
    }

    dist = lDistanceToMove;
    if (lpDistanceToMoveHigh != NULL) dist = (int64_t)(((uint64_t)(uint32_t)*lpDistanceToMoveHigh << 32) | (uint32_t)lDistanceToMove);

    res = lseek(((win_file *)hFile)->fd, dist, (dwMoveMethod == 1) ? SEEK_CUR : ((dwMoveMethod == 2) ? SEEK_END : SEEK_SET));
    if (res < 0)
    {
        last_error = ERROR_INVALID_PARAMETER;
        return 0xffffffff;
    }
    if (lpDistanceToMoveHigh != NULL) *lpDistanceToMoveHigh = (int32_t)(res >> 32);
    last_error = 0;
    return (uint32_t) res;
}

uint32_t CCALL SetEndOfFile_c(void *hFile)
{
    off_t pos;
    if (handle_type(hFile) != HT_FILE) return 0;
    pos = lseek(((win_file *)hFile)->fd, 0, SEEK_CUR);
    return (ftruncate(((win_file *)hFile)->fd, pos) == 0) ? 1 : 0;
}

uint32_t CCALL FlushFileBuffers_c(void *hFile)
{
    return (handle_type(hFile) == HT_FILE) ? 1 : 0;
}

uint32_t CCALL GetFileType_c(void *hFile)
{
    // FILE_TYPE_DISK = 1, FILE_TYPE_CHAR = 2
    if (handle_type(hFile) == HT_FILE) return 1;
    if (H2G(hFile) <= 3) return 2;
    return 0;
}

static void unix_to_filetime(time_t t, uint32_t *ft)
{
    uint64_t v = ((uint64_t)t + 11644473600ULL) * 10000000ULL;
    ft[0] = (uint32_t) v;
    ft[1] = (uint32_t)(v >> 32);
}

uint32_t CCALL GetFileTime_c(void *hFile, uint32_t *lpCreationTime, uint32_t *lpLastAccessTime, uint32_t *lpLastWriteTime)
{
    struct stat st;

    if (handle_type(hFile) != HT_FILE) return 0;
    if (fstat(((win_file *)hFile)->fd, &st) != 0) return 0;
    if (lpCreationTime != NULL) unix_to_filetime(st.st_mtime, lpCreationTime);
    if (lpLastAccessTime != NULL) unix_to_filetime(st.st_atime, lpLastAccessTime);
    if (lpLastWriteTime != NULL) unix_to_filetime(st.st_mtime, lpLastWriteTime);
    return 1;
}

void * CCALL GetStdHandle_c(uint32_t nStdHandle)
{
    // STD_INPUT_HANDLE -10, STD_OUTPUT_HANDLE -11, STD_ERROR_HANDLE -12
    switch ((int32_t)nStdHandle)
    {
        case -10: return G2H(1);
        case -11: return G2H(2);
        case -12: return G2H(3);
    }
    return INVALID_HANDLE_VALUE;
}

uint32_t CCALL SetStdHandle_c(uint32_t nStdHandle, void *hHandle) { return 1; }

uint32_t CCALL DeleteFileA_c(const char *lpFileName)
{
    char path[1024];

    if ((lpFileName == NULL) || !vfs_resolve(lpFileName, path, sizeof(path)))
    {
        last_error = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    if (unlink(path) != 0)
    {
        last_error = errno_to_win(errno);
        return 0;
    }
    return 1;
}

uint32_t CCALL CopyFileA_c(const char *lpExistingFileName, const char *lpNewFileName, uint32_t bFailIfExists)
{
    char src[1024], dst[1024], buf[65536];
    int fin, fout;
    ssize_t n;

    if ((lpExistingFileName == NULL) || (lpNewFileName == NULL) || !vfs_resolve(lpExistingFileName, src, sizeof(src)))
    {
        last_error = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    if (vfs_resolve(lpNewFileName, dst, sizeof(dst)) && bFailIfExists)
    {
        last_error = ERROR_FILE_EXISTS;
        return 0;
    }

    fin = open(src, O_RDONLY | O_BINARY);
    if (fin < 0) { last_error = errno_to_win(errno); return 0; }
    fout = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fout < 0) { close(fin); last_error = errno_to_win(errno); return 0; }

    while ((n = read(fin, buf, sizeof(buf))) > 0)
    {
        if (write(fout, buf, n) != n) break;
    }
    close(fin);
    close(fout);
    return 1;
}

uint32_t CCALL SetFileAttributesA_c(const char *lpFileName, uint32_t dwFileAttributes)
{
    char path[1024];
    if ((lpFileName == NULL) || !vfs_resolve(lpFileName, path, sizeof(path))) return 0;
    return 1;
}


/* ------------------------------------------------------------------ */
/* FindFirstFileA                                                      */

#pragma pack(push, 1)
typedef struct {
    uint32_t dwFileAttributes;
    uint32_t ftCreationTime[2];
    uint32_t ftLastAccessTime[2];
    uint32_t ftLastWriteTime[2];
    uint32_t nFileSizeHigh;
    uint32_t nFileSizeLow;
    uint32_t dwReserved0;
    uint32_t dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} win32_find_data;
#pragma pack(pop)

static int find_next(find_handle *h, win32_find_data *data)
{
    struct dirent *entry;
    struct stat st;
    char full[1300];

    while ((entry = readdir(h->dir)) != NULL)
    {
        if (0 != fnmatch(h->pattern, entry->d_name, FNM_CASEFOLD)) continue;

        snprintf(full, sizeof(full), "%s/%s", h->dirpath, entry->d_name);
        if (stat(full, &st) != 0) continue;

        memset(data, 0, sizeof(win32_find_data));
        data->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        unix_to_filetime(st.st_mtime, data->ftCreationTime);
        unix_to_filetime(st.st_atime, data->ftLastAccessTime);
        unix_to_filetime(st.st_mtime, data->ftLastWriteTime);
        data->nFileSizeLow = (uint32_t) st.st_size;
        data->nFileSizeHigh = (uint32_t)((uint64_t)st.st_size >> 32);
        strncpy(data->cFileName, entry->d_name, 259);
        return 1;
    }
    return 0;
}

void * CCALL FindFirstFileA_c(const char *lpFileName, win32_find_data *lpFindFileData)
{
    char spec[1024];
    char *slash;
    find_handle *h;

    if ((lpFileName == NULL) || (lpFindFileData == NULL))
    {
        last_error = ERROR_INVALID_PARAMETER;
        return INVALID_HANDLE_VALUE;
    }

    h = (find_handle *) game_calloc(1, sizeof(find_handle));
    h->type = HT_FIND;

    strncpy(spec, lpFileName, sizeof(spec) - 1);
    spec[sizeof(spec) - 1] = 0;
    slash = strrchr(spec, '\\');
    if (slash == NULL) slash = strrchr(spec, '/');
    if (slash != NULL)
    {
        *slash = 0;
        strncpy(h->pattern, slash + 1, sizeof(h->pattern) - 1);
        vfs_resolve(spec, h->dirpath, sizeof(h->dirpath));
    }
    else
    {
        strncpy(h->pattern, spec, sizeof(h->pattern) - 1);
        strcpy(h->dirpath, ".");
    }
    if (0 == strcmp(h->pattern, "*.*")) strcpy(h->pattern, "*");

    h->dir = opendir(h->dirpath);
    if ((h->dir == NULL) || !find_next(h, lpFindFileData))
    {
        if (winapi_debug) eprintf("FindFirstFileA: %s -> not found\n", lpFileName);
        if (h->dir != NULL) closedir(h->dir);
        game_free(h);
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }

    if (winapi_debug) eprintf("FindFirstFileA: %s -> %s\n", lpFileName, lpFindFileData->cFileName);
    return h;
}

uint32_t CCALL FindNextFileA_c(void *hFindFile, win32_find_data *lpFindFileData)
{
    if (handle_type(hFindFile) != HT_FIND)
    {
        last_error = ERROR_INVALID_HANDLE;
        return 0;
    }
    if (!find_next((find_handle *)hFindFile, lpFindFileData))
    {
        last_error = ERROR_NO_MORE_FILES;
        return 0;
    }
    return 1;
}

uint32_t CCALL FindClose_c(void *hFindFile)
{
    find_handle *h = (find_handle *)hFindFile;

    if (handle_type(hFindFile) != HT_FIND) return 0;
    if (h->dir != NULL) closedir(h->dir);
    game_free(h);
    return 1;
}


/* ------------------------------------------------------------------ */
/* file mapping                                                        */

#define PAGE_READONLY 0x02
#define FILE_MAP_WRITE 0x0002

void * CCALL CreateFileMappingA_c(void *hFile, void *lpAttributes, uint32_t flProtect, uint32_t dwMaximumSizeHigh, uint32_t dwMaximumSizeLow, const char *lpName)
{
    mapping_handle *m;
    struct stat st;

    m = (mapping_handle *) game_calloc(1, sizeof(mapping_handle));
    m->type = HT_MAPPING;
    m->writable = (flProtect != PAGE_READONLY);
    m->fd = -1;

    if (handle_type(hFile) == HT_FILE)
    {
        m->fd = dup(((win_file *)hFile)->fd);
        if (fstat(m->fd, &st) != 0)
        {
            close(m->fd);
            game_free(m);
            return NULL;
        }
        m->size = (dwMaximumSizeLow != 0) ? dwMaximumSizeLow : (uint32_t) st.st_size;
    }
    else
    {
        m->size = dwMaximumSizeLow;
    }

    if (winapi_debug) eprintf("CreateFileMappingA: size %u\n", m->size);
    return m;
}

#define MAX_VIEWS 64
static struct { void *addr; size_t len; } views[MAX_VIEWS];

void * CCALL MapViewOfFile_c(void *hFileMappingObject, uint32_t dwDesiredAccess, uint32_t dwFileOffsetHigh, uint32_t dwFileOffsetLow, uint32_t dwNumberOfBytesToMap)
{
    mapping_handle *m = (mapping_handle *) hFileMappingObject;
    size_t len;
    void *addr;
    int i;

    if (handle_type(m) != HT_MAPPING) return NULL;

    len = (dwNumberOfBytesToMap != 0) ? dwNumberOfBytesToMap : (m->size - dwFileOffsetLow);
    if (len == 0) len = 1;

    if (m->fd >= 0)
    {
        // private mapping: changes are never written back to the file
        addr = sys_map_low(len, m->fd, dwFileOffsetLow);
    }
    else
    {
        addr = sys_map_low(len, -1, 0);
    }
    if (addr == NULL) return NULL;

    for (i = 0; i < MAX_VIEWS; i++)
    {
        if (views[i].addr == NULL)
        {
            views[i].addr = addr;
            views[i].len = len;
            break;
        }
    }
    return addr;
}

uint32_t CCALL UnmapViewOfFile_c(void *lpBaseAddress)
{
    int i;

    for (i = 0; i < MAX_VIEWS; i++)
    {
        if (views[i].addr == lpBaseAddress)
        {
            sys_unmap(views[i].addr, views[i].len);
            views[i].addr = NULL;
            return 1;
        }
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* heaps                                                               */

#define HEAP_ZERO_MEMORY 0x08
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10

typedef struct heap_block {
    struct heap_block *prev, *next;
    uint32_t size;
    struct heap_obj *heap;
    uint32_t magic;     // HEAP_BLOCK_MAGIC while allocated
    uint32_t pad[3];
} heap_block;   // 32 bytes (keeps 16-byte alignment)

#define HEAP_BLOCK_MAGIC 0x48504C42
// slack after every block: small overruns (the game has some) land here instead of in the host malloc's
// metadata - Windows heaps round sizes up and are similarly tolerant
#define HEAP_SLACK 32

// Windows HeapFree/HeapReAlloc/HeapSize fail on invalid blocks instead of aborting;
// the game frees a block twice at exit (WinMain cleanup, dword_504C0C). A magic value in the header isn't
// enough (freed memory keeps it when it's reused without being overwritten), so live blocks are kept in a
// hash set (open addressing, linear probing, tombstones).
#define LIVE_EMPTY ((uintptr_t)0)
#define LIVE_DELETED ((uintptr_t)1)
static uintptr_t *live_set;
static uint32_t live_capacity, live_used;   // used = live + tombstones

static uint32_t live_hash(uintptr_t p)
{
    return (uint32_t)((p >> 4) * 2654435761u);
}

static void live_insert(const heap_block *b);

static void live_grow(void)
{
    uintptr_t *old = live_set;
    uint32_t old_capacity = live_capacity, i;

    live_capacity = live_capacity ? live_capacity * 2 : 65536;
    live_set = (uintptr_t *) calloc(live_capacity, sizeof(uintptr_t));
    live_used = 0;
    for (i = 0; i < old_capacity; i++)
    {
        if (old[i] > LIVE_DELETED) live_insert((const heap_block *) old[i]);
    }
    free(old);
}

static void live_insert(const heap_block *b)
{
    uint32_t i;
    if ((live_used + 1) * 2 > live_capacity) live_grow();
    for (i = live_hash((uintptr_t) b) & (live_capacity - 1); live_set[i] > LIVE_DELETED; i = (i + 1) & (live_capacity - 1)) ;
    if (live_set[i] == LIVE_EMPTY) live_used++;
    live_set[i] = (uintptr_t) b;
}

static int live_find(const heap_block *b)
{
    uint32_t i;
    if (live_capacity == 0) return -1;
    for (i = live_hash((uintptr_t) b) & (live_capacity - 1); live_set[i] != LIVE_EMPTY; i = (i + 1) & (live_capacity - 1))
    {
        if (live_set[i] == (uintptr_t) b) return (int) i;
    }
    return -1;
}

static void live_remove(const heap_block *b)
{
    int i = live_find(b);
    if (i >= 0) live_set[i] = LIVE_DELETED;
}

static int valid_block(const heap_block *b)
{
    return (live_find(b) >= 0) && (b->magic == HEAP_BLOCK_MAGIC);
}

typedef struct heap_obj {
    uint32_t type;
    heap_block *first;
} heap_obj;

// the heap functions are called from several threads (the engine's sound, music and file threads)
#if defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP)
static pthread_mutex_t heap_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t heap_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;     // macOS
#endif
struct heap_lock { heap_lock() { pthread_mutex_lock(&heap_mutex); } ~heap_lock() { pthread_mutex_unlock(&heap_mutex); } };

static heap_obj process_heap_init = { HT_HEAP, NULL };
LOW_SLOT(process_heap)
#define process_heap LOW_OBJ(process_heap)

/* Heap robustness and diagnostics
 * - the slack after every block is filled with a check pattern; overruns (the game writing past a block) are
 *   detected when the block is freed/reallocated, and for all blocks once per frame with AVP2_HEAPCHECK=1
 * - freed blocks are quarantined before their memory is reused, so that writes to freed memory (the game
 *   has double frees, so likely also use-after-free) hit quarantined data instead of live data or the host
 *   allocator's metadata (on Windows such writes usually go unnoticed) */
// zero: the game relies on zero bytes after its blocks (e.g. sub_469B00 parses a loaded text file with
// strpbrk and needs a terminator after the data); an overrun shows up as non-zero bytes
#define HEAP_CANARY 0x00
#define QUARANTINE_COUNT 4096
#define QUARANTINE_BYTES (32 * 1024 * 1024)

#define MAX_HEAPS 64
static heap_obj *heaps[MAX_HEAPS];
static int num_heaps;

static heap_block *quarantine[QUARANTINE_COUNT];
static int quarantine_pos;
static uint32_t quarantine_bytes;

/* AVP2_HEAPGUARD=1 (debugging): every block gets its own pages, ending right before an inaccessible guard
 * page, and freed blocks are made inaccessible (kept for a while) - an overrun or a write to freed memory
 * crashes immediately at the responsible instruction (see the core dump). Uses a lot of memory. */
static int heap_guard = -1;
#define GUARD_ZERO_PAD 16
#define GUARD_KEEP_BYTES (256u * 1024 * 1024)
static uint32_t guard_kept_bytes;
static struct { void *base; size_t len; } guard_kept[65536];
static int guard_kept_pos;

static int use_heap_guard(void)
{
    if (heap_guard < 0) heap_guard = (getenv("AVP2_HEAPGUARD") != NULL);
    return heap_guard;
}

static heap_block *guard_alloc(uint32_t total)
{
    size_t data = ((total + 3) & ~(size_t)3) + GUARD_ZERO_PAD;  // zero pad (terminators), then the guard page
    size_t len = (data + 4095) & ~(size_t)4095;
    uint8_t *base = (uint8_t *) sys_map_low(len + 4096, -1, 0);
    if (base == NULL) return NULL;
    sys_protect_none(base + len, 4096);
    return (heap_block *)(base + len - data);
}

static void guard_free(heap_block *b, uint32_t total)
{
    size_t data = ((total + 3) & ~(size_t)3) + GUARD_ZERO_PAD;  // zero pad (terminators), then the guard page
    size_t len = (data + 4095) & ~(size_t)4095;
    uint8_t *base = (uint8_t *) b + data - len;
    sys_protect_none(base, len);
    // keep the freed pages inaccessible for a while, then release them
    if (guard_kept[guard_kept_pos].base != NULL)
    {
        sys_unmap(guard_kept[guard_kept_pos].base, guard_kept[guard_kept_pos].len);
        guard_kept_bytes -= (uint32_t) guard_kept[guard_kept_pos].len;
    }
    guard_kept[guard_kept_pos].base = base;
    guard_kept[guard_kept_pos].len = len + 4096;
    guard_kept_bytes += (uint32_t)(len + 4096);
    guard_kept_pos = (guard_kept_pos + 1) % 65536;
    (void) GUARD_KEEP_BYTES;
}

static void block_release(heap_block *b);

static void arm_slack(heap_block *b)
{
    if (use_heap_guard()) return;   // no slack in guard mode: the guard page follows the block
    memset((uint8_t *)(b + 1) + b->size, HEAP_CANARY, HEAP_SLACK);
}

#if defined(__i386__) && !defined(__SANITIZE_ADDRESS__)
// 32-bit build: blocks come from glibc's malloc - checks the size field of the malloc chunk header
// (damaged by writes before the block or after the previous one; glibc aborts when it's freed)
static int check_host_chunk(heap_block *b, uint32_t size, const char *where)
{
    uint32_t chunk = ((const uint32_t *)b)[-1] & ~7u;
    if ((chunk < sizeof(heap_block) + size + HEAP_SLACK + 4) || (chunk > sizeof(heap_block) + size + HEAP_SLACK + 4096 + 64 * 1024 * 1024))
    {
        eprintf("heap: malloc chunk header of block %p (size %u) damaged (%s): %08x %08x\n", (void *)(b + 1), size, where,
                ((const uint32_t *)b)[-2], ((const uint32_t *)b)[-1]);
        return 0;
    }
    return 1;
}
#endif

// returns 0 if the block was damaged (and reports it once)
static int check_block(heap_block *b, const char *where)
{
    const uint8_t *slack = (const uint8_t *)(b + 1) + b->size;
    int i;
    if (use_heap_guard()) return 1;
#if defined(__i386__) && !defined(__SANITIZE_ADDRESS__)
    if (!check_host_chunk(b, b->size, where)) return 0;
#endif
    if (b->magic != HEAP_BLOCK_MAGIC)
    {
        eprintf("heap: block header at %p damaged (%s)\n", (void *)(b + 1), where);
        return 0;
    }
    for (i = 0; i < HEAP_SLACK; i++)
    {
        if (slack[i] != HEAP_CANARY)
        {
            eprintf("heap: overrun after block %p (size %u) at +%u (%s): %02x %02x %02x %02x\n", (void *)(b + 1), b->size,
                    b->size + i, where, slack[i], (i + 1 < HEAP_SLACK) ? slack[i + 1] : 0, (i + 2 < HEAP_SLACK) ? slack[i + 2] : 0,
                    (i + 3 < HEAP_SLACK) ? slack[i + 3] : 0);
            arm_slack(b);   // report each overrun once
            return 0;
        }
    }
    return 1;
}

static void quarantine_free(heap_block *b)
{
    if (use_heap_guard())
    {
        guard_free(b, b->size + sizeof(heap_block));
        return;
    }
    block_release(b);
}

static void block_release(heap_block *b)
{
    uint32_t size = b->size + sizeof(heap_block) + HEAP_SLACK;
    heap_block *old;

    // evict until there is room
    while ((quarantine_bytes + size > QUARANTINE_BYTES) || (quarantine[quarantine_pos] != NULL))
    {
        old = quarantine[quarantine_pos];
        if (old != NULL)
        {
            quarantine_bytes -= old->size + sizeof(heap_block) + HEAP_SLACK;
            x86_free(old);
            quarantine[quarantine_pos] = NULL;
            if (quarantine_bytes + size <= QUARANTINE_BYTES) break;
        }
        quarantine_pos = (quarantine_pos + 1) % QUARANTINE_COUNT;
        if (quarantine_bytes == 0) break;
    }
    if (size > QUARANTINE_BYTES)
    {
        x86_free(b);
        return;
    }
    quarantine[quarantine_pos] = b;
    quarantine_bytes += size;
    quarantine_pos = (quarantine_pos + 1) % QUARANTINE_COUNT;
}

void heap_check_all(void)
{
    static int enabled = -1;
    int i;
    heap_block *b;

    static uint32_t last;
    uint32_t now;

    if (enabled < 0) enabled = (getenv("AVP2_HEAPCHECK") != NULL);
    if (!enabled) return;
    now = winapi_get_ticks();
    if (now - last < 100) return;
    last = now;
    for (i = -1; i < num_heaps; i++)
    {
        heap_obj *h = (i < 0) ? &process_heap : heaps[i];
        if (h == NULL) continue;
        for (b = h->first; b != NULL; b = b->next) check_block(b, "periodic check");
    }
#if defined(__i386__) && !defined(__SANITIZE_ADDRESS__)
    if (!use_heap_guard())
    {
        static int reported;
        for (i = 0; i < QUARANTINE_COUNT; i++)
        {
            if ((quarantine[i] != NULL) && !reported && !check_host_chunk(quarantine[i], quarantine[i]->size, "periodic check, freed block")) reported = 1;
        }
    }
#endif
}

void * CCALL GetProcessHeap_c(void) { return &process_heap; }

void * CCALL HeapCreate_c(uint32_t flOptions, uint32_t dwInitialSize, uint32_t dwMaximumSize)
{
    heap_obj *h = (heap_obj *) x86_calloc(1, sizeof(heap_obj));
    int i;
    h->type = HT_HEAP;
    for (i = 0; i < MAX_HEAPS; i++) if (heaps[i] == NULL) { heaps[i] = h; if (i >= num_heaps) num_heaps = i + 1; break; }
    return h;
}

uint32_t CCALL HeapDestroy_c(heap_obj *hHeap)
{
    heap_block *b, *next;

    if (handle_type(hHeap) != HT_HEAP) return 0;
    for (b = hHeap->first; b != NULL; b = next)
    {
        next = b->next;
        check_block(b, "HeapDestroy");
        live_remove(b);
        b->magic = 0;
        quarantine_free(b);
    }
    hHeap->first = NULL;
    if (hHeap != &process_heap)
    {
        int i;
        for (i = 0; i < num_heaps; i++) if (heaps[i] == hHeap) heaps[i] = NULL;
        x86_free(hHeap);
    }
    return 1;
}

void * CCALL HeapAlloc_c(heap_obj *hHeap, uint32_t dwFlags, uint32_t dwBytes)
{
    heap_lock lock;
    heap_block *b;

    if (handle_type(hHeap) != HT_HEAP) hHeap = &process_heap;

    b = use_heap_guard() ? guard_alloc(sizeof(heap_block) + dwBytes) : (heap_block *) x86_malloc(sizeof(heap_block) + dwBytes + HEAP_SLACK);
    if (b == NULL) return NULL;
    // always zero: the game uses uninitialised fields of HeapAlloc'ed structs (e.g. texture
    // animation descriptors in sub_449xxx) which happen to be zero on a fresh Windows heap
    memset(b + 1, 0, dwBytes);

    b->size = dwBytes;
    arm_slack(b);
    b->magic = HEAP_BLOCK_MAGIC;
    live_insert(b);
    b->heap = hHeap;
    b->prev = NULL;
    b->next = hHeap->first;
    if (b->next != NULL) b->next->prev = b;
    hHeap->first = b;
    return b + 1;
}

static void unlink_block(heap_block *b)
{
    if (b->prev != NULL) b->prev->next = b->next; else b->heap->first = b->next;
    if (b->next != NULL) b->next->prev = b->prev;
}

uint32_t CCALL HeapFree_c(heap_obj *hHeap, uint32_t dwFlags, void *lpMem)
{
    heap_lock lock;
    heap_block *b;

    if (lpMem == NULL) return 1;
    b = ((heap_block *)lpMem) - 1;
    if (!valid_block(b))
    {
        if (winapi_debug) eprintf("HeapFree: invalid block %p\n", lpMem);
        return 0;
    }
    check_block(b, "HeapFree");
    b->magic = 0;
    live_remove(b);
    unlink_block(b);
    quarantine_free(b);
    return 1;
}

void * CCALL HeapReAlloc_c(heap_obj *hHeap, uint32_t dwFlags, void *lpMem, uint32_t dwBytes)
{
    heap_lock lock;
    heap_block *b, *nb;
    uint32_t oldsize;

    if (lpMem == NULL) return NULL;
    b = ((heap_block *)lpMem) - 1;
    if (!valid_block(b)) return NULL;
    check_block(b, "HeapReAlloc");
    oldsize = b->size;

    if (dwFlags & HEAP_REALLOC_IN_PLACE_ONLY)
    {
        if (dwBytes > oldsize) return NULL;
        b->size = dwBytes;
        arm_slack(b);
        return lpMem;
    }

    // new block + copy; the old block goes to the quarantine (stale pointers to it stay harmless)
    nb = use_heap_guard() ? guard_alloc(sizeof(heap_block) + dwBytes) : (heap_block *) x86_malloc(sizeof(heap_block) + dwBytes + HEAP_SLACK);
    if (nb == NULL) return NULL;
    memcpy(nb + 1, b + 1, (dwBytes < oldsize) ? dwBytes : oldsize);
    if (dwBytes > oldsize) memset((uint8_t *)(nb + 1) + oldsize, 0, dwBytes - oldsize);
    nb->size = dwBytes;
    nb->magic = HEAP_BLOCK_MAGIC;
    nb->heap = b->heap;
    arm_slack(nb);
    live_insert(nb);
    nb->prev = NULL;
    nb->next = nb->heap->first;
    if (nb->next != NULL) nb->next->prev = nb;
    nb->heap->first = nb;

    b->magic = 0;
    live_remove(b);
    unlink_block(b);
    quarantine_free(b);
    return nb + 1;
}

// GlobalAlloc & co. (ANETDLL, WINET): fixed memory from the process heap (GMEM_MOVEABLE handles are the pointers)
void * CCALL GlobalAlloc_c(uint32_t uFlags, uint32_t dwBytes)
{
    return HeapAlloc_c(&process_heap, HEAP_ZERO_MEMORY, dwBytes ? dwBytes : 1);
}

void * CCALL GlobalReAlloc_c(void *hMem, uint32_t dwBytes, uint32_t uFlags)
{
    if (hMem == NULL) return GlobalAlloc_c(uFlags, dwBytes);
    return HeapReAlloc_c(&process_heap, HEAP_ZERO_MEMORY, hMem, dwBytes ? dwBytes : 1);
}

void * CCALL GlobalFree_c(void *hMem)
{
    if (hMem == NULL) return NULL;
    return HeapFree_c(&process_heap, 0, hMem) ? NULL : hMem;
}

/* Memory the runtime hands to the game (COM objects, surfaces, sound buffers, handles, Smacker objects...):
 * allocated from a private emulated Win32 heap, so it gets the same protection as the game's own heap blocks
 * (quarantine after free, overrun detection, AVP2_HEAPGUARD) - in the 32-bit build x86_malloc is glibc's malloc,
 * and stray game writes into freed runtime memory corrupted glibc (crashes noticed later in the GL driver). */
static heap_obj runtime_heap = { HT_HEAP, NULL };

void *low_object(void **slot, const void *init, size_t size)
{
    if (*slot == NULL)
    {
        void *p = game_malloc((uint32_t) size);
        memcpy(p, init, size);
        *slot = p;
    }
    return *slot;
}

void *game_malloc(uint32_t size) { return HeapAlloc_c(&runtime_heap, 0, size); }     // HeapAlloc zeroes
void *game_calloc(uint32_t n, uint32_t size) { return HeapAlloc_c(&runtime_heap, 0, n * size); }
void game_free(void *p) { if (p != NULL) HeapFree_c(&runtime_heap, 0, p); }

uint32_t CCALL HeapSize_c(heap_obj *hHeap, uint32_t dwFlags, const void *lpMem)
{
    heap_lock lock;
    if ((lpMem == NULL) || !valid_block(((const heap_block *)lpMem) - 1)) return (uint32_t)-1;  // valid_block checks the set first
    return (((const heap_block *)lpMem) - 1)->size;
}

uint32_t CCALL HeapCompact_c(heap_obj *hHeap, uint32_t dwFlags)
{
    return 64 * 1024 * 1024;
}


/* ------------------------------------------------------------------ */
/* virtual memory                                                      */

#define MEM_COMMIT  0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000

#define MAX_REGIONS 256
static struct { uint8_t *addr; uint32_t size; } regions[MAX_REGIONS];

void * CCALL VirtualAlloc_c(void *lpAddress, uint32_t dwSize, uint32_t flAllocationType, uint32_t flProtect)
{
    void *addr;
    uint32_t size;
    int i;

    if (lpAddress != NULL)
    {
        // commit inside an already reserved region (regions are always fully accessible)
        for (i = 0; i < MAX_REGIONS; i++)
        {
            if ((regions[i].addr != NULL) && ((uint8_t *)lpAddress >= regions[i].addr) && ((uint8_t *)lpAddress < regions[i].addr + regions[i].size))
            {
                return (void *)((uintptr_t)lpAddress & ~(uintptr_t)4095);
            }
        }
        eprintf("VirtualAlloc: unsupported fixed address 0x%x\n", H2G(lpAddress));
        return NULL;
    }

    size = (dwSize + 65535) & ~65535u;
    addr = sys_map_low(size, -1, 0);
    if (addr == NULL)
    {
        last_error = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }

    for (i = 0; i < MAX_REGIONS; i++)
    {
        if (regions[i].addr == NULL)
        {
            regions[i].addr = (uint8_t *) addr;
            regions[i].size = size;
            break;
        }
    }
    if (winapi_debug) eprintf("VirtualAlloc: %u bytes -> 0x%x\n", dwSize, H2G(addr));
    return addr;
}

uint32_t CCALL VirtualFree_c(void *lpAddress, uint32_t dwSize, uint32_t dwFreeType)
{
    int i;

    if (!(dwFreeType & MEM_RELEASE)) return 1; // decommit: keep the memory

    for (i = 0; i < MAX_REGIONS; i++)
    {
        if (regions[i].addr == lpAddress)
        {
            sys_unmap(regions[i].addr, regions[i].size);
            regions[i].addr = NULL;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    uint32_t BaseAddress;
    uint32_t AllocationBase;
    uint32_t AllocationProtect;
    uint32_t RegionSize;
    uint32_t State;
    uint32_t Protect;
    uint32_t Type;
} memory_basic_information;

uint32_t CCALL VirtualQuery_c(const void *lpAddress, memory_basic_information *lpBuffer, uint32_t dwLength)
{
    int i;

    if ((lpBuffer == NULL) || (dwLength < sizeof(memory_basic_information))) return 0;
    memset(lpBuffer, 0, sizeof(memory_basic_information));

    for (i = 0; i < MAX_REGIONS; i++)
    {
        if ((regions[i].addr != NULL) && ((const uint8_t *)lpAddress >= regions[i].addr) && ((const uint8_t *)lpAddress < regions[i].addr + regions[i].size))
        {
            lpBuffer->BaseAddress = H2G(lpAddress) & ~4095u;
            lpBuffer->AllocationBase = H2G(regions[i].addr);
            lpBuffer->AllocationProtect = 0x04;
            lpBuffer->RegionSize = H2G(regions[i].addr + regions[i].size) - lpBuffer->BaseAddress;
            lpBuffer->State = MEM_COMMIT;
            lpBuffer->Protect = 0x04; // PAGE_READWRITE
            lpBuffer->Type = 0x20000; // MEM_PRIVATE
            return sizeof(memory_basic_information);
        }
    }

    // any other address: report committed read/write memory
    lpBuffer->BaseAddress = H2G(lpAddress) & ~4095u;
    lpBuffer->AllocationBase = lpBuffer->BaseAddress;
    lpBuffer->AllocationProtect = 0x04;
    lpBuffer->RegionSize = 4096;
    lpBuffer->State = MEM_COMMIT;
    lpBuffer->Protect = 0x04;
    lpBuffer->Type = 0x20000;
    return sizeof(memory_basic_information);
}


/* ------------------------------------------------------------------ */
/* system information                                                  */

typedef struct {
    uint16_t wProcessorArchitecture;
    uint16_t wReserved;
    uint32_t dwPageSize;
    uint32_t lpMinimumApplicationAddress;
    uint32_t lpMaximumApplicationAddress;
    uint32_t dwActiveProcessorMask;
    uint32_t dwNumberOfProcessors;
    uint32_t dwProcessorType;
    uint32_t dwAllocationGranularity;
    uint16_t wProcessorLevel;
    uint16_t wProcessorRevision;
} system_info;

void CCALL GetSystemInfo_c(system_info *lpSystemInfo)
{
    if (lpSystemInfo == NULL) return;
    memset(lpSystemInfo, 0, sizeof(system_info));
    lpSystemInfo->dwPageSize = 4096;
    lpSystemInfo->lpMinimumApplicationAddress = 0x10000;
    lpSystemInfo->lpMaximumApplicationAddress = 0x7ffeffff;
    lpSystemInfo->dwActiveProcessorMask = 1;
    lpSystemInfo->dwNumberOfProcessors = 1;
    lpSystemInfo->dwProcessorType = 586;
    lpSystemInfo->dwAllocationGranularity = 65536;
    lpSystemInfo->wProcessorLevel = 5;
    lpSystemInfo->wProcessorRevision = 0x0201;
}

static struct timespec start_time;
static int start_time_set;

uint32_t winapi_get_ticks(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!start_time_set)
    {
        start_time = now;
        start_time_set = 1;
    }
    return (uint32_t)((now.tv_sec - start_time.tv_sec) * 1000 + (now.tv_nsec - start_time.tv_nsec) / 1000000);
}

uint32_t CCALL GetTickCount_c(void) { return winapi_get_ticks(); }

uint32_t CCALL GetCurrentThreadId_c(void);

void CCALL Sleep_c(uint32_t dwMilliseconds)
{
    struct timespec ts;

    winapi_process_events();
    if (winapi_debug && (dwMilliseconds >= 50)) eprintf("Sleep: thread %u sleeps %u ms, caller %08x\n", GetCurrentThreadId_c(), dwMilliseconds, game_caller());
    ts.tv_sec = dwMilliseconds / 1000;
    ts.tv_nsec = (dwMilliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

void * CCALL GetCurrentProcess_c(void) { return &process_handle_obj; }
uint32_t CCALL SetPriorityClass_c(void *hProcess, uint32_t dwPriorityClass) { return 1; }
uint32_t CCALL GetSystemDefaultLCID_c(void) { return 0x0409; } // en-US

void CCALL OutputDebugStringA_c(const char *lpOutputString)
{
    if (winapi_debug && (lpOutputString != NULL)) eprintf("OutputDebugString: %s", lpOutputString);
}

char * CCALL lstrcatA_c(char *lpString1, const char *lpString2) { return (lpString1 && lpString2) ? strcat(lpString1, lpString2) : NULL; }
char * CCALL lstrcpyA_c(char *lpString1, const char *lpString2) { return (lpString1 && lpString2) ? strcpy(lpString1, lpString2) : NULL; }

uint32_t CCALL GetCurrentDirectoryA_c(uint32_t nBufferLength, char *lpBuffer)
{
    uint32_t len = strlen(VFS_GAME_DIR);

    if ((lpBuffer == NULL) || (nBufferLength <= len)) return len + 1;
    strcpy(lpBuffer, VFS_GAME_DIR);
    return len;
}

uint32_t CCALL GetWindowsDirectoryA_c(char *lpBuffer, uint32_t uSize)
{
    const char *dir = "C:\\WINDOWS";
    if ((lpBuffer == NULL) || (uSize <= strlen(dir))) return strlen(dir) + 1;
    strcpy(lpBuffer, dir);
    return strlen(dir);
}

uint32_t CCALL GetModuleFileNameA_c(void *hModule, char *lpFilename, uint32_t nSize)
{
    const char *name = VFS_GAME_DIR "\\lithtech.exe";
    if ((lpFilename == NULL) || (nSize == 0)) return 0;
    strncpy(lpFilename, name, nSize - 1);
    lpFilename[nSize - 1] = 0;
    return strlen(lpFilename);
}

// drives: C: = fixed disk; D: = CD-ROM drive. Both map to the game directory.
// AVP2_CD selects what the CD drive contains:
//   unset/2 = an audio CD (label "AUDIO_CD"): the game needs a CD-ROM drive to start the CD music
//             (played from music/*.mp3 by cdaudio.c, like GOG's win32.dll)
//   1       = the game CD (volume "AVP2_CD2"); with it the shell takes other code paths
//             (e.g. TRAINING starts the game directly)
//   0       = no CD drive (no music)
#define CD_LABEL "AVP2_CD2"

static int cd_mode(void)
{
    static int mode = -1;
    if (mode == -1) mode = config_get_int("cd", 2);
    return mode;
}

static int cd_drive(void)
{
    return (cd_mode() != 0) ? 'd' : 0;
}
#define CD_DRIVE cd_drive()

uint32_t CCALL GetLogicalDrives_c(void) { return (1u << 2) | (CD_DRIVE ? (1u << 3) : 0); }

uint32_t CCALL GetLogicalDriveStringsA_c(uint32_t nBufferLength, char *lpBuffer)
{
    if (!CD_DRIVE)
    {
        if ((lpBuffer == NULL) || (nBufferLength < 5)) return 5;
        memcpy(lpBuffer, "C:\\\0\0", 5);
        return 4;
    }
    if ((lpBuffer == NULL) || (nBufferLength < 9)) return 9;
    memcpy(lpBuffer, "C:\\\0D:\\\0\0", 9);
    return 8;
}

uint32_t CCALL GetDriveTypeA_c(const char *lpRootPathName)
{
    // DRIVE_NO_ROOT_DIR 1, DRIVE_FIXED 3, DRIVE_CDROM 5
    if ((lpRootPathName == NULL) || ((lpRootPathName[0] | 0x20) == 'c')) return 3;
    if (CD_DRIVE && ((lpRootPathName[0] | 0x20) == CD_DRIVE)) return 5;
    return 1;
}

uint32_t CCALL GetVolumeInformationA_c(const char *lpRootPathName, char *lpVolumeNameBuffer, uint32_t nVolumeNameSize, uint32_t *lpVolumeSerialNumber, uint32_t *lpMaximumComponentLength, uint32_t *lpFileSystemFlags, char *lpFileSystemNameBuffer, uint32_t nFileSystemNameSize)
{
    int cd;

    cd = CD_DRIVE && (lpRootPathName != NULL) && ((lpRootPathName[0] | 0x20) == CD_DRIVE);
    if ((lpRootPathName != NULL) && ((lpRootPathName[0] | 0x20) != 'c') && !cd)
    {
        last_error = 21; // ERROR_NOT_READY
        return 0;
    }
    if ((lpVolumeNameBuffer != NULL) && (nVolumeNameSize > 0))
    {
        strncpy(lpVolumeNameBuffer, cd ? ((cd_mode() == 1) ? CD_LABEL : "AUDIO_CD") : "", nVolumeNameSize - 1);
        lpVolumeNameBuffer[nVolumeNameSize - 1] = 0;
    }
    if (lpVolumeSerialNumber != NULL) *lpVolumeSerialNumber = cd ? 0x07601998 : 0x12345678;
    if (lpMaximumComponentLength != NULL) *lpMaximumComponentLength = 255;
    if (lpFileSystemFlags != NULL) *lpFileSystemFlags = 0;
    if ((lpFileSystemNameBuffer != NULL) && (nFileSystemNameSize >= 6)) strcpy(lpFileSystemNameBuffer, cd ? "CDFS" : "FAT");
    return 1;
}


/* ------------------------------------------------------------------ */
/* synchronization (the game is single threaded)                       */



void CCALL ExitProcess_c(uint32_t uExitCode)
{
    app_exit(uExitCode);
}

uint32_t CCALL TerminateProcess_c(void *hProcess, uint32_t uExitCode)
{
    if (hProcess == &process_handle_obj) ExitProcess_c(uExitCode);
    return 0;
}


/* ------------------------------------------------------------------ */
/* code pages (single byte, ASCII)                                     */

uint32_t CCALL GetCPInfo_c(uint32_t CodePage, uint8_t *lpCPInfo)
{
    if (lpCPInfo == NULL) return 0;
    memset(lpCPInfo, 0, 20);
    *(uint32_t *)lpCPInfo = 1;   // MaxCharSize
    lpCPInfo[4] = '?';           // DefaultChar
    return 1;
}

int32_t CCALL MultiByteToWideChar_c(uint32_t CodePage, uint32_t dwFlags, const char *lpMultiByteStr, int32_t cbMultiByte, uint16_t *lpWideCharStr, int32_t cchWideChar)
{
    int32_t i, len;

    if (lpMultiByteStr == NULL) return 0;
    len = (cbMultiByte < 0) ? (int32_t)strlen(lpMultiByteStr) + 1 : cbMultiByte;
    if (cchWideChar == 0) return len;
    if (len > cchWideChar) len = cchWideChar;
    for (i = 0; i < len; i++) lpWideCharStr[i] = (uint8_t)lpMultiByteStr[i];
    return len;
}

int32_t CCALL WideCharToMultiByte_c(uint32_t CodePage, uint32_t dwFlags, const uint16_t *lpWideCharStr, int32_t cchWideChar, char *lpMultiByteStr, int32_t cbMultiByte, const char *lpDefaultChar, uint32_t *lpUsedDefaultChar)
{
    int32_t i, len;

    if (lpWideCharStr == NULL) return 0;
    if (cchWideChar < 0) { for (len = 0; lpWideCharStr[len]; len++); len++; } else len = cchWideChar;
    if (cbMultiByte == 0) return len;
    if (len > cbMultiByte) len = cbMultiByte;
    for (i = 0; i < len; i++) lpMultiByteStr[i] = (lpWideCharStr[i] < 256) ? (char)lpWideCharStr[i] : '?';
    return len;
}


/* ------------------------------------------------------------------ */
/* modules (recompiled DLLs are linked in)                             */

#ifdef __cplusplus
extern "C" {
#endif
#ifdef __cplusplus
}
#endif

typedef struct {
    const char *name;
    void *address;
} module_export;

typedef struct {
    uint32_t type;      // not a handle type - modules are identified by address
    const char *name;
    const module_export *exports;
    void *entry;        // the DLL's entry point (its C runtime's _DllMainCRTStartup), recompiled
    int loaded;
    uint8_t *image;     // the DLL file mapped as data (its resources)
    // the DLL's data section (initialized data and bss: <module>_data_start/end, global_aliases.sci) and a copy of it
    // from before its first DllMain: put back when the DLL is loaded again after FreeLibrary
    uint8_t *data_start, *data_end, *data_initial;
    int refs;           // LoadLibrary - FreeLibrary
} module_info;

// recompiled game DLLs (stand-ins until the native builds from Monolith's source replace them)

static const module_export cshell_exports[] = {
    { "GetClientShellFunctions", (void *) cshell_GetClientShellFunctions },
    { "GetClientShellVersion", (void *) cshell_GetClientShellVersion },
    { "SetInstanceHandle", (void *) cshell_SetInstanceHandle },
    { NULL, NULL }
};


static const module_export object_exports[] = {
    { "GetServerShellFunctions", (void *) object_GetServerShellFunctions },
    { "GetServerShellVersion", (void *) object_GetServerShellVersion },
    { "ObjectDLLSetup", (void *) object_ObjectDLLSetup },
    { "SetInstanceHandle", (void *) object_SetInstanceHandle },
    { NULL, NULL }
};

// the engine's server (recompiled; lithtech.exe loads it for local and hosted games)

static const module_export server_exports[] = {
    { "CreateServer", (void *) server_CreateServer },
    { "DeleteServer", (void *) server_DeleteServer },
    { NULL, NULL }
};

// the renderer (render.c): native, replaces d3d.ren

static const module_export render_exports[] = {
    { "RenderDLLSetup", (void *) RenderDLLSetup_asm2c },
    { "GetSupportedModes", (void *) GetSupportedModes_asm2c },
    { "FreeModeList", (void *) FreeModeList_asm2c },
    { NULL, NULL }
};


// binkw32.dll (bink.c): the engine's BinkVideoMgr asks for these by their decorated names
static const module_export bink_exports[] = {
    { "_BinkOpen@8", (void *) BinkOpen_asm2c },
    { "_BinkClose@4", (void *) BinkClose_asm2c },
    { "_BinkWait@4", (void *) BinkWait_asm2c },
    { "_BinkCopyToBuffer@28", (void *) BinkCopyToBuffer_asm2c },
    { "_BinkDoFrame@4", (void *) BinkDoFrame_asm2c },
    { "_BinkNextFrame@4", (void *) BinkNextFrame_asm2c },
    { "_BinkSetSoundSystem@8", (void *) BinkSetSoundSystem_asm2c },
    { "_BinkOpenMiles@4", (void *) BinkOpenMiles_asm2c },
    { "_BinkOpenDirectSound@4", (void *) BinkOpenDirectSound_asm2c },
    { "_BinkSetSoundTrack@4", (void *) BinkSetSoundTrack_asm2c },
    { "_BinkIsSoftwareCursor@8", (void *) BinkIsSoftwareCursor_asm2c },
    { "_BinkCheckCursor@20", (void *) BinkCheckCursor_asm2c },
    { "_BinkRestoreCursor@4", (void *) BinkRestoreCursor_asm2c },
    { NULL, NULL }
};

// AvP2's modules: the engine loads the game DLLs from temporary copies (any path, matched by name)
// the game sees the modules' addresses (HMODULE): LOW_OBJ
// (the engine frees object.lto and server.dll when a game ends - Abort Game, a saved game loaded from the menu - and
// loads them again for the next one: as on Windows, they start again from their data as in the file)
static module_info modules_init[] = {
    { 0, "cshell.dll", cshell_exports, (void *) cshell_DllMainCRTStartup, 0, NULL, (uint8_t *) cshell_data_start, (uint8_t *) cshell_data_end, NULL, 0 },
    { 0, "object.lto", object_exports, (void *) object_DllMainCRTStartup, 0, NULL, (uint8_t *) object_data_start, (uint8_t *) object_data_end, NULL, 0 },
    { 0, "server.dll", server_exports, (void *) server_DllMainCRTStartup, 0, NULL, (uint8_t *) server_data_start, (uint8_t *) server_data_end, NULL, 0 },
    { 0, "d3d.ren", render_exports, NULL, 0, NULL, NULL, NULL, NULL, 0 },
    { 0, "binkw32.dll", bink_exports, NULL, 0, NULL, NULL, NULL, NULL, 0 },
};

LOW_SLOT(modules)
#define modules LOW_OBJ(modules)

static module_info *find_module(const char *name)
{
    const char *base;
    unsigned int i;

    base = strrchr(name, '\\');
    if (base == NULL) base = strrchr(name, '/');
    base = (base != NULL) ? base + 1 : name;

    for (i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
    {
        if ((modules[i].name != NULL) && (0 == strcasecmp(base, modules[i].name))) return &modules[i];
    }
    return NULL;
}

void * CCALL LoadLibraryA_c(const char *lpLibFileName)
{
    module_info *m;

    if (lpLibFileName == NULL) return NULL;

    m = find_module(lpLibFileName);
    if (m == NULL)
    {
        // not a recompiled module: a DLL used for its resources (ltmsg.dll, ...)
        uint8_t *image = pe_image_find(lpLibFileName);
        if (image == NULL) image = pe_image_load(lpLibFileName, 0);
        if (winapi_debug) eprintf("LoadLibraryA: %s -> %s\n", lpLibFileName, (image != NULL) ? "resources" : "not available");
        if (image == NULL) last_error = 126; // ERROR_MOD_NOT_FOUND
        return image;
    }
    if (winapi_debug) eprintf("LoadLibraryA: %s -> recompiled module\n", lpLibFileName);

    m->refs++;
    if (!m->loaded)
    {
        m->loaded = 1;
        // its resources (the code is the recompiled one)
        if (m->image == NULL) m->image = pe_image_load(lpLibFileName, 0);
        // its data as Windows maps it: as in the file, also when it's loaded again (the last game's globals - pointers
        // to its freed objects - made the next game crash: Continue Game after Abort Game)
        if ((m->data_start != NULL) && (m->data_end > m->data_start))
        {
            size_t n = (size_t)(m->data_end - m->data_start);
            if (m->data_initial == NULL)
            {
                m->data_initial = (uint8_t *) malloc(n);
                if (m->data_initial != NULL) memcpy(m->data_initial, m->data_start, n);
            }
            else
            {
                memcpy(m->data_start, m->data_initial, n);
                if (winapi_debug) eprintf("LoadLibraryA: %s loaded again: its data (%zu bytes) as in the file\n", m->name, n);
            }
        }
        // DllMain(hModule, DLL_PROCESS_ATTACH, NULL): the DLL's C runtime initializes itself and runs its C++
        // static constructors
        if (m->entry != NULL)
        {
            uint32_t args[3] = { H2G(m), 1, 0 };
            if (call_game(H2G(m->entry), 3, args) == 0)
            {
                eprintf("LoadLibraryA: %s: DllMain failed\n", m->name);
                m->loaded = 0;
                m->refs--;
                last_error = 1114;  // ERROR_DLL_INIT_FAILED
                return NULL;
            }
        }
    }
    return m;
}

void * CCALL GetModuleHandleA_c(const char *lpModuleName)
{
    if (lpModuleName == NULL) return (void *)lithtech_image;  // main.c: lithtech.exe's headers and resources
    if (find_module(lpModuleName) != NULL) return find_module(lpModuleName);
    return pe_image_find(lpModuleName);
}

void * CCALL GetProcAddress_c(module_info *hModule, const char *lpProcName)
{
    const module_export *e;
    unsigned int i;

    for (i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
    {
        if (hModule == &modules[i]) break;
    }
    if ((i == sizeof(modules) / sizeof(modules[0])) || (H2G(lpProcName) < 0x10000))
    {
        if (winapi_debug && (hModule != NULL)) eprintf("GetProcAddress: %s in a data-only module\n", (H2G(lpProcName) < 0x10000) ? "(ordinal)" : lpProcName);
        return NULL;
    }

    for (e = hModule->exports; e->name != NULL; e++)
    {
        if (0 == strcmp(e->name, lpProcName)) return camera_hook_export(lpProcName, e->address);
    }
    if (winapi_debug) eprintf("GetProcAddress: %s!%s not found\n", hModule->name, lpProcName);
    return NULL;
}

// a game DLL's last FreeLibrary: DllMain(DLL_PROCESS_DETACH) (its C runtime runs the static destructors and frees its
// heap), and the next LoadLibrary starts it again from its data as in the file. DLLs used for their resources stay.
uint32_t CCALL FreeLibrary_c(void *hLibModule)
{
    module_info *m = NULL;
    unsigned int i;

    for (i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
    {
        if (hLibModule == &modules[i]) m = &modules[i];
    }
    if (m == NULL) return 1;
    if (m->refs > 0) m->refs--;
    if (winapi_debug) eprintf("FreeLibrary: %s (%d references left)\n", m->name, m->refs);
    if ((m->refs == 0) && m->loaded && (m->entry != NULL) && (m->data_initial != NULL))
    {
        uint32_t args[3] = { H2G(m), 0, 0 };
        call_game(H2G(m->entry), 3, args);
        m->loaded = 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* command line                                                        */

static char command_line_init[1024];
LOW_SLOT(command_line)
#define command_line LOW_OBJ(command_line)

void winapi_set_command_line(const char *cmdline)
{
    snprintf(command_line, sizeof(command_line), "%s", cmdline);
}

char * CCALL GetCommandLineA_c(void)
{
    return command_line;
}

/* ------------------------------------------------------------------ */
/* version, environment                                                */

// Windows XP SP3 (5.1.2600, NT platform): what the engine and the Sierra updater checked against
uint32_t CCALL GetVersion_c(void)
{
    return (2600u << 16) | (1 << 8) | 5;
}

typedef struct {
    uint32_t dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    char szCSDVersion[128];
} OSVERSIONINFOA_32;

uint32_t CCALL GetVersionExA_c(OSVERSIONINFOA_32 *info)
{
    if ((info == NULL) || (info->dwOSVersionInfoSize < sizeof(OSVERSIONINFOA_32))) { last_error = 87; return 0; }
    info->dwMajorVersion = 5;
    info->dwMinorVersion = 1;
    info->dwBuildNumber = 2600;
    info->dwPlatformId = 2;     // VER_PLATFORM_WIN32_NT
    strcpy(info->szCSDVersion, "Service Pack 3");
    return 1;
}

// the game's environment is empty (variables of the host don't apply)
uint32_t CCALL GetEnvironmentVariableA_c(const char *lpName, char *lpBuffer, uint32_t nSize)
{
    last_error = 203;   // ERROR_ENVVAR_NOT_FOUND
    return 0;
}

/* ------------------------------------------------------------------ */
/* AvP2: the statically linked C runtime's startup, time, locale       */


typedef struct {
    uint32_t cb;
    uint32_t lpReserved, lpDesktop, lpTitle, dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    uint16_t wShowWindow, cbReserved2;
    uint32_t lpReserved2, hStdInput, hStdOutput, hStdError;
} STARTUPINFOA_32;

void CCALL GetStartupInfoA_c(STARTUPINFOA_32 *si)
{
    memset(si, 0, sizeof(*si));
    si->cb = sizeof(*si);
}

uint32_t CCALL SetHandleCount_c(uint32_t uNumber) { return uNumber; }

// an empty environment block (two terminators)
static char env_block_init[4];
static uint16_t env_block_w_init[4];
LOW_SLOT(env_block) LOW_SLOT(env_block_w)
#define env_block LOW_OBJ(env_block)
#define env_block_w LOW_OBJ(env_block_w)
char * CCALL GetEnvironmentStrings_c(void) { return env_block; }
uint16_t * CCALL GetEnvironmentStringsW_c(void) { return env_block_w; }
uint32_t CCALL FreeEnvironmentStringsA_c(char *p) { return 1; }
uint32_t CCALL FreeEnvironmentStringsW_c(uint16_t *p) { return 1; }
uint32_t CCALL SetEnvironmentVariableA_c(const char *lpName, const char *lpValue) { return 1; }

uint32_t CCALL GetACP_c(void) { return 1252; }
uint32_t CCALL GetOEMCP_c(void) { return 437; }

// C1_* character types of Windows-1252 (ASCII part; other characters: alpha)
static uint16_t char_type(unsigned c)
{
    uint16_t t = 0;
    if (c < 0x20 || c == 0x7f) t |= 0x20;                                       // C1_CNTRL
    if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x08;                             // C1_SPACE
    if (c == ' ' || c == 9) t |= 0x40;                                          // C1_BLANK
    if (c >= '0' && c <= '9') t |= 0x04;                                        // C1_DIGIT
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) t |= 0x80;   // C1_XDIGIT
    if (c >= 'A' && c <= 'Z') t |= 0x100 | 0x01;                                // C1_ALPHA | C1_UPPER
    if (c >= 'a' && c <= 'z') t |= 0x100 | 0x02;                                // C1_ALPHA | C1_LOWER
    if (c >= 0x21 && c <= 0x7e && !(t & (0x04 | 0x100))) t |= 0x10;             // C1_PUNCT
    if (c >= 0xc0 && c != 0xd7 && c != 0xf7) t |= 0x100 | ((c < 0xdf) ? 0x01 : 0x02);
    return t;
}

uint32_t CCALL GetStringTypeA_c(uint32_t Locale, uint32_t dwInfoType, const char *lpSrcStr, int32_t cchSrc, uint16_t *lpCharType)
{
    int32_t i;
    if (cchSrc < 0) cchSrc = (int32_t)strlen(lpSrcStr) + 1;
    for (i = 0; i < cchSrc; i++) lpCharType[i] = (dwInfoType == 1) ? char_type((uint8_t)lpSrcStr[i]) : 0;
    return 1;
}

uint32_t CCALL GetStringTypeW_c(uint32_t dwInfoType, const uint16_t *lpSrcStr, int32_t cchSrc, uint16_t *lpCharType)
{
    int32_t i;
    if (cchSrc < 0) { for (cchSrc = 0; lpSrcStr[cchSrc]; cchSrc++); cchSrc++; }
    for (i = 0; i < cchSrc; i++) lpCharType[i] = (dwInfoType == 1) ? ((lpSrcStr[i] < 256) ? char_type(lpSrcStr[i]) : 0x100) : 0;
    return 1;
}

// LCMAP_LOWERCASE 0x100, LCMAP_UPPERCASE 0x200; anything else copies
int32_t CCALL LCMapStringA_c(uint32_t Locale, uint32_t dwMapFlags, const char *lpSrcStr, int32_t cchSrc, char *lpDestStr, int32_t cchDest)
{
    int32_t i;
    if (cchSrc < 0) cchSrc = (int32_t)strlen(lpSrcStr) + 1;
    if (cchDest == 0) return cchSrc;
    if (cchDest < cchSrc) { last_error = 122; return 0; }
    for (i = 0; i < cchSrc; i++)
    {
        int c = (uint8_t)lpSrcStr[i];
        if (dwMapFlags & 0x100) c = tolower(c); else if (dwMapFlags & 0x200) c = toupper(c);
        lpDestStr[i] = (char)c;
    }
    return cchSrc;
}

int32_t CCALL LCMapStringW_c(uint32_t Locale, uint32_t dwMapFlags, const uint16_t *lpSrcStr, int32_t cchSrc, uint16_t *lpDestStr, int32_t cchDest)
{
    int32_t i;
    if (cchSrc < 0) { for (cchSrc = 0; lpSrcStr[cchSrc]; cchSrc++); cchSrc++; }
    if (cchDest == 0) return cchSrc;
    if (cchDest < cchSrc) { last_error = 122; return 0; }
    for (i = 0; i < cchSrc; i++)
    {
        uint16_t c = lpSrcStr[i];
        if ((dwMapFlags & 0x100) && c < 128) c = (uint16_t)tolower(c); else if ((dwMapFlags & 0x200) && c < 128) c = (uint16_t)toupper(c);
        lpDestStr[i] = c;
    }
    return cchSrc;
}

// NORM_IGNORECASE 1; returns CSTR_LESS_THAN 1, CSTR_EQUAL 2, CSTR_GREATER_THAN 3
int32_t CCALL CompareStringA_c(uint32_t Locale, uint32_t dwCmpFlags, const char *s1, int32_t n1, const char *s2, int32_t n2)
{
    int32_t i, r = 0;
    if (n1 < 0) n1 = (int32_t)strlen(s1);
    if (n2 < 0) n2 = (int32_t)strlen(s2);
    for (i = 0; (i < n1) && (i < n2) && (r == 0); i++)
    {
        int a = (uint8_t)s1[i], b = (uint8_t)s2[i];
        if (dwCmpFlags & 1) { a = tolower(a); b = tolower(b); }
        r = a - b;
    }
    if (r == 0) r = n1 - n2;
    return (r < 0) ? 1 : ((r > 0) ? 3 : 2);
}

int32_t CCALL CompareStringW_c(uint32_t Locale, uint32_t dwCmpFlags, const uint16_t *s1, int32_t n1, const uint16_t *s2, int32_t n2)
{
    int32_t i, r = 0;
    if (n1 < 0) for (n1 = 0; s1[n1]; n1++);
    if (n2 < 0) for (n2 = 0; s2[n2]; n2++);
    for (i = 0; (i < n1) && (i < n2) && (r == 0); i++)
    {
        int a = s1[i], b = s2[i];
        if ((dwCmpFlags & 1) && a < 128 && b < 128) { a = tolower(a); b = tolower(b); }
        r = a - b;
    }
    if (r == 0) r = n1 - n2;
    return (r < 0) ? 1 : ((r > 0) ? 3 : 2);
}

// structured exception handling doesn't exist here: the filters are only recorded, raising stops
static uint32_t unhandled_filter;
uint32_t CCALL SetUnhandledExceptionFilter_c(uint32_t lpTopLevelExceptionFilter)
{
    uint32_t old = unhandled_filter;
    unhandled_filter = lpTopLevelExceptionFilter;
    return old;
}

int32_t CCALL UnhandledExceptionFilter_c(void *ExceptionInfo) { return 0; }

void CCALL RtlUnwind_c(void *TargetFrame, void *TargetIp, void *ExceptionRecord, uint32_t ReturnValue)
{
    eprintf("Error: RtlUnwind called - structured exception handling is not supported\n");
    app_exit(1);
}

void CCALL RaiseException_c(uint32_t dwExceptionCode, uint32_t dwExceptionFlags, uint32_t nNumberOfArguments, const uint32_t *lpArguments)
{
    // 0xE06D7363 = Visual C++ throw (_CxxThrowException): argument 1 is the thrown object, 2 its ThrowInfo
    eprintf("Error: RaiseException 0x%08x (%s) - exceptions are not supported\n", dwExceptionCode,
            (dwExceptionCode == 0xE06D7363) ? "C++ throw" : "SEH");
    app_exit(1);
}

uint32_t CCALL IsBadCodePtr_c(void *p) { return p == NULL; }
uint32_t CCALL IsBadReadPtr_c(const void *p, uint32_t ucb) { return (p == NULL) && (ucb != 0); }
uint32_t CCALL IsBadWritePtr_c(void *p, uint32_t ucb) { return (p == NULL) && (ucb != 0); }

int32_t CCALL InterlockedExchange_c(int32_t *Target, int32_t Value) { return __atomic_exchange_n(Target, Value, __ATOMIC_SEQ_CST); }

uint32_t CCALL QueryPerformanceFrequency_c(uint64_t *lpFrequency) { *lpFrequency = 1000000; return 1; }
uint32_t CCALL QueryPerformanceCounter_c(uint64_t *lpPerformanceCount)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *lpPerformanceCount = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    return 1;
}

typedef struct { uint16_t wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME_32;

// FILETIME: 100 ns units since 1601-01-01
static uint64_t unix_to_filetime_ns(time_t t, long nsec) { return ((uint64_t)t + 11644473600ull) * 10000000ull + nsec / 100; }

void CCALL GetSystemTimeAsFileTime_c(uint64_t *lpSystemTimeAsFileTime)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *lpSystemTimeAsFileTime = unix_to_filetime_ns(ts.tv_sec, ts.tv_nsec);
}

static void tm_to_systemtime(const struct tm *tm, long msec, SYSTEMTIME_32 *st)
{
    st->wYear = (uint16_t)(tm->tm_year + 1900); st->wMonth = (uint16_t)(tm->tm_mon + 1); st->wDayOfWeek = (uint16_t)tm->tm_wday;
    st->wDay = (uint16_t)tm->tm_mday; st->wHour = (uint16_t)tm->tm_hour; st->wMinute = (uint16_t)tm->tm_min;
    st->wSecond = (uint16_t)tm->tm_sec; st->wMilliseconds = (uint16_t)msec;
}

#ifdef _WIN32
// (MinGW: the C runtime's _s versions)
static struct tm *gmtime_r(const time_t *t, struct tm *r) { return (gmtime_s(r, t) == 0) ? r : NULL; }
static struct tm *localtime_r(const time_t *t, struct tm *r) { return (localtime_s(r, t) == 0) ? r : NULL; }
#endif

// local time - UTC at t, in seconds
static long utc_offset(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
#ifdef _WIN32
    return (long)(_mkgmtime(&lt) - t);
#else
    return lt.tm_gmtoff;
#endif
}

void CCALL GetSystemTime_c(SYSTEMTIME_32 *st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    tm_to_systemtime(&tm, ts.tv_nsec / 1000000, st);
}

void CCALL GetLocalTime_c(SYSTEMTIME_32 *st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    tm_to_systemtime(&tm, ts.tv_nsec / 1000000, st);
}

// TIME_ZONE_INFORMATION (172 bytes): UTC, no daylight saving
uint32_t CCALL GetTimeZoneInformation_c(uint8_t *lpTimeZoneInformation)
{
    memset(lpTimeZoneInformation, 0, 172);
    return 0;   // TIME_ZONE_ID_UNKNOWN
}

uint32_t CCALL FileTimeToLocalFileTime_c(const uint64_t *lpFileTime, uint64_t *lpLocalFileTime)
{
    time_t t = (time_t)(*lpFileTime / 10000000ull - 11644473600ull);
    *lpLocalFileTime = *lpFileTime + (int64_t)utc_offset(t) * 10000000;
    return 1;
}

uint32_t CCALL FileTimeToSystemTime_c(const uint64_t *lpFileTime, SYSTEMTIME_32 *st)
{
    time_t t = (time_t)(*lpFileTime / 10000000ull - 11644473600ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    tm_to_systemtime(&tm, (long)((*lpFileTime / 10000) % 1000), st);
    return 1;
}

uint32_t CCALL FileTimeToDosDateTime_c(const uint64_t *lpFileTime, uint16_t *lpFatDate, uint16_t *lpFatTime)
{
    SYSTEMTIME_32 st;
    FileTimeToSystemTime_c(lpFileTime, &st);
    *lpFatDate = (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    *lpFatTime = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    return 1;
}

uint32_t CCALL GetFileSize_c(void *hFile, uint32_t *lpFileSizeHigh)
{
    struct stat st;
    if ((handle_type(hFile) != HT_FILE) || (fstat(((win_file *)hFile)->fd, &st) != 0))
    {
        last_error = ERROR_INVALID_HANDLE;
        return 0xFFFFFFFFu;
    }
    if (lpFileSizeHigh != NULL) *lpFileSizeHigh = (uint32_t)((uint64_t)st.st_size >> 32);
    return (uint32_t)st.st_size;
}

// paths are passed through: the game uses relative paths below its directory
uint32_t CCALL GetFullPathNameA_c(const char *lpFileName, uint32_t nBufferLength, char *lpBuffer, char **lpFilePart)
{
    char cwd[260];
    uint32_t len;
    const char *p;

    if ((lpFileName[0] == '\\') || (lpFileName[0] != 0 && lpFileName[1] == ':')) snprintf(cwd, sizeof(cwd), "%s", lpFileName);
    else
    {
        GetCurrentDirectoryA_c(sizeof(cwd), cwd);
        len = (uint32_t)strlen(cwd);
        snprintf(cwd + len, sizeof(cwd) - len, "%s%s", (len && cwd[len - 1] == '\\') ? "" : "\\", lpFileName);
    }
    len = (uint32_t)strlen(cwd);
    if (len + 1 > nBufferLength) return len + 1;
    strcpy(lpBuffer, cwd);
    if (lpFilePart != NULL)
    {
        p = strrchr(lpBuffer, '\\');
        *(uint32_t *)lpFilePart = H2G((p != NULL) ? p + 1 : lpBuffer);
    }
    return len;
}

uint32_t CCALL SetCurrentDirectoryA_c(const char *lpPathName)
{
    if (winapi_debug) eprintf("SetCurrentDirectoryA: %s (ignored)\n", lpPathName);
    return 1;
}

// temporary files (the engine extracts the game DLLs from AVP2DLL.REZ into them): the game's own temp
// directory below its directory
uint32_t CCALL GetTempPathA_c(uint32_t nBufferLength, char *lpBuffer)
{
    char path[260];
    uint32_t len;
    GetCurrentDirectoryA_c(sizeof(path), path);
    len = (uint32_t)strlen(path);
    snprintf(path + len, sizeof(path) - len, "%stemp\\", (len && path[len - 1] == '\\') ? "" : "\\");
#ifdef _WIN32
    mkdir("temp");
#else
    mkdir("temp", 0755);
#endif
    len = (uint32_t)strlen(path);
    if (len + 1 > nBufferLength) return len + 1;
    strcpy(lpBuffer, path);
    return len;
}

uint32_t CCALL GetTempFileNameA_c(const char *lpPathName, const char *lpPrefixString, uint32_t uUnique, char *lpTempFileName)
{
    static uint32_t counter;
    char host[1024];
    uint32_t n = uUnique;
    size_t len = strlen(lpPathName);

    for (;;)
    {
        if (uUnique == 0) n = (uint32_t)getpid() + ++counter;
        sprintf(lpTempFileName, "%s%s%.3s%x.tmp", lpPathName, (len && lpPathName[len - 1] == '\\') ? "" : "\\", lpPrefixString, n & 0xFFFF);
        if (uUnique != 0) return n;
        if (!vfs_resolve(lpTempFileName, host, sizeof(host)) || (access(host, F_OK) != 0))
        {
            // reserve the name (as Windows does)
            if (vfs_resolve(lpTempFileName, host, sizeof(host))) { FILE *f = fopen(host, "wb"); if (f != NULL) fclose(f); }
            return n & 0xFFFF;
        }
    }
}

uint32_t CCALL GetDiskFreeSpaceA_c(const char *lpRootPathName, uint32_t *lpSectorsPerCluster, uint32_t *lpBytesPerSector, uint32_t *lpNumberOfFreeClusters, uint32_t *lpTotalNumberOfClusters)
{
    if (lpSectorsPerCluster) *lpSectorsPerCluster = 8;
    if (lpBytesPerSector) *lpBytesPerSector = 512;
    if (lpNumberOfFreeClusters) *lpNumberOfFreeClusters = 0x100000;     // 2 GB
    if (lpTotalNumberOfClusters) *lpTotalNumberOfClusters = 0x200000;
    return 1;
}

typedef struct { uint32_t dwLength, dwMemoryLoad, dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual; } MEMORYSTATUS_32;

void CCALL GlobalMemoryStatus_c(MEMORYSTATUS_32 *ms)
{
    ms->dwLength = sizeof(*ms);
    ms->dwMemoryLoad = 10;
    ms->dwTotalPhys = ms->dwTotalPageFile = 0x40000000;     // 1 GB
    ms->dwAvailPhys = ms->dwAvailPageFile = 0x30000000;
    ms->dwTotalVirtual = 0x7FFE0000;
    ms->dwAvailVirtual = 0x60000000;
}

void * CCALL LocalFree_c(void *hMem) { if (hMem != NULL) game_free(hMem); return NULL; }
int32_t CCALL lstrlenA_c(const char *s) { return (s != NULL) ? (int32_t)strlen(s) : 0; }

// RT_MESSAGETABLE (11), name 1: blocks of (LowId, HighId, OffsetToEntries); entries (Length, Flags, Text)
static int message_table_text(const uint8_t *image, uint32_t id, char *out, size_t size)
{
    const uint8_t *e, *data;
    uint32_t nblocks, i, k;

    if (image == NULL) return 0;
    e = pe_resource_find(image, 1, 11);
    if (e == NULL) return 0;
    data = (const uint8_t *)pe_resource_data(image, e, NULL);
    nblocks = *(const uint32_t *)data;
    for (i = 0; i < nblocks; i++)
    {
        const uint32_t *b = (const uint32_t *)(data + 4 + 12 * i);
        if ((id >= b[0]) && (id <= b[1]))
        {
            const uint8_t *entry = data + b[2];
            uint16_t len, flags;
            size_t n = 0;
            for (k = b[0]; k < id; k++) entry += *(const uint16_t *)entry;
            len = *(const uint16_t *)entry;
            flags = *(const uint16_t *)(entry + 2);
            if (flags & 1)
            {
                const uint16_t *t = (const uint16_t *)(entry + 4);
                for (k = 0; (k < (uint32_t)(len - 4) / 2u) && t[k] && (n + 1 < size); k++) out[n++] = (t[k] < 256) ? (char)t[k] : '?';
            }
            else
            {
                const char *t = (const char *)(entry + 4);
                for (k = 0; (k < (uint32_t)(len - 4)) && t[k] && (n + 1 < size); k++) out[n++] = t[k];
            }
            out[n] = 0;
            return 1;
        }
    }
    return 0;
}

// FORMAT_MESSAGE_ALLOCATE_BUFFER 0x100, _IGNORE_INSERTS 0x200, _FROM_STRING 0x400, _FROM_HMODULE 0x800,
// _FROM_SYSTEM 0x1000, _ARGUMENT_ARRAY 0x2000, _MAX_WIDTH_MASK 0xff.
// Arguments: a pointer to the caller's (32-bit) va_list, or to an array of dwords; inserts %1..%99 take
// the n-th dword, formatted by !printf format! (default !s!).
const char *gamma_string(uint32_t id);

uint32_t CCALL FormatMessageA_c(uint32_t dwFlags, const void *lpSource, uint32_t dwMessageId, uint32_t dwLanguageId, char *lpBuffer, uint32_t nSize, void *Arguments)
{
    char source[5000], *out;
    const char *src;
    uint32_t *args = NULL;
    size_t n = 0, cap = 8192;
    uint32_t max_width = dwFlags & 0xff;

    if (dwFlags & 0x400) src = (const char *)lpSource;
    else
    {
        int found = 0;
        if ((dwFlags & 0x800) && (gamma_string(dwMessageId) != NULL))
        {
            snprintf(source, sizeof(source), "%s", gamma_string(dwMessageId));
            found = 1;
        }
        if (!found && (dwFlags & 0x800)) found = message_table_text(winapi_module_image((void *)lpSource), dwMessageId, source, sizeof(source));
        if (!found && (dwFlags & 0x1000))
        {
            snprintf(source, sizeof(source), "Error %u%%n", dwMessageId);
            found = 1;
        }
        if (!found)
        {
            last_error = 317;   // ERROR_MR_MID_NOT_FOUND
            return 0;
        }
        src = source;
    }
    if (src == NULL) { last_error = ERROR_INVALID_PARAMETER; return 0; }
    if ((Arguments != NULL) && !(dwFlags & 0x200))
        args = (dwFlags & 0x2000) ? (uint32_t *)Arguments : (uint32_t *)G2H(*(uint32_t *)Arguments);

    out = (char *)malloc(cap);
    while (*src != 0)
    {
        char piece[1024];
        size_t plen = 0;

        if (*src == '%')
        {
            src++;
            if ((*src >= '1') && (*src <= '9'))
            {
                int index = *src++ - '0';
                char fmt[32] = "%s";
                if ((*src >= '0') && (*src <= '9')) index = index * 10 + (*src++ - '0');
                if (*src == '!')
                {
                    const char *endf = strchr(src + 1, '!');
                    if ((endf != NULL) && (endf - src < 30))
                    {
                        fmt[0] = '%';
                        memcpy(fmt + 1, src + 1, endf - src - 1);
                        fmt[endf - src] = 0;
                        src = endf + 1;
                    }
                }
                if (args == NULL)
                {
                    // inserts are kept
                    plen = (size_t)snprintf(piece, sizeof(piece), "%%%d", index);
                }
                else
                {
                    vsnprintf_x86(piece, sizeof(piece), fmt, args + index - 1);
                    plen = strlen(piece);
                }
            }
            else if (*src == '0') break;
            else if (*src == 'n') { piece[0] = '\r'; piece[1] = '\n'; plen = 2; src++; }
            else if (*src == 'r') { piece[0] = '\r'; plen = 1; src++; }
            else if (*src == 't') { piece[0] = '\t'; plen = 1; src++; }
            else if (*src != 0) { piece[0] = *src++; plen = 1; }
        }
        else if ((max_width != 0) && ((*src == '\r') || (*src == '\n')))
        {
            // line breaks of the text are ignored (only %n breaks lines)
            if ((*src == '\r') && (src[1] == '\n')) src++;
            src++;
            piece[0] = ' ';
            plen = 1;
        }
        else
        {
            piece[0] = *src++;
            plen = 1;
        }

        if (n + plen + 1 > cap)
        {
            cap = (n + plen + 1) * 2;
            out = (char *)realloc(out, cap);
        }
        memcpy(out + n, piece, plen);
        n += plen;
    }
    out[n] = 0;

    if (dwFlags & 0x100)    // lpBuffer receives a LocalAlloc'd pointer
    {
        char *p = (char *)game_malloc((uint32_t)n + 1);
        memcpy(p, out, n + 1);
        *(uint32_t *)lpBuffer = H2G(p);
    }
    else
    {
        if (n + 1 > nSize)
        {
            free(out);
            last_error = 122;   // ERROR_INSUFFICIENT_BUFFER
            return 0;
        }
        memcpy(lpBuffer, out, n + 1);
    }
    free(out);
    if (winapi_debug >= 2) eprintf("FormatMessageA: %u -> %u chars\n", dwMessageId, (unsigned)n);
    return (uint32_t)n;
}

void CCALL DebugBreak_c(void) { eprintf("DebugBreak called\n"); }

/* ------------------------------------------------------------------ */
/* AvP2: resources of the PE images (peimage.c)                        */

static const uint8_t *module_image(void *hModule)
{
    unsigned int i;
    if (hModule == NULL) return lithtech_image;
    for (i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
    {
        if (hModule == &modules[i]) return modules[i].image;
    }
    return (const uint8_t *)hModule;
}

const uint8_t *winapi_module_image(void *hModule) { return module_image(hModule); }

// a resource's name or type: MAKEINTRESOURCE (a number below 0x10000) or a string
static uintptr_t res_id(const void *p)
{
    return (H2G(p) < 0x10000) ? (uintptr_t)H2G(p) : (uintptr_t)p;
}

void * CCALL FindResourceA_c(void *hModule, const char *lpName, const char *lpType)
{
    const uint8_t *e = pe_resource_find(module_image(hModule), res_id(lpName), res_id(lpType));
    if (winapi_debug >= 2) eprintf("FindResourceA: %p %p -> %p\n", lpName, lpType, e);
    if (e == NULL) last_error = 1814;   // ERROR_RESOURCE_NAME_NOT_FOUND
    return (void *)e;
}

void * CCALL LoadResource_c(void *hModule, void *hResInfo)
{
    if (hResInfo == NULL) return NULL;
    return pe_resource_data(module_image(hModule), (const uint8_t *)hResInfo, NULL);
}

void * CCALL LockResource_c(void *hResData) { return hResData; }

uint32_t CCALL SizeofResource_c(void *hModule, void *hResInfo)
{
    uint32_t size = 0;
    if (hResInfo != NULL) pe_resource_data(module_image(hModule), (const uint8_t *)hResInfo, &size);
    return size;
}

// the port's strings (gamma.c: its slider on the game's Display page), else the module's
const char *gamma_string(uint32_t id);

int32_t CCALL LoadStringA_c(void *hInstance, uint32_t uID, char *lpBuffer, int32_t cchBufferMax)
{
    const char *own = gamma_string(uID);
    if ((own != NULL) && (lpBuffer != NULL) && (cchBufferMax > 0))
    {
        snprintf(lpBuffer, (size_t)cchBufferMax, "%s", own);
        return (int32_t)strlen(lpBuffer);
    }
    return pe_load_string(module_image(hInstance), uID, lpBuffer, cchBufferMax);
}

/* ------------------------------------------------------------------ */
/* AvP2: threads, events, waits, critical sections, TLS                */
/* Each thread runs recompiled code on its own emulated x86 stack (call_game: x86_initialize_stack is per   */
/* thread). Waitable objects share one mutex and condition variable.                                        */

#define HT_EVENT 0x45564e54
#define HT_WTHREAD 0x54485244

typedef struct {
    uint32_t type;
    int manual, state;
} event_handle;

typedef struct {
    uint32_t type;
    pthread_t thread;
    uint32_t start, param, id, exit_code;
    int started, finished, suspended;
} thread_handle;

static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static uint32_t next_thread_id = 2;
static thread_local uint32_t current_thread_id;

uint32_t CCALL GetCurrentThreadId_c(void)
{
    if (current_thread_id == 0) current_thread_id = 1;     // the main thread
    return current_thread_id;
}

static thread_local thread_handle *current_thread;

// the thread's handle becomes signalled (WaitForSingleObject on it returns)
static void thread_finished(thread_handle *t, uint32_t code)
{
    pthread_mutex_lock(&wait_mutex);
    t->exit_code = code;
    t->finished = 1;
    pthread_cond_broadcast(&wait_cond);
    pthread_mutex_unlock(&wait_mutex);
}

static void *thread_main(void *arg)
{
    thread_handle *t = (thread_handle *)arg;
    uint32_t param = t->param;

    current_thread_id = t->id;
    current_thread = t;
    thread_finished(t, call_game(t->start, 1, &param));
    return NULL;
}

static int start_thread(thread_handle *t)
{
    pthread_attr_t attr;
    int err;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    t->started = 1;
    err = pthread_create(&t->thread, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (err == 0) pthread_detach(t->thread);
    return err == 0;
}

void * CCALL CreateThread_c(void *lpThreadAttributes, uint32_t dwStackSize, uint32_t lpStartAddress, uint32_t lpParameter, uint32_t dwCreationFlags, uint32_t *lpThreadId)
{
    thread_handle *t = (thread_handle *)game_malloc(sizeof(thread_handle));
    t->type = HT_WTHREAD;
    t->start = lpStartAddress;
    t->param = lpParameter;
    pthread_mutex_lock(&wait_mutex);
    t->id = next_thread_id++;
    pthread_mutex_unlock(&wait_mutex);
    if (lpThreadId != NULL) *lpThreadId = t->id;
    if (winapi_debug) eprintf("CreateThread: start 0x%x param 0x%x id %u%s\n", lpStartAddress, lpParameter, t->id, (dwCreationFlags & 4) ? " (suspended)" : "");
    if (dwCreationFlags & 4)    // CREATE_SUSPENDED
    {
        t->suspended = 1;
        return t;
    }
    if (!start_thread(t))
    {
        game_free(t);
        last_error = 8;
        return NULL;
    }
    return t;
}

uint32_t CCALL ResumeThread_c(void *hThread)
{
    thread_handle *t = (thread_handle *)hThread;
    if (handle_type(t) != HT_WTHREAD) return 0xFFFFFFFFu;
    if (t->suspended && !t->started)
    {
        t->suspended = 0;
        start_thread(t);
        return 1;
    }
    return 0;
}

uint32_t CCALL SetThreadPriority_c(void *hThread, int32_t nPriority) { return 1; }

void CCALL ExitThread_c(uint32_t dwExitCode)
{
    if (GetCurrentThreadId_c() == 1) app_exit(dwExitCode);
    // the thread's call_game frame is abandoned with it. The C runtime's threads always end here
    // (_endthreadex), so this is what signals their handles.
    if (winapi_debug) eprintf("ExitThread: thread %u exits (%u)\n", GetCurrentThreadId_c(), dwExitCode);
    if (current_thread != NULL) thread_finished(current_thread, dwExitCode);
    pthread_exit(NULL);
}

void * CCALL CreateEventA_c(void *lpEventAttributes, uint32_t bManualReset, uint32_t bInitialState, const char *lpName)
{
    event_handle *e = (event_handle *)game_malloc(sizeof(event_handle));
    e->type = HT_EVENT; e->manual = (bManualReset != 0); e->state = (bInitialState != 0);
    return e;
}

static uint32_t set_event(void *h, int state, int pulse)
{
    if (handle_type(h) != HT_EVENT) { last_error = ERROR_INVALID_HANDLE; return 0; }
    pthread_mutex_lock(&wait_mutex);
    ((event_handle *)h)->state = state;
    if (state || pulse) pthread_cond_broadcast(&wait_cond);
    pthread_mutex_unlock(&wait_mutex);
    return 1;
}
uint32_t CCALL SetEvent_c(void *h) { return set_event(h, 1, 0); }
uint32_t CCALL ResetEvent_c(void *h) { return set_event(h, 0, 0); }
// PulseEvent: waiters that are waiting now are released; approximated by setting and clearing around a wakeup
uint32_t CCALL PulseEvent_c(void *h)
{
    if (handle_type(h) != HT_EVENT) return 0;
    set_event(h, 1, 0);
    sched_yield();
    return set_event(h, 0, 1);
}

// called with wait_mutex held: is the object signalled? (consumes auto-reset events)
static int signalled(void *h, int consume)
{
    switch (handle_type(h))
    {
        case HT_EVENT:
        {
            event_handle *e = (event_handle *)h;
            if (!e->state) return 0;
            if (consume && !e->manual) e->state = 0;
            return 1;
        }
        case HT_WTHREAD: return ((thread_handle *)h)->finished;
        case HT_PROCESS: return 0;
        default: return 1;      // files, the main thread pseudo handle, ...: signalled
    }
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// AVP2_DEBUG: waits of 50 ms or more (the thread, the recompiled code's return address)
static void report_long_wait(const char *what, uint64_t start, uint32_t timeout, uint32_t result)
{
    uint64_t t = now_ms() - start;
    if (winapi_debug && (t >= 50))
        eprintf("%s: thread %u waited %u ms (timeout %d, result %u), caller %08x\n", what, GetCurrentThreadId_c(), (unsigned)t,
                (int)timeout, result, game_caller());
}

static uint32_t wait_objects(uint32_t n, void *const *h, int all, uint32_t ms)
{
    struct timespec deadline;
    uint32_t i, result = 0x102;     // WAIT_TIMEOUT
    uint64_t start = now_ms();

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }

    pthread_mutex_lock(&wait_mutex);
    for (;;)
    {
        if (all)
        {
            for (i = 0; i < n && signalled(h[i], 0); i++);
            if (i == n) { for (i = 0; i < n; i++) signalled(h[i], 1); result = 0; break; }
        }
        else
        {
            for (i = 0; i < n; i++) if (signalled(h[i], 1)) break;
            if (i < n) { result = i; break; }
        }
        if (ms == 0) break;
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&wait_cond, &wait_mutex);
        else if (pthread_cond_timedwait(&wait_cond, &wait_mutex, &deadline) != 0) break;
    }
    pthread_mutex_unlock(&wait_mutex);
    report_long_wait("wait", start, ms, result);
    return result;
}

uint32_t CCALL WaitForSingleObject_c(void *hHandle, uint32_t dwMilliseconds)
{
    return wait_objects(1, &hHandle, 0, dwMilliseconds);
}

uint32_t CCALL WaitForMultipleObjects_c(uint32_t nCount, const uint32_t *lpHandles, uint32_t bWaitAll, uint32_t dwMilliseconds)
{
    void *h[64];
    uint32_t i;
    if (nCount > 64) nCount = 64;
    for (i = 0; i < nCount; i++) h[i] = G2H(lpHandles[i]);
    return wait_objects(nCount, h, bWaitAll != 0, dwMilliseconds);
}

// CRITICAL_SECTION (24 bytes in game memory): its first dword (DebugInfo) holds a recursive mutex allocated
// below 2 GB
static pthread_mutex_t cs_init_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *cs_mutex(void *cs)
{
    uint32_t *p = (uint32_t *)cs;
    pthread_mutex_t *m = (pthread_mutex_t *)G2H(__atomic_load_n(p, __ATOMIC_ACQUIRE));
    if (m != NULL) return m;
    pthread_mutex_lock(&cs_init_mutex);
    m = (pthread_mutex_t *)G2H(*p);
    if (m == NULL)
    {
        pthread_mutexattr_t attr;
        m = (pthread_mutex_t *)x86_malloc(sizeof(pthread_mutex_t));
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &attr);
        pthread_mutexattr_destroy(&attr);
        __atomic_store_n(p, H2G(m), __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&cs_init_mutex);
    return m;
}

void CCALL InitializeCriticalSection_c(void *lpCriticalSection)
{
    memset(lpCriticalSection, 0, 24);
    cs_mutex(lpCriticalSection);
}

void CCALL DeleteCriticalSection_c(void *lpCriticalSection)
{
    uint32_t *p = (uint32_t *)lpCriticalSection;
    pthread_mutex_t *m = (pthread_mutex_t *)G2H(*p);
    if (m != NULL)
    {
        pthread_mutex_destroy(m);
        x86_free(m);
        *p = 0;
    }
}

void CCALL EnterCriticalSection_c(void *lpCriticalSection)
{
    pthread_mutex_t *m = cs_mutex(lpCriticalSection);
    if (pthread_mutex_trylock(m) != 0)
    {
        uint64_t start = now_ms();
        pthread_mutex_lock(m);
        report_long_wait("EnterCriticalSection", start, 0xFFFFFFFFu, 0);
    }
}
void CCALL LeaveCriticalSection_c(void *lpCriticalSection) { pthread_mutex_unlock(cs_mutex(lpCriticalSection)); }

int32_t CCALL InterlockedIncrement_c(int32_t *lpAddend) { return __atomic_add_fetch(lpAddend, 1, __ATOMIC_SEQ_CST); }
int32_t CCALL InterlockedDecrement_c(int32_t *lpAddend) { return __atomic_sub_fetch(lpAddend, 1, __ATOMIC_SEQ_CST); }

// TLS: slots are allocated process-wide, the values are per thread
static uint32_t tls_next = 1;
static thread_local uint32_t tls_values[64];
uint32_t CCALL TlsAlloc_c(void)
{
    uint32_t i;
    pthread_mutex_lock(&wait_mutex);
    i = (tls_next < 64) ? tls_next++ : 0xFFFFFFFFu;
    pthread_mutex_unlock(&wait_mutex);
    return i;
}
uint32_t CCALL TlsGetValue_c(uint32_t dwTlsIndex) { return (dwTlsIndex < 64) ? tls_values[dwTlsIndex] : 0; }
uint32_t CCALL TlsSetValue_c(uint32_t dwTlsIndex, uint32_t lpTlsValue) { if (dwTlsIndex >= 64) return 0; tls_values[dwTlsIndex] = lpTlsValue; return 1; }

uint32_t CCALL TlsFree_c(uint32_t dwTlsIndex) { return (dwTlsIndex < 64) ? 1 : 0; }

uint32_t CCALL WaitForSingleObjectEx_c(void *hHandle, uint32_t dwMilliseconds, uint32_t bAlertable)
{
    return WaitForSingleObject_c(hHandle, dwMilliseconds);
}

/* ------------------------------------------------------------------ */
/* AvP2: directories, files, times, INI files, processes               */

static uint32_t error_mode;
uint32_t CCALL SetErrorMode_c(uint32_t uMode)
{
    uint32_t old = error_mode;
    error_mode = uMode;
    return old;
}

uint32_t CCALL CreateDirectoryA_c(const char *lpPathName, void *lpSecurityAttributes)
{
    char path[1024];

    if (lpPathName == NULL) { last_error = ERROR_FILE_NOT_FOUND; return 0; }
    vfs_resolve(lpPathName, path, sizeof(path));
    if (winapi_debug) eprintf("CreateDirectoryA: %s -> %s\n", lpPathName, path);
#ifdef _WIN32
    if (mkdir(path) != 0)
#else
    if (mkdir(path, 0777) != 0)
#endif
    {
        last_error = (errno == EEXIST) ? 183 : errno_to_win(errno);    // ERROR_ALREADY_EXISTS
        return 0;
    }
    return 1;
}

uint32_t CCALL RemoveDirectoryA_c(const char *lpPathName)
{
    char path[1024];

    if ((lpPathName == NULL) || !vfs_resolve(lpPathName, path, sizeof(path))) { last_error = ERROR_FILE_NOT_FOUND; return 0; }
    if (rmdir(path) != 0) { last_error = errno_to_win(errno); return 0; }
    return 1;
}

uint32_t CCALL MoveFileA_c(const char *lpExistingFileName, const char *lpNewFileName)
{
    char from[1024], to[1024];

    if ((lpExistingFileName == NULL) || (lpNewFileName == NULL) || !vfs_resolve(lpExistingFileName, from, sizeof(from)))
    {
        last_error = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    vfs_resolve(lpNewFileName, to, sizeof(to));
    if (access(to, F_OK) == 0) { last_error = 183; return 0; }  // MoveFile doesn't replace
    if (rename(from, to) != 0) { last_error = errno_to_win(errno); return 0; }
    return 1;
}

uint32_t CCALL SetFileTime_c(void *hFile, const uint64_t *lpCreationTime, const uint64_t *lpLastAccessTime, const uint64_t *lpLastWriteTime)
{
    if (handle_type(hFile) != HT_FILE) { last_error = ERROR_INVALID_HANDLE; return 0; }
#ifndef _WIN32
    struct timespec ts[2];
    ts[0].tv_nsec = ts[1].tv_nsec = UTIME_OMIT;
    ts[0].tv_sec = ts[1].tv_sec = 0;
    if (lpLastAccessTime != NULL)
    {
        ts[0].tv_sec = (time_t)(*lpLastAccessTime / 10000000ull - 11644473600ull);
        ts[0].tv_nsec = (long)(*lpLastAccessTime % 10000000ull) * 100;
    }
    if (lpLastWriteTime != NULL)
    {
        ts[1].tv_sec = (time_t)(*lpLastWriteTime / 10000000ull - 11644473600ull);
        ts[1].tv_nsec = (long)(*lpLastWriteTime % 10000000ull) * 100;
    }
    futimens(((win_file *)hFile)->fd, ts);
#endif
    return 1;
}

uint32_t CCALL SystemTimeToFileTime_c(const SYSTEMTIME_32 *st, uint64_t *lpFileTime)
{
    struct tm tm;
    time_t t;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min = st->wMinute;
    tm.tm_sec = st->wSecond;
#ifdef _WIN32
    t = _mkgmtime(&tm);
#else
    t = timegm(&tm);
#endif
    *lpFileTime = ((uint64_t)t + 11644473600ull) * 10000000ull + (uint64_t)st->wMilliseconds * 10000;
    return 1;
}

uint32_t CCALL LocalFileTimeToFileTime_c(const uint64_t *lpLocalFileTime, uint64_t *lpFileTime)
{
    time_t t = (time_t)(*lpLocalFileTime / 10000000ull - 11644473600ull);
    *lpFileTime = *lpLocalFileTime - (int64_t)utc_offset(t) * 10000000;
    return 1;
}

// OFSTRUCT: cBytes, fFixedDisk, nErrCode, Reserved1, Reserved2, szPathName[128]
typedef struct { uint8_t cBytes, fFixedDisk; uint16_t nErrCode, Reserved1, Reserved2; char szPathName[128]; } OFSTRUCT_32;

int32_t CCALL OpenFile_c(const char *lpFileName, OFSTRUCT_32 *lpReOpenBuff, uint32_t uStyle)
{
    char path[1024];
    void *h;
    uint32_t access_mode, creation;

    if (lpReOpenBuff != NULL)
    {
        memset(lpReOpenBuff, 0, sizeof(*lpReOpenBuff));
        lpReOpenBuff->cBytes = sizeof(*lpReOpenBuff);
        lpReOpenBuff->fFixedDisk = 1;
        if (lpFileName != NULL) snprintf(lpReOpenBuff->szPathName, sizeof(lpReOpenBuff->szPathName), "%s", lpFileName);
    }
    if (lpFileName == NULL) return -1;

    if (uStyle & 0x4000)    // OF_EXIST
    {
        int ok = vfs_resolve(lpFileName, path, sizeof(path)) && (access(path, F_OK) == 0);
        if (winapi_debug) eprintf("OpenFile: %s (exist) -> %d\n", lpFileName, ok);
        if (!ok && (lpReOpenBuff != NULL)) lpReOpenBuff->nErrCode = ERROR_FILE_NOT_FOUND;
        return ok ? 1 : -1;
    }
    if (uStyle & 0x0200)    // OF_DELETE
    {
        return DeleteFileA_c(lpFileName) ? 1 : -1;
    }
    access_mode = ((uStyle & 3) == 0) ? 0x80000000u : (((uStyle & 3) == 1) ? 0x40000000u : 0xC0000000u);
    creation = (uStyle & 0x1000) ? 2 : 3;   // OF_CREATE: CREATE_ALWAYS, else OPEN_EXISTING
    h = CreateFileA_c(lpFileName, access_mode, 3, NULL, creation, 0x80, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (lpReOpenBuff != NULL) lpReOpenBuff->nErrCode = (uint16_t)last_error;
        return -1;
    }
    return (int32_t)H2G(h);
}

uint32_t CCALL CreateProcessA_c(const char *lpApplicationName, char *lpCommandLine, void *lpProcessAttributes, void *lpThreadAttributes,
                                uint32_t bInheritHandles, uint32_t dwCreationFlags, void *lpEnvironment, const char *lpCurrentDirectory,
                                void *lpStartupInfo, void *lpProcessInformation)
{
    eprintf("CreateProcessA: %s %s (not supported)\n", (lpApplicationName != NULL) ? lpApplicationName : "", (lpCommandLine != NULL) ? lpCommandLine : "");
    last_error = ERROR_FILE_NOT_FOUND;
    return 0;
}

// INI files: read and written whole, case-insensitive sections and keys
#define INI_MAX_LINES 2048

typedef struct { int nlines; char *lines[INI_MAX_LINES]; } ini_file;

static void ini_load(const char *winpath, ini_file *ini, char *host, size_t hostsize)
{
    FILE *f;
    char line[1024];

    ini->nlines = 0;
    // a bare name is in the Windows directory; ours is the game directory
    vfs_resolve(winpath, host, hostsize);
    f = fopen(host, "rt");
    if (f == NULL) return;
    while ((ini->nlines < INI_MAX_LINES) && (fgets(line, sizeof(line), f) != NULL))
    {
        size_t n = strlen(line);
        while (n && ((line[n - 1] == '\n') || (line[n - 1] == '\r'))) line[--n] = 0;
        ini->lines[ini->nlines++] = strdup(line);
    }
    fclose(f);
}

static void ini_free(ini_file *ini)
{
    int i;
    for (i = 0; i < ini->nlines; i++) free(ini->lines[i]);
    ini->nlines = 0;
}

static const char *ini_trim(const char *s, char *out, size_t size, char stop)
{
    size_t n = 0;
    while ((*s == ' ') || (*s == '\t')) s++;
    while ((*s != 0) && (*s != stop) && (n + 1 < size)) out[n++] = *s++;
    while (n && ((out[n - 1] == ' ') || (out[n - 1] == '\t'))) n--;
    out[n] = 0;
    return s;
}

// line index of the section header, or -1
static int ini_find_section(ini_file *ini, const char *section)
{
    int i;
    char name[256];
    for (i = 0; i < ini->nlines; i++)
    {
        const char *s = ini->lines[i];
        while ((*s == ' ') || (*s == '\t')) s++;
        if (*s != '[') continue;
        ini_trim(s + 1, name, sizeof(name), ']');
        if (0 == strcasecmp(name, section)) return i;
    }
    return -1;
}

// line index of the key in the section starting at sec, or -1; *end = the line after the section's last line
static int ini_find_key(ini_file *ini, int sec, const char *key, int *end)
{
    int i;
    char name[256];
    for (i = sec + 1; i < ini->nlines; i++)
    {
        const char *s = ini->lines[i];
        while ((*s == ' ') || (*s == '\t')) s++;
        if (*s == '[') break;
        if (strchr(s, '=') == NULL) continue;
        ini_trim(s, name, sizeof(name), '=');
        if ((key != NULL) && (0 == strcasecmp(name, key))) { if (end != NULL) *end = i + 1; return i; }
    }
    if (end != NULL)
    {
        // after the last non-empty line of the section
        int last = i;
        while ((last > sec + 1) && (ini->lines[last - 1][0] == 0)) last--;
        *end = last;
    }
    return -1;
}

static uint32_t copy_string_list(char *buffer, uint32_t size, const char *list, uint32_t len)
{
    // list: strings separated by zeros, len excluding the final terminator
    if (size == 0) return 0;
    if (len + 1 > size)
    {
        len = (size >= 2) ? size - 2 : 0;
        memcpy(buffer, list, len);
        buffer[len] = 0;
        if (size >= 2) buffer[len + 1] = 0;
        return len;
    }
    memcpy(buffer, list, len);
    buffer[len] = 0;
    return (len > 0) ? len - 1 : 0;
}

uint32_t CCALL GetPrivateProfileStringA_c(const char *lpAppName, const char *lpKeyName, const char *lpDefault, char *lpReturnedString, uint32_t nSize, const char *lpFileName)
{
    ini_file ini;
    char host[1024], value[1024];
    uint32_t n = 0;
    int sec, k;

    if ((lpReturnedString == NULL) || (nSize == 0) || (lpFileName == NULL)) return 0;
    ini_load(lpFileName, &ini, host, sizeof(host));
    sec = (lpAppName != NULL) ? ini_find_section(&ini, lpAppName) : -1;

    if ((lpAppName == NULL) || (lpKeyName == NULL))
    {
        // the names of all sections, or of all keys in a section
        char list[8192];
        uint32_t len = 0;
        int i;
        for (i = (lpAppName == NULL) ? 0 : sec + 1; (i >= 0) && (i < ini.nlines); i++)
        {
            const char *s = ini.lines[i];
            char name[256];
            while ((*s == ' ') || (*s == '\t')) s++;
            if (lpAppName == NULL)
            {
                if (*s != '[') continue;
                ini_trim(s + 1, name, sizeof(name), ']');
            }
            else
            {
                if (*s == '[') break;
                if (strchr(s, '=') == NULL) continue;
                ini_trim(s, name, sizeof(name), '=');
            }
            if (len + strlen(name) + 2 >= sizeof(list)) break;
            strcpy(list + len, name);
            len += (uint32_t)strlen(name) + 1;
        }
        if ((lpAppName != NULL) && (sec < 0)) len = 0;
        ini_free(&ini);
        return copy_string_list(lpReturnedString, nSize, list, len);
    }

    k = (sec >= 0) ? ini_find_key(&ini, sec, lpKeyName, NULL) : -1;
    if (k >= 0)
    {
        ini_trim(strchr(ini.lines[k], '=') + 1, value, sizeof(value), 0);
        // surrounding quotes are removed
        n = (uint32_t)strlen(value);
        if ((n >= 2) && (((value[0] == '"') && (value[n - 1] == '"')) || ((value[0] == '\'') && (value[n - 1] == '\''))))
        {
            memmove(value, value + 1, n - 2);
            value[n - 2] = 0;
        }
    }
    else
    {
        ini_trim((lpDefault != NULL) ? lpDefault : "", value, sizeof(value), 0);
    }
    ini_free(&ini);
    n = (uint32_t)strlen(value);
    if (n >= nSize) n = nSize - 1;
    memcpy(lpReturnedString, value, n);
    lpReturnedString[n] = 0;
    if (winapi_debug >= 2) eprintf("GetPrivateProfileStringA: [%s] %s in %s -> %s\n", lpAppName, lpKeyName, lpFileName, lpReturnedString);
    return n;
}

uint32_t CCALL WritePrivateProfileStringA_c(const char *lpAppName, const char *lpKeyName, const char *lpString, const char *lpFileName)
{
    ini_file ini;
    char host[1024], line[1024];
    int sec, k, end, i;
    FILE *f;

    if ((lpAppName == NULL) || (lpFileName == NULL)) return 0;
    ini_load(lpFileName, &ini, host, sizeof(host));
    sec = ini_find_section(&ini, lpAppName);

    if (lpKeyName == NULL)
    {
        // delete the section
        if (sec >= 0)
        {
            ini_find_key(&ini, sec, NULL, &end);
            for (i = sec; i < end; i++) { free(ini.lines[i]); ini.lines[i] = NULL; }
        }
    }
    else
    {
        if (sec < 0)
        {
            if (lpString == NULL) { ini_free(&ini); return 1; }
            if (ini.nlines + 2 >= INI_MAX_LINES) { ini_free(&ini); return 0; }
            snprintf(line, sizeof(line), "[%s]", lpAppName);
            ini.lines[ini.nlines++] = strdup(line);
            sec = ini.nlines - 1;
        }
        k = ini_find_key(&ini, sec, lpKeyName, &end);
        if (lpString == NULL)
        {
            if (k >= 0) { free(ini.lines[k]); ini.lines[k] = NULL; }
        }
        else
        {
            snprintf(line, sizeof(line), "%s=%s", lpKeyName, lpString);
            if (k >= 0)
            {
                free(ini.lines[k]);
                ini.lines[k] = strdup(line);
            }
            else if (ini.nlines < INI_MAX_LINES)
            {
                memmove(&ini.lines[end + 1], &ini.lines[end], (ini.nlines - end) * sizeof(char *));
                ini.lines[end] = strdup(line);
                ini.nlines++;
            }
        }
    }

    f = fopen(host, "wt");
    if (f == NULL) { ini_free(&ini); last_error = errno_to_win(errno); return 0; }
    for (i = 0; i < ini.nlines; i++)
    {
        if (ini.lines[i] != NULL) fprintf(f, "%s\r\n", ini.lines[i]);
    }
    fclose(f);
    for (i = 0; i < ini.nlines; i++) free(ini.lines[i]);
    return 1;
}

EXTERN_C_END
