/**
 *
 *  Copyright (C) 2025-2026 Roman Pauer
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 */

#if !defined(_PLATFORM_H_INCLUDED_)
#define _PLATFORM_H_INCLUDED_

#if defined(__GNUC__)
    #define PACKED __attribute__ ((__packed__))
    #if defined(__i386) || (defined(__x86_64) && defined(_WIN32))
        #define CCALL __attribute__ ((__cdecl__))
    #else
        #define CCALL
    #endif
#elif defined(_MSC_VER)
    #define PACKED
    #define CCALL __cdecl
    #define getcwd _getcwd
    #define mkdir _mkdir
    #define putenv _putenv
    #define strcasecmp _stricmp
    #define strdup _strdup
    #define strncasecmp _strnicmp
    #define tzset _tzset
    #ifdef _WIN64
        typedef __int64 ssize_t;
    #else
        typedef int ssize_t;
    #endif
    #if _MSC_VER < 1900
        #define snprintf _snprintf
        #define vsscanf _vsscanf
    #endif
#else
    #define PACKED
    #define CCALL
#endif

// the 64-bit build (SRW OUT_X64) compiles the runtime as C++ (PTR32 types, Game-Memory.c): functions and
// variables used by the recompiled code must keep C linkage
#ifdef __cplusplus
    #define EXTERN_C_BEGIN extern "C" {
    #define EXTERN_C_END }
    #if !defined(_Static_assert)
        #define _Static_assert static_assert
    #endif
#else
    #define EXTERN_C_BEGIN
    #define EXTERN_C_END
#endif

// Game addresses (the recompiled code's 32-bit pointers) <-> the runtime's pointers. With pointer offsets (the llasm
// version with -ptrofs: native arm64 and any 64-bit target) a game address is an offset from pointer_offset (the
// game sees a 4 GB window of the address space); otherwise it's the low address itself. 0 <-> NULL.
#include <stdint.h>
#include <stddef.h>
#ifdef PTROFS_64BIT
extern uint64_t pointer_offset;
static inline void *G2H(uint32_t a) { return (a != 0) ? (void *)((uintptr_t)a + (uintptr_t)pointer_offset) : NULL; }
static inline uint32_t H2G(const void *p) { return (p != NULL) ? (uint32_t)((uintptr_t)p - (uintptr_t)pointer_offset) : 0; }
// a game address that isn't 0 (no test: the hot accessors)
static inline void *GADDR(uint32_t a) { return (void *)((uintptr_t)a + (uintptr_t)pointer_offset); }
// the game's window: [pointer_offset, pointer_offset + 4 GB)
static inline int in_game_window(const void *p) { return ((uintptr_t)p - (uintptr_t)pointer_offset) < UINT64_C(0x100000000); }
#else
static inline void *G2H(uint32_t a) { return (void *)(uintptr_t)a; }
static inline uint32_t H2G(const void *p) { return (uint32_t)(uintptr_t)p; }
static inline void *GADDR(uint32_t a) { return (void *)(uintptr_t)a; }
static inline int in_game_window(const void *p) { return (uintptr_t)p < UINT64_C(0x100000000); }
#endif
// the game's memory isn't the runtime's (blob: the runtime is above 4 GB; pointer offsets): what the game sees is
// allocated in its window (lowalloc.h, lowobj.h)
#if defined(SR_BLOB) || defined(PTROFS_64BIT)
#define SR_GAME_WINDOW 1
#endif

#endif /* _PLATFORM_H_INCLUDED_ */
