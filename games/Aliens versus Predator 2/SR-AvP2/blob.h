/**
 *
 *  The recompiled code as a separate image (SR_BLOB; tools/mkblob.py): loaded at its fixed low address when the
 *  program starts. See asmsyms.h.
 *
 */

#if !defined(_BLOB_H_INCLUDED_)
#define _BLOB_H_INCLUDED_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const char *file;           // next to the program
    uintptr_t base;
    size_t text_size;           // code (from the base): read-only and executable
    size_t file_size;           // the file's bytes (code, read-only data, data)
    size_t size;                // with the zeroed data
} blob_info;

// a thunk's slot in the image and the C function it jumps to
typedef struct
{
    uintptr_t slot;
    void (*fn)(void);
} blob_import;

extern const blob_info blob;
extern const blob_import blob_imports[];
extern const unsigned blob_nimports;

// maps and links the image; 0 on success (prints why not)
int blob_load(void);

#ifdef __cplusplus
}
#endif

#endif /* _BLOB_H_INCLUDED_ */
