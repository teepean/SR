/**
 *
 *  MSVCRT emulation for the recompiled modules.
 *
 *  Used both for i76.exe/i76shell.dll imports of MSVCRT.DLL and for the statically linked
 *  CRT functions of ZGLIDE.DLL/STRLKUP.DLL (redirected with external_procedures.sci).
 *
 *  Note: scanf family passes the x86 argument area directly as va_list - valid for the
 *  i386 build only (pc-linux).
 *
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "platform.h"
#include "printf_x86.h"
#include "vfs.h"
#include "msvcrt.h"
#include "Game-Memory.h"
#include "winapi.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)


/* ------------------------------------------------------------------ */
/* data imports                                                        */

// MS ctype flags
#define MS_UPPER   0x01
#define MS_LOWER   0x02
#define MS_DIGIT   0x04
#define MS_SPACE   0x08
#define MS_PUNCT   0x10
#define MS_CONTROL 0x20
#define MS_BLANK   0x40
#define MS_HEX     0x80
#define MS_ALPHA   0x100

static uint16_t ms_ctype_table[257];     // index 0 = EOF (-1)
PTR32(uint16_t) msvcrt__pctype = ms_ctype_table + 1;    // data import: a 32-bit pointer variable
uint8_t msvcrt__mbctype[257];           // single-byte code page: no lead bytes
int32_t msvcrt___mb_cur_max = 1;
ms_FILE msvcrt__iob[3];


/* ------------------------------------------------------------------ */
/* FILE emulation                                                      */

#define MS_IOREAD  0x0001
#define MS_IOWRT   0x0002
#define MS_IOEOF   0x0010
#define MS_IOERR   0x0020
#define MS_IORW    0x0080

// the game sees ms_FILE only; the host FILE pointer follows it in the (low memory) allocation, or is in
// iob_host for the standard streams. The text mode flag is kept in _charbuf.
typedef struct {
    ms_FILE pub;
    FILE *host;
} ms_FILE_ext;

static FILE *iob_host[3];

static FILE *hostfile(ms_FILE *f)
{
    if ((f >= msvcrt__iob) && (f < msvcrt__iob + 3)) return iob_host[f - msvcrt__iob];
    return ((ms_FILE_ext *)f)->host;
}
#define HOSTFILE(f) hostfile(f)
#define TEXTMODE(f) ((f)->_charbuf)

static int is_std_file(ms_FILE *f)
{
    return (f >= msvcrt__iob) && (f < msvcrt__iob + 3);
}

void msvcrt_init(void)
{
    int c;

    for (c = 0; c < 256; c++)
    {
        uint16_t v = 0;
        if (c < 128)
        {
            if (isupper(c)) v |= MS_UPPER | MS_ALPHA;
            if (islower(c)) v |= MS_LOWER | MS_ALPHA;
            if (isdigit(c)) v |= MS_DIGIT;
            if (isspace(c)) v |= MS_SPACE;
            if (ispunct(c)) v |= MS_PUNCT;
            if (iscntrl(c)) v |= MS_CONTROL;
            if (c == ' ' || c == '\t') v |= MS_BLANK;
            if (isxdigit(c)) v |= MS_HEX;
        }
        ms_ctype_table[c + 1] = v;
    }
    ms_ctype_table[0] = 0;
    memset(msvcrt__mbctype, 0, sizeof(msvcrt__mbctype));

    memset(msvcrt__iob, 0, sizeof(msvcrt__iob));
    iob_host[0] = stdin;
    msvcrt__iob[0]._flag = MS_IOREAD;
    msvcrt__iob[0]._file = 0;
    iob_host[1] = stdout;
    msvcrt__iob[1]._flag = MS_IOWRT;
    msvcrt__iob[1]._file = 1;
    iob_host[2] = stderr;
    msvcrt__iob[2]._flag = MS_IOWRT;
    msvcrt__iob[2]._file = 2;
}

static void update_flags(ms_FILE *f)
{
    if (feof(HOSTFILE(f))) f->_flag |= MS_IOEOF; else f->_flag &= ~MS_IOEOF;
    if (ferror(HOSTFILE(f))) f->_flag |= MS_IOERR; else f->_flag &= ~MS_IOERR;
    f->_cnt = 0;
}

// reads one character; in text mode CR LF -> LF, and Ctrl-Z is end of file
static int file_getc(ms_FILE *f)
{
    int c;
    FILE *hf = HOSTFILE(f);

    c = getc(hf);
    if (TEXTMODE(f))
    {
        if (c == '\r')
        {
            int c2 = getc(hf);
            if (c2 == '\n') c = '\n';
            else if (c2 != EOF) ungetc(c2, hf);
        }
        else if (c == 0x1a)
        {
            ungetc(c, hf);
            c = EOF;
            f->_flag |= MS_IOEOF;
            f->_cnt = 0;
            return EOF;
        }
    }
    update_flags(f);
    return c;
}

ms_FILE * CCALL fopen_c(const char *filename, const char *mode)
{
    char path[1024];
    char hmode[8];
    int i, j, text;
    FILE *hf;
    ms_FILE *f;

    if ((filename == NULL) || (mode == NULL)) return NULL;

    vfs_resolve(filename, path, sizeof(path));
    {
        // Windows can't open directories with fopen
        struct stat st;
        if ((stat(path, &st) == 0) && S_ISDIR(st.st_mode)) return NULL;
    }

    text = 1;
    for (i = j = 0; mode[i] != 0 && j < 6; i++)
    {
        if (mode[i] == 'b') text = 0;
        else if (mode[i] == 't') text = 1;
        else if (mode[i] == 'r' || mode[i] == 'w' || mode[i] == 'a' || mode[i] == '+') hmode[j++] = mode[i];
    }
    hmode[j++] = 'b';
    hmode[j] = 0;

    hf = fopen(path, hmode);
#if defined(__DEBUG__)
    eprintf("fopen: %s (%s) -> %s: %s\n", filename, mode, path, (hf != NULL) ? "ok" : "failed");
#else
    if (winapi_debug && ((strchr(mode, 'w') != NULL) || (strchr(mode, 'a') != NULL) || (hf == NULL)))
        eprintf("fopen: %s (%s) -> %s: %s\n", filename, mode, path, (hf != NULL) ? "ok" : "failed");
#endif
    if (hf == NULL) return NULL;

    f = (ms_FILE *) game_calloc(1, sizeof(ms_FILE_ext));
    if (f == NULL)
    {
        fclose(hf);
        return NULL;
    }

    ((ms_FILE_ext *)f)->host = hf;
    f->_charbuf = text;
    f->_flag = (strchr(hmode, '+') != NULL) ? MS_IORW : ((hmode[0] == 'r') ? MS_IOREAD : MS_IOWRT);
    f->_file = fileno(hf);
    return f;
}

int32_t CCALL fclose_c(ms_FILE *f)
{
    int res;

    if (f == NULL) return -1;
    if (is_std_file(f)) return 0;

    res = fclose(HOSTFILE(f));
    game_free(f);
    return res;
}

uint32_t CCALL fread_c(void *ptr, uint32_t size, uint32_t nmemb, ms_FILE *f)
{
    size_t res;

    if ((f == NULL) || (size == 0) || (nmemb == 0)) return 0;

    if (TEXTMODE(f))
    {
        uint8_t *dst = (uint8_t *) ptr;
        uint32_t total = size * nmemb, n;
        int c;

        for (n = 0; n < total; n++)
        {
            c = file_getc(f);
            if (c == EOF) break;
            dst[n] = (uint8_t) c;
        }
        return n / size;
    }

    res = fread(ptr, size, nmemb, HOSTFILE(f));
    update_flags(f);
    return res;
}

uint32_t CCALL fwrite_c(const void *ptr, uint32_t size, uint32_t nmemb, ms_FILE *f)
{
    size_t res;

    if (f == NULL) return 0;
    res = fwrite(ptr, size, nmemb, HOSTFILE(f));
    update_flags(f);
    return res;
}

int32_t CCALL fseek_c(ms_FILE *f, int32_t offset, int32_t whence)
{
    int res;

    if (f == NULL) return -1;
    res = fseek(HOSTFILE(f), offset, whence);
    if (res == 0) f->_flag &= ~MS_IOEOF;
    f->_cnt = 0;
    return res ? -1 : 0;
}

int32_t CCALL ftell_c(ms_FILE *f)
{
    if (f == NULL) return -1;
    return (int32_t) ftell(HOSTFILE(f));
}

char * CCALL fgets_c(char *s, int32_t size, ms_FILE *f)
{
    int n, c;

    if ((f == NULL) || (s == NULL) || (size <= 0)) return NULL;

    n = 0;
    while (n < size - 1)
    {
        c = file_getc(f);
        if (c == EOF) break;
        s[n++] = (char) c;
        if (c == '\n') break;
    }
    s[n] = 0;

    return (n == 0) ? NULL : s;
}

int32_t CCALL fgetc_c(ms_FILE *f)
{
    if (f == NULL) return EOF;
    return file_getc(f);
}

int32_t CCALL getc_c(ms_FILE *f)
{
    return fgetc_c(f);
}

int32_t CCALL _filbuf_c(ms_FILE *f)
{
    // called by the inline getc macro when _cnt goes negative
    if (f == NULL) return EOF;
    f->_cnt = 0;
    return file_getc(f);
}

int32_t CCALL ungetc_c(int32_t c, ms_FILE *f)
{
    int res;

    if ((f == NULL) || (c == EOF)) return EOF;
    res = ungetc(c, HOSTFILE(f));
    f->_flag &= ~MS_IOEOF;
    f->_cnt = 0;
    return res;
}

int32_t CCALL fputc_c(int32_t c, ms_FILE *f)
{
    int res;

    if (f == NULL) return EOF;
    res = fputc(c, HOSTFILE(f));
    update_flags(f);
    return res;
}

int32_t CCALL fputs_c(const char *s, ms_FILE *f)
{
    int res;

    if ((f == NULL) || (s == NULL)) return EOF;
    res = fputs(s, HOSTFILE(f));
    update_flags(f);
    return (res < 0) ? EOF : 0;
}

int32_t CCALL fflush_c(ms_FILE *f)
{
    if (f == NULL) return fflush(NULL);
    return fflush(HOSTFILE(f));
}

int32_t CCALL setvbuf_c(ms_FILE *f, char *buf, int32_t mode, uint32_t size)
{
    // MS: _IOFBF 0, _IOLBF 0x40, _IONBF 4 - keep the host buffering
    return 0;
}

static void out_file(char character, void *arg)
{
    fputc(character, (FILE *) arg);
}

int32_t CCALL fprintf_c(ms_FILE *f, const char *format, uint32_t *ap)
{
    int res;

    if ((f == NULL) || (format == NULL)) return -1;
    res = vfctprintf_x86(out_file, HOSTFILE(f), format, ap);
    update_flags(f);
    return res;
}

int32_t CCALL printf_c(const char *format, uint32_t *ap)
{
    if (format == NULL) return -1;
    return vfctprintf_x86(out_file, stdout, format, ap);
}

/* scanf: the game's arguments are 32-bit pointers into game memory. The format is translated to the host
   (MS 'l' on integers = 32 bits, host long is 64 bits on x86-64) and the host function is called with an
   explicit argument list (a host va_list can't be built from the game's stack portably). */
#define SCANF_MAX_ARGS 32

static int scanf_prepare(const char *format, const uint32_t *ap, char *hostfmt, size_t hostfmt_size, void **args)
{
    const char *p = format;
    char *o = hostfmt, *end = hostfmt + hostfmt_size - 1;
    int n = 0;

    while (*p && (o < end))
    {
        if (*p != '%') { *o++ = *p++; continue; }
        *o++ = *p++;
        if (*p == '%') { if (o < end) *o++ = *p++; continue; }
        {
            int suppress = 0;
            if (*p == '*') { suppress = 1; if (o < end) *o++ = *p++; }
            while ((*p >= '0') && (*p <= '9') && (o < end)) *o++ = *p++;
            if (*p == 'h') { if (o < end) *o++ = *p++; }
            else if ((*p == 'l') && (p[1] != 0) && (strchr("diouxXn", p[1]) != NULL)) p++;     // MS long = 32 bits
            else if (*p == 'l') { if (o < end) *o++ = *p++; }                                // %lf = double
            else if ((p[0] == 'I') && (p[1] == '6') && (p[2] == '4')) { p += 3; if (o + 1 < end) { *o++ = 'l'; *o++ = 'l'; } }
            if (*p == '[')
            {
                if (o < end) *o++ = *p++;
                if ((*p == '^') && (o < end)) *o++ = *p++;
                if ((*p == ']') && (o < end)) *o++ = *p++;
                while (*p && (*p != ']') && (o < end)) *o++ = *p++;
            }
            if (*p && (o < end)) *o++ = *p++;
            if (!suppress && (n < SCANF_MAX_ARGS)) { args[n] = (void *)(uintptr_t) ap[n]; n++; }
        }
    }
    *o = 0;
    while (n < SCANF_MAX_ARGS) args[n++] = NULL;
    return n;
}

#define SCANF_ARGS(a) a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15], \
                      a[16], a[17], a[18], a[19], a[20], a[21], a[22], a[23], a[24], a[25], a[26], a[27], a[28], a[29], a[30], a[31]

int32_t CCALL fscanf_c(ms_FILE *f, const char *format, uint32_t *ap)
{
    int res;
    char hostfmt[512];
    void *args[SCANF_MAX_ARGS];

    if ((f == NULL) || (format == NULL)) return -1;
    // text mode needs no translation here: CR is white space for scanf
    scanf_prepare(format, ap, hostfmt, sizeof(hostfmt), args);
    res = fscanf(HOSTFILE(f), hostfmt, SCANF_ARGS(args));
    update_flags(f);
    return res;
}


/* ------------------------------------------------------------------ */
/* string formatting                                                   */

int32_t CCALL sprintf_c(char *str, const char *format, uint32_t *ap)
{
    return vsprintf_x86(str, format, ap);
}

int32_t CCALL vsprintf_c(char *str, const char *format, uint32_t *ap)
{
    return vsprintf_x86(str, format, ap);
}

int32_t CCALL _vsnprintf_c(char *str, uint32_t size, const char *format, uint32_t *ap)
{
    int res;

    res = vsnprintf_x86(str, size, format, ap);
    // MS: returns -1 if the output was truncated
    return (res < 0 || (uint32_t)res >= size) ? -1 : res;
}

int32_t CCALL sscanf_c(const char *str, const char *format, uint32_t *ap)
{
    char hostfmt[512];
    void *args[SCANF_MAX_ARGS];

    if ((str == NULL) || (format == NULL)) return -1;
    scanf_prepare(format, ap, hostfmt, sizeof(hostfmt), args);
    return sscanf(str, hostfmt, SCANF_ARGS(args));
}


/* ------------------------------------------------------------------ */
/* strings                                                             */

char * CCALL strchr_c(const char *s, int32_t c) { return (char *) strchr(s, c); }
char * CCALL strrchr_c(const char *s, int32_t c) { return (char *) strrchr(s, c); }
char * CCALL strstr_c(const char *s1, const char *s2) { return (char *) strstr(s1, s2); }
char * CCALL strpbrk_c(const char *s, const char *accept) { return (char *) strpbrk(s, accept); }
uint32_t CCALL strspn_c(const char *s, const char *accept) { return strspn(s, accept); }
uint32_t CCALL strcspn_c(const char *s, const char *reject) { return strcspn(s, reject); }
char * CCALL strncpy_c(char *dest, const char *src, uint32_t n) { return strncpy(dest, src, n); }
char * CCALL strncat_c(char *dest, const char *src, uint32_t n) { return strncat(dest, src, n); }
int32_t CCALL strncmp_c(const char *s1, const char *s2, uint32_t n) { return strncmp(s1, s2, n); }
int32_t CCALL _stricmp_c(const char *s1, const char *s2) { return strcasecmp(s1, s2); }
int32_t CCALL _strnicmp_c(const char *s1, const char *s2, uint32_t n) { return strncasecmp(s1, s2, n); }
void * CCALL memmove_c(void *dest, const void *src, uint32_t n) { return memmove(dest, src, n); }
void * CCALL memcpy_c(void *dest, const void *src, uint32_t n) { return memmove(dest, src, n); }

static char *strtok_state;
char * CCALL strtok_c(char *s, const char *delim) { return strtok_r(s, delim, &strtok_state); }

char * CCALL _strlwr_c(char *s)
{
    char *p;
    if (s == NULL) return NULL;
    for (p = s; *p; p++) *p = tolower((unsigned char)*p);
    return s;
}

int32_t CCALL tolower_c(int32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int32_t CCALL toupper_c(int32_t c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int32_t CCALL isspace_c(int32_t c) { return (c >= -1 && c < 256) ? (msvcrt__pctype[c] & MS_SPACE) : 0; }
int32_t CCALL _isctype_c(int32_t c, int32_t mask) { return (c >= -1 && c < 256) ? (msvcrt__pctype[c] & mask) : 0; }
int32_t CCALL atoi_c(const char *s) { return atoi(s); }
int32_t CCALL atol_c(const char *s) { return (int32_t) atol(s); }

char * CCALL _itoa_c(int32_t value, char *str, int32_t radix)
{
    char tmp[36];
    uint32_t v;
    int i, neg;

    if (str == NULL) return NULL;
    if (radix < 2 || radix > 36) { str[0] = 0; return str; }

    neg = (radix == 10 && value < 0);
    v = neg ? (uint32_t)(-value) : (uint32_t)value;
    i = 0;
    do
    {
        int d = v % radix;
        tmp[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        v /= radix;
    } while (v != 0);

    {
        int j = 0;
        if (neg) str[j++] = '-';
        while (i > 0) str[j++] = tmp[--i];
        str[j] = 0;
    }
    return str;
}

// multibyte functions: the game runs with a single-byte code page
int32_t CCALL _ismbcalnum_c(uint32_t c) { return (c < 256) ? (msvcrt__pctype[c] & (MS_ALPHA | MS_DIGIT)) : 0; }
int32_t CCALL _ismbcpunct_c(uint32_t c) { return (c < 256) ? (msvcrt__pctype[c] & MS_PUNCT) : 0; }
int32_t CCALL _ismbcspace_c(uint32_t c) { return (c < 256) ? (msvcrt__pctype[c] & MS_SPACE) : 0; }
int32_t CCALL _mbsicmp_c(const char *s1, const char *s2) { return strcasecmp(s1, s2); }
int32_t CCALL _mbsnbicmp_c(const char *s1, const char *s2, uint32_t n) { return strncasecmp(s1, s2, n); }
char * CCALL _mbsnbcpy_c(char *dest, const char *src, uint32_t n) { return strncpy(dest, src, n); }
char * CCALL _mbsnbcat_c(char *dest, const char *src, uint32_t n) { return strncat(dest, src, n); }

char * CCALL setlocale_c(int32_t category, const char *locale)
{
    static char c_locale[] = "C";
    return c_locale;
}


/* ------------------------------------------------------------------ */
/* memory                                                              */

// the CRT heap uses the emulated Win32 heap (winapi-kernel32.c): exact _msize, zeroed blocks with slack, and
// invalid/double frees are rejected instead of corrupting the host malloc (the game has some)
void * CCALL HeapCreate_c(uint32_t flOptions, uint32_t dwInitialSize, uint32_t dwMaximumSize);
void * CCALL HeapAlloc_c(void *hHeap, uint32_t dwFlags, uint32_t dwBytes);
uint32_t CCALL HeapFree_c(void *hHeap, uint32_t dwFlags, void *lpMem);
void * CCALL HeapReAlloc_c(void *hHeap, uint32_t dwFlags, void *lpMem, uint32_t dwBytes);
uint32_t CCALL HeapSize_c(void *hHeap, uint32_t dwFlags, const void *lpMem);

static void *crt_heap;

static void *get_crt_heap(void)
{
    if (crt_heap == NULL) crt_heap = HeapCreate_c(0, 0, 0);
    return crt_heap;
}

void * CCALL malloc_c(uint32_t size)
{
    return HeapAlloc_c(get_crt_heap(), 0, size);
}

void CCALL free_c(void *ptr)
{
    if (ptr != NULL) HeapFree_c(get_crt_heap(), 0, ptr);
}

void * CCALL calloc_c(uint32_t nmemb, uint32_t size)
{
    uint64_t total = (uint64_t)nmemb * size;
    if (total > 0x7fffffff) return NULL;
    return malloc_c((uint32_t) total);     // HeapAlloc zeroes
}

void * CCALL realloc_c(void *ptr, uint32_t size)
{
    if (ptr == NULL) return malloc_c(size);
    if (size == 0)
    {
        free_c(ptr);
        return NULL;
    }
    return HeapReAlloc_c(get_crt_heap(), 0, ptr, size);
}

uint32_t CCALL _msize_c(void *ptr)
{
    if (ptr == NULL) return (uint32_t)-1;
    return HeapSize_c(get_crt_heap(), 0, ptr);
}

void * CCALL operator_new_c(uint32_t size)
{
    return malloc_c(size ? size : 1);
}

void CCALL operator_delete_c(void *ptr)
{
    free_c(ptr);
}


/* ------------------------------------------------------------------ */
/* sort/search (callbacks are recompiled cdecl functions)              */
/*                                                                     */
/* These reproduce the MSVC CRT algorithms exactly: the game's         */
/* comparators return the difference of two dwords (which overflows),  */
/* so the resulting order - and whether bsearch finds an element -     */
/* depends on the algorithm (glibc's merge sort gives other results).  */

// comparators are game code (cdecl): 32-bit function addresses called through call_game
typedef uint32_t compare_func;

static int call_compare(compare_func f, const void *a, const void *b)
{
    uint32_t args[2] = { (uint32_t)(uintptr_t) a, (uint32_t)(uintptr_t) b };
    return (int32_t) call_game(f, 2, args);
}

static void ms_swap(char *a, char *b, uint32_t width)
{
    char tmp;
    if (a != b)
    {
        while (width--)
        {
            tmp = *a;
            *a++ = *b;
            *b++ = tmp;
        }
    }
}

static void ms_shortsort(char *lo, char *hi, uint32_t width, compare_func comp)
{
    char *p, *max;

    while (hi > lo)
    {
        max = lo;
        for (p = lo + width; p <= hi; p += width)
        {
            if (call_compare(comp, p, max) > 0) max = p;
        }
        ms_swap(max, hi, width);
        hi -= width;
    }
}

void CCALL qsort_c(void *base, uint32_t num, uint32_t width, compare_func comp)
{
    char *lo, *hi, *mid, *loguy, *higuy;
    uint32_t size;
    char *lostk[32], *histk[32];
    int stkptr;

    if ((num < 2) || (width == 0)) return;

    stkptr = 0;
    lo = (char *) base;
    hi = (char *) base + width * (num - 1);

recurse:
    size = (uint32_t)((hi - lo) / width) + 1;

    if (size <= 8)
    {
        ms_shortsort(lo, hi, width, comp);
    }
    else
    {
        mid = lo + (size / 2) * width;
        ms_swap(mid, lo, width);

        loguy = lo;
        higuy = hi + width;

        for (;;)
        {
            do
            {
                loguy += width;
            } while ((loguy <= hi) && (call_compare(comp, loguy, lo) <= 0));

            do
            {
                higuy -= width;
            } while ((higuy > lo) && (call_compare(comp, higuy, lo) >= 0));

            if (higuy < loguy) break;

            ms_swap(loguy, higuy, width);
        }

        ms_swap(lo, higuy, width);

        if (higuy - 1 - lo >= hi - loguy)
        {
            if (lo + width < higuy)
            {
                lostk[stkptr] = lo;
                histk[stkptr] = higuy - width;
                ++stkptr;
            }
            if (loguy < hi)
            {
                lo = loguy;
                goto recurse;
            }
        }
        else
        {
            if (loguy < hi)
            {
                lostk[stkptr] = loguy;
                histk[stkptr] = hi;
                ++stkptr;
            }
            if (lo + width < higuy)
            {
                hi = higuy - width;
                goto recurse;
            }
        }
    }

    --stkptr;
    if (stkptr >= 0)
    {
        lo = lostk[stkptr];
        hi = histk[stkptr];
        goto recurse;
    }
}

void * CCALL bsearch_c(const void *key, const void *base, uint32_t num, uint32_t width, compare_func compare)
{
    char *lo = (char *) base;
    char *hi = (char *) base + (num - 1) * width;
    char *mid;
    uint32_t half;
    int result;

    while (lo <= hi)
    {
        if ((half = num / 2) != 0)
        {
            mid = lo + ((num & 1) ? half : (half - 1)) * width;
            if (!(result = call_compare(compare, key, mid)))
            {
                return mid;
            }
            else if (result < 0)
            {
                hi = mid - width;
                num = (num & 1) ? half : half - 1;
            }
            else
            {
                lo = mid + width;
                num = half;
            }
        }
        else if (num)
        {
            return call_compare(compare, key, lo) ? NULL : lo;
        }
        else
        {
            break;
        }
    }
    return NULL;
}


/* ------------------------------------------------------------------ */
/* math                                                                */

static double dword2double(uint32_t lo, uint32_t hi)
{
    union { uint64_t u; double d; } v;
    v.u = ((uint64_t)hi << 32) | lo;
    return v.d;
}

double CCALL floor_c(uint32_t lo, uint32_t hi) { return floor(dword2double(lo, hi)); }
int32_t CCALL _isnan_c(uint32_t lo, uint32_t hi) { return isnan(dword2double(lo, hi)) ? 1 : 0; }
double CCALL difftime_c(int32_t t1, int32_t t0) { return (double)t1 - (double)t0; }

// FPU intrinsics: args[0] = st0, args[1] = st1; result goes to args[0]
void CCALL _CIacos_c(double *args) { args[0] = acos(args[0]); }
void CCALL _CIasin_c(double *args) { args[0] = asin(args[0]); }
void CCALL _CIatan2_c(double *args) { args[0] = atan2(args[1], args[0]); }
void CCALL _CIfmod_c(double *args) { args[0] = fmod(args[1], args[0]); }
void CCALL _CIpow_c(double *args) { args[0] = pow(args[1], args[0]); }

uint64_t CCALL msvcrt_div_c(int32_t num, int32_t denom)
{
    // div_t is returned in edx:eax
    uint32_t quot, rem;
    quot = (uint32_t)(num / denom);
    rem = (uint32_t)(num % denom);
    return ((uint64_t)rem << 32) | quot;
}


/* ------------------------------------------------------------------ */
/* random numbers (MS algorithm, RAND_MAX = 0x7fff)                    */

static uint32_t rand_seed = 1;

void CCALL srand_c(uint32_t seed) { rand_seed = seed; }

int32_t CCALL rand_c(void)
{
    rand_seed = rand_seed * 214013 + 2531011;
    return (rand_seed >> 16) & 0x7fff;
}


/* ------------------------------------------------------------------ */
/* time                                                                */

int32_t CCALL time_c(int32_t *t)
{
    int32_t now = (int32_t) time(NULL);
    if (t != NULL) *t = now;
    return now;
}

int32_t CCALL clock_c(void)
{
    // CLOCKS_PER_SEC = 1000
    static struct timespec start;
    static int initialized;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!initialized)
    {
        start = now;
        initialized = 1;
    }
    return (int32_t)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
}


/* ------------------------------------------------------------------ */
/* low level io                                                        */

#define MS_O_WRONLY  0x0001
#define MS_O_RDWR    0x0002
#define MS_O_APPEND  0x0008
#define MS_O_CREAT   0x0100
#define MS_O_TRUNC   0x0200
#define MS_O_EXCL    0x0400

int32_t CCALL _open_c(const char *filename, int32_t oflag, uint32_t *ap)
{
    char path[1024];
    int flags;

    if (filename == NULL) return -1;
    vfs_resolve(filename, path, sizeof(path));

    flags = (oflag & MS_O_RDWR) ? O_RDWR : ((oflag & MS_O_WRONLY) ? O_WRONLY : O_RDONLY);
    if (oflag & MS_O_APPEND) flags |= O_APPEND;
    if (oflag & MS_O_CREAT) flags |= O_CREAT;
    if (oflag & MS_O_TRUNC) flags |= O_TRUNC;
    if (oflag & MS_O_EXCL) flags |= O_EXCL;

    return open(path, flags, 0644);
}

int32_t CCALL _close_c(int32_t fd) { return close(fd); }
int32_t CCALL _read_c(int32_t fd, void *buf, uint32_t count) { return (int32_t) read(fd, buf, count); }
int32_t CCALL _lseek_c(int32_t fd, int32_t offset, int32_t whence) { return (int32_t) lseek(fd, offset, whence); }

int32_t CCALL _access_c(const char *filename, int32_t mode)
{
    char path[1024];
    if (filename == NULL) return -1;
    vfs_resolve(filename, path, sizeof(path));
    return access(path, mode & 6);
}

int32_t CCALL _mkdir_c(const char *dirname)
{
    char path[1024];
    if (dirname == NULL) return -1;
    vfs_resolve(dirname, path, sizeof(path));
    return mkdir(path, 0755);
}

int32_t CCALL _unlink_c(const char *filename)
{
    char path[1024];
    if (filename == NULL) return -1;
    if (!vfs_resolve(filename, path, sizeof(path))) return -1;
    return unlink(path);
}

#pragma pack(push, 1)
typedef struct {
    uint32_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    int16_t st_nlink;
    int16_t st_uid;
    int16_t st_gid;
    uint16_t pad;
    uint32_t st_rdev;
    int32_t st_size;
    int32_t st_atime_;
    int32_t st_mtime_;
    int32_t st_ctime_;
} ms_stat;
#pragma pack(pop)

int32_t CCALL _stat_c(const char *filename, ms_stat *buf)
{
    char path[1024];
    struct stat st;

    if ((filename == NULL) || (buf == NULL)) return -1;
    if (!vfs_resolve(filename, path, sizeof(path))) { errno = ENOENT; return -1; }
    if (stat(path, &st) != 0) return -1;

    memset(buf, 0, sizeof(ms_stat));
    buf->st_mode = (S_ISDIR(st.st_mode) ? 0x4000 : 0x8000) | 0x0100 | 0x0080 | (S_ISDIR(st.st_mode) ? 0x0040 : 0);
    buf->st_nlink = 1;
    buf->st_dev = buf->st_rdev = 2; // drive C:
    buf->st_size = (int32_t) st.st_size;
    buf->st_atime_ = (int32_t) st.st_atime;
    buf->st_mtime_ = (int32_t) st.st_mtime;
    buf->st_ctime_ = (int32_t) st.st_ctime;
    return 0;
}

// _findfirst/_findnext
#pragma pack(push, 1)
typedef struct {
    uint32_t attrib;
    int32_t time_create;
    int32_t time_access;
    int32_t time_write;
    uint32_t size;
    char name[260];
} ms_finddata_t;
#pragma pack(pop)

typedef struct {
    DIR *dir;
    char dirpath[1024];
    char pattern[260];
} find_handle;

static int find_next_entry(find_handle *h, ms_finddata_t *data)
{
    struct dirent *entry;
    struct stat st;
    char full[1300];

    while ((entry = readdir(h->dir)) != NULL)
    {
        if (0 != fnmatch(h->pattern, entry->d_name, FNM_CASEFOLD)) continue;

        snprintf(full, sizeof(full), "%s/%s", h->dirpath, entry->d_name);
        if (stat(full, &st) != 0) continue;

        memset(data, 0, sizeof(ms_finddata_t));
        data->attrib = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        data->time_create = data->time_access = data->time_write = (int32_t) st.st_mtime;
        data->size = (uint32_t) st.st_size;
        strncpy(data->name, entry->d_name, 259);
        return 0;
    }
    return -1;
}

int32_t CCALL _findfirst_c(const char *filespec, ms_finddata_t *data)
{
    char spec[1024];
    char *slash;
    find_handle *h;

    if ((filespec == NULL) || (data == NULL)) return -1;

    h = (find_handle *) game_calloc(1, sizeof(find_handle));
    if (h == NULL) return -1;

    // resolve the directory part, keep the pattern part as given
    strncpy(spec, filespec, sizeof(spec) - 1);
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
    if ((h->dir == NULL) || (find_next_entry(h, data) != 0))
    {
        if (h->dir != NULL) closedir(h->dir);
        game_free(h);
        errno = ENOENT;
        return -1;
    }

    return (int32_t)(uintptr_t) h;
}

int32_t CCALL _findnext_c(int32_t handle, ms_finddata_t *data)
{
    if ((handle == -1) || (handle == 0) || (data == NULL)) return -1;
    return find_next_entry((find_handle *)(uintptr_t) handle, data);
}

int32_t CCALL _findclose_c(int32_t handle)
{
    find_handle *h;

    if ((handle == -1) || (handle == 0)) return -1;
    h = (find_handle *)(uintptr_t) handle;
    if (h->dir != NULL) closedir(h->dir);
    game_free(h);
    return 0;
}

void CCALL _splitpath_c(const char *path, char *drive, char *dir, char *fname, char *ext)
{
    const char *p, *last_slash, *dot;
    size_t len;

    if (path == NULL) return;

    p = path;
    if (p[0] != 0 && p[1] == ':')
    {
        if (drive != NULL) { drive[0] = p[0]; drive[1] = ':'; drive[2] = 0; }
        p += 2;
    }
    else if (drive != NULL) drive[0] = 0;

    last_slash = NULL;
    dot = NULL;
    {
        const char *q;
        for (q = p; *q; q++)
        {
            if (*q == '\\' || *q == '/') { last_slash = q; dot = NULL; }
            else if (*q == '.') dot = q;
        }
    }

    if (last_slash != NULL)
    {
        len = last_slash + 1 - p;
        if (dir != NULL) { if (len > 255) len = 255; memcpy(dir, p, len); dir[len] = 0; }
        p = last_slash + 1;
    }
    else if (dir != NULL) dir[0] = 0;

    if (dot != NULL)
    {
        len = dot - p;
        if (fname != NULL) { if (len > 255) len = 255; memcpy(fname, p, len); fname[len] = 0; }
        if (ext != NULL) { strncpy(ext, dot, 255); ext[255] = 0; }
    }
    else
    {
        if (fname != NULL) { strncpy(fname, p, 255); fname[255] = 0; }
        if (ext != NULL) ext[0] = 0;
    }
}


/* ------------------------------------------------------------------ */
/* process                                                             */

// errno is thread-local (high memory in the 64-bit build): the game gets a low copy
static int32_t ms_errno;
int32_t * CCALL _errno_c(void) { ms_errno = errno; return &ms_errno; }

void CCALL exit_c(int32_t status)
{
    fflush(NULL);
    exit(status);
}

void CCALL abort_c(void)
{
    eprintf("abort() called\n");
    fflush(NULL);
    exit(3);
}

void CCALL _assert_c(const char *expr, const char *file, uint32_t line)
{
    eprintf("Assertion failed: %s, file %s, line %u\n", (expr != NULL) ? expr : "?", (file != NULL) ? file : "?", line);
    fflush(NULL);
    exit(3);
}

int32_t CCALL _putch_c(int32_t c)
{
    putchar(c);
    return c;
}

EXTERN_C_END
