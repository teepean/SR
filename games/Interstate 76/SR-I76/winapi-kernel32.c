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
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "ptr32.h"
#include "Game-Memory.h"
#include "platform.h"
#include "config.h"
#include "vfs.h"
#include "winapi.h"
#include "display.h"

EXTERN_C_BEGIN

// mappings the game can see: below 2 GB in the 64-bit build (reserve low address space, map over it)
static void *low_mmap(size_t len, int fd, off_t offset)
{
#ifdef __cplusplus
    void *base = map_memory_32bit((unsigned int) len, 1);
    if (base == NULL) return MAP_FAILED;
    if (fd >= 0) return mmap(base, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, offset);
    return mmap(base, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#else
    // 32-bit build: the kernel places mappings high (0xE0000000...), but game code assumes Win32 addresses
    // below 2 GB (e.g. sub_469B00 computes NULL - pointer as a signed length) - search the low range
    static uintptr_t hint = 0x20000000;
    uintptr_t start = hint;
    size_t alen = (len + 4095) & ~(size_t)4095;
    int flags = ((fd >= 0) ? MAP_PRIVATE : (MAP_PRIVATE | MAP_ANONYMOUS)) | MAP_FIXED_NOREPLACE;
    for (;;)
    {
        void *addr;
        if (hint + alen > 0x7FFF0000u) hint = 0x10000000;
        addr = mmap((void *) hint, len, PROT_READ | PROT_WRITE, flags, fd, (fd >= 0) ? offset : 0);
        if (addr != MAP_FAILED)
        {
            hint += alen;
            return addr;
        }
        if (errno != EEXIST) return MAP_FAILED;
        hint += alen;
        if ((hint <= start) && (hint + alen > start)) return MAP_FAILED;    // wrapped around: no room
    }
#endif
}

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

static uint32_t process_handle_obj = HT_PROCESS;
static uint32_t thread_handle_obj = HT_THREAD;

static uint32_t handle_type(void *h)
{
    if ((h == NULL) || (h == (void *)-1)) return 0;
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

    fd = open(path, flags, 0644);
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
    if ((uintptr_t)hFile <= 3) return 2;
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
        case -10: return (void *)1;
        case -11: return (void *)2;
        case -12: return (void *)3;
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

    fin = open(src, O_RDONLY);
    if (fin < 0) { last_error = errno_to_win(errno); return 0; }
    fout = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
        addr = low_mmap(len, m->fd, dwFileOffsetLow);
    }
    else
    {
        addr = low_mmap(len, -1, 0);
    }
    if (addr == MAP_FAILED) return NULL;

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
            munmap(views[i].addr, views[i].len);
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

static heap_obj process_heap = { HT_HEAP, NULL };

/* Heap robustness and diagnostics
 * - the slack after every block is filled with a check pattern; overruns (the game writing past a block) are
 *   detected when the block is freed/reallocated, and for all blocks once per frame with I76_HEAPCHECK=1
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

/* I76_HEAPGUARD=1 (debugging): every block gets its own pages, ending right before an inaccessible guard
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
    if (heap_guard < 0) heap_guard = (getenv("I76_HEAPGUARD") != NULL);
    return heap_guard;
}

static heap_block *guard_alloc(uint32_t total)
{
    size_t data = ((total + 3) & ~(size_t)3) + GUARD_ZERO_PAD;  // zero pad (terminators), then the guard page
    size_t len = (data + 4095) & ~(size_t)4095;
    uint8_t *base = (uint8_t *) low_mmap(len + 4096, -1, 0);
    if (base == (uint8_t *) MAP_FAILED) return NULL;
    mprotect(base + len, 4096, PROT_NONE);
    return (heap_block *)(base + len - data);
}

static void guard_free(heap_block *b, uint32_t total)
{
    size_t data = ((total + 3) & ~(size_t)3) + GUARD_ZERO_PAD;  // zero pad (terminators), then the guard page
    size_t len = (data + 4095) & ~(size_t)4095;
    uint8_t *base = (uint8_t *) b + data - len;
    mprotect(base, len, PROT_NONE);
    // keep the freed pages inaccessible for a while, then release them
    if (guard_kept[guard_kept_pos].base != NULL)
    {
        munmap(guard_kept[guard_kept_pos].base, guard_kept[guard_kept_pos].len);
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

#if !defined(__x86_64__) && !defined(__SANITIZE_ADDRESS__)
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
#if !defined(__x86_64__) && !defined(__SANITIZE_ADDRESS__)
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

    if (enabled < 0) enabled = (getenv("I76_HEAPCHECK") != NULL);
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
#if !defined(__x86_64__) && !defined(__SANITIZE_ADDRESS__)
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

/* Memory the runtime hands to the game (COM objects, surfaces, sound buffers, handles, Smacker objects...):
 * allocated from a private emulated Win32 heap, so it gets the same protection as the game's own heap blocks
 * (quarantine after free, overrun detection, I76_HEAPGUARD) - in the 32-bit build x86_malloc is glibc's malloc,
 * and stray game writes into freed runtime memory corrupted glibc (crashes noticed later in the GL driver). */
static heap_obj runtime_heap = { HT_HEAP, NULL };

void *game_malloc(uint32_t size) { return HeapAlloc_c(&runtime_heap, 0, size); }     // HeapAlloc zeroes
void *game_calloc(uint32_t n, uint32_t size) { return HeapAlloc_c(&runtime_heap, 0, n * size); }
void game_free(void *p) { if (p != NULL) HeapFree_c(&runtime_heap, 0, p); }

uint32_t CCALL HeapSize_c(heap_obj *hHeap, uint32_t dwFlags, const void *lpMem)
{
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
        eprintf("VirtualAlloc: unsupported fixed address 0x%x\n", (unsigned)(uintptr_t)lpAddress);
        return NULL;
    }

    size = (dwSize + 65535) & ~65535u;
    addr = low_mmap(size, -1, 0);
    if (addr == MAP_FAILED)
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
    if (winapi_debug) eprintf("VirtualAlloc: %u bytes -> 0x%x\n", dwSize, (unsigned)(uintptr_t)addr);
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
            munmap(regions[i].addr, regions[i].size);
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
            lpBuffer->BaseAddress = (uint32_t)(uintptr_t)lpAddress & ~4095u;
            lpBuffer->AllocationBase = (uint32_t)(uintptr_t)regions[i].addr;
            lpBuffer->AllocationProtect = 0x04;
            lpBuffer->RegionSize = (uint32_t)(uintptr_t)(regions[i].addr + regions[i].size) - lpBuffer->BaseAddress;
            lpBuffer->State = MEM_COMMIT;
            lpBuffer->Protect = 0x04; // PAGE_READWRITE
            lpBuffer->Type = 0x20000; // MEM_PRIVATE
            return sizeof(memory_basic_information);
        }
    }

    // any other address: report committed read/write memory
    lpBuffer->BaseAddress = (uint32_t)(uintptr_t)lpAddress & ~4095u;
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

void CCALL Sleep_c(uint32_t dwMilliseconds)
{
    struct timespec ts;

    winapi_process_events();
    ts.tv_sec = dwMilliseconds / 1000;
    ts.tv_nsec = (dwMilliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

void * CCALL GetCurrentProcess_c(void) { return &process_handle_obj; }
uint32_t CCALL GetCurrentThreadId_c(void) { return 1; }
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
    const char *name = VFS_GAME_DIR "\\i76.exe";
    if ((lpFilename == NULL) || (nSize == 0)) return 0;
    strncpy(lpFilename, name, nSize - 1);
    lpFilename[nSize - 1] = 0;
    return strlen(lpFilename);
}

// drives: C: = fixed disk; D: = CD-ROM drive. Both map to the game directory.
// I76_CD selects what the CD drive contains:
//   unset/2 = an audio CD (label "AUDIO_CD"): the game needs a CD-ROM drive to start the CD music
//             (played from music/*.mp3 by cdaudio.c, like GOG's win32.dll)
//   1       = the game CD (volume "I76_CD2"); with it the shell takes other code paths
//             (e.g. TRAINING starts the game directly)
//   0       = no CD drive (no music)
#define CD_LABEL "I76_CD2"

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

void CCALL InitializeCriticalSection_c(void *lpCriticalSection) {}
void CCALL DeleteCriticalSection_c(void *lpCriticalSection) {}
void CCALL EnterCriticalSection_c(void *lpCriticalSection) {}
void CCALL LeaveCriticalSection_c(void *lpCriticalSection) {}
int32_t CCALL InterlockedIncrement_c(int32_t *lpAddend) { return ++*lpAddend; }
int32_t CCALL InterlockedDecrement_c(int32_t *lpAddend) { return --*lpAddend; }
uint32_t CCALL WaitForSingleObject_c(void *hHandle, uint32_t dwMilliseconds) { return 0; }

static uint32_t tls_values[64];
uint32_t CCALL TlsGetValue_c(uint32_t dwTlsIndex) { return (dwTlsIndex < 64) ? tls_values[dwTlsIndex] : 0; }
uint32_t CCALL TlsSetValue_c(uint32_t dwTlsIndex, uint32_t lpTlsValue) { if (dwTlsIndex >= 64) return 0; tls_values[dwTlsIndex] = lpTlsValue; return 1; }

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
// i76shell.dll
extern void ShellMain(void);
extern void ShellWindowProc(void);
extern uint32_t i76shell_xc_a[];     // 32-bit entries (recompiled code), called through call_game
extern uint32_t i76shell_xc_z[];
// ZGLIDE.DLL
extern void CheckFunc(void), FirstDevice(void), GetFuncDesc(void), GetNumDevice(void), GetSocketCaps(void),
            LastDevice(void), LockDisplay(void), LostDeviceDisplay(void), PreloadTexture(void), RefreshDisplay(void),
            Render(void), RenderNoClip(void), RenderRefresh(void), RestoreDevice(void), SetLumaTable(void),
            SetState(void), SetTexturePalette(void), UnlockDisplay(void), UpdateTexture(void);
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
    uint32_t *xc_a;
    uint32_t *xc_z;
    int loaded;
} module_info;

static const module_export shell_exports[] = {
    { "ShellMain", (void *) ShellMain },
    { "ShellWindowProc", (void *) ShellWindowProc },
    { NULL, NULL }
};

static const module_export zglide_exports[] = {
    { "CheckFunc", (void *) CheckFunc },
    { "FirstDevice", (void *) FirstDevice },
    { "GetFuncDesc", (void *) GetFuncDesc },
    { "GetNumDevice", (void *) GetNumDevice },
    { "GetSocketCaps", (void *) GetSocketCaps },
    { "LastDevice", (void *) LastDevice },
    { "LockDisplay", (void *) LockDisplay },
    { "LostDeviceDisplay", (void *) LostDeviceDisplay },
    { "PreloadTexture", (void *) PreloadTexture },
    { "RefreshDisplay", (void *) RefreshDisplay },
    { "Render", (void *) Render },
    { "RenderNoClip", (void *) RenderNoClip },
    { "RenderRefresh", (void *) RenderRefresh },
    { "RestoreDevice", (void *) RestoreDevice },
    { "SetLumaTable", (void *) SetLumaTable },
    { "SetState", (void *) SetState },
    { "SetTexturePalette", (void *) SetTexturePalette },
    { "UnlockDisplay", (void *) UnlockDisplay },
    { "UpdateTexture", (void *) UpdateTexture },
    { NULL, NULL }
};

static module_info modules[] = {
    { 0, "I76SHELL.DLL", shell_exports, i76shell_xc_a, i76shell_xc_z, 0 },
    { 0, "ZGLIDE.DLL", zglide_exports, NULL, NULL, 0 },
};

static module_info *find_module(const char *name)
{
    const char *base;
    unsigned int i;

    base = strrchr(name, '\\');
    if (base == NULL) base = strrchr(name, '/');
    base = (base != NULL) ? base + 1 : name;

    for (i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
    {
        if (0 == strcasecmp(base, modules[i].name)) return &modules[i];
    }
    return NULL;
}

void * CCALL LoadLibraryA_c(const char *lpLibFileName)
{
    module_info *m;

    if (lpLibFileName == NULL) return NULL;

    m = find_module(lpLibFileName);
    if (winapi_debug) eprintf("LoadLibraryA: %s -> %s\n", lpLibFileName, (m != NULL) ? "ok" : "not available");
    if (m == NULL)
    {
        last_error = 126; // ERROR_MOD_NOT_FOUND
        return NULL;
    }

    if (!m->loaded)
    {
        m->loaded = 1;
        // DllMain equivalent: run the module's C++ static constructors
        if (m->xc_a != NULL)
        {
            uint32_t *p;
            for (p = m->xc_a; p < m->xc_z; p++)
            {
                if (*p != 0) call_game(*p, 0, NULL);
            }
        }
    }
    return m;
}

void * CCALL GetModuleHandleA_c(const char *lpModuleName)
{
    if (lpModuleName == NULL) return (void *)0x400000;
    return find_module(lpModuleName);
}

void * CCALL GetProcAddress_c(module_info *hModule, const char *lpProcName)
{
    const module_export *e;
    unsigned int i;

    for (i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
    {
        if (hModule == &modules[i]) break;
    }
    if ((i == sizeof(modules) / sizeof(modules[0])) || ((uintptr_t)lpProcName < 0x10000)) return NULL;

    for (e = hModule->exports; e->name != NULL; e++)
    {
        if (0 == strcmp(e->name, lpProcName)) return e->address;
    }
    if (winapi_debug) eprintf("GetProcAddress: %s!%s not found\n", hModule->name, lpProcName);
    return NULL;
}

uint32_t CCALL FreeLibrary_c(void *hLibModule)
{
    return 1;
}

EXTERN_C_END
