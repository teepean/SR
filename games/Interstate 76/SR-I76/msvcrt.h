/**
 *
 *  MSVCRT emulation for the recompiled modules.
 *
 */

#if !defined(_MSVCRT_H_INCLUDED_)
#define _MSVCRT_H_INCLUDED_

#include <stdint.h>
#include "ptr32.h"

#ifdef __cplusplus
extern "C" {
#endif

// FILE structure of MSVCRT (32 bytes) - the recompiled code may access it directly
// (inline getc/putc macros, feof/ferror macros)
typedef struct {
    PTR32(char) _ptr;
    int32_t _cnt;
    PTR32(char) _base;
    int32_t _flag;
    int32_t _file;
    int32_t _charbuf;
    int32_t _bufsiz;
    PTR32(char) _tmpfname;
} ms_FILE;

void msvcrt_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _MSVCRT_H_INCLUDED_ */
