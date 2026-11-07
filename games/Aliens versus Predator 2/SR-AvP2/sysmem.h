/**
 *
 *  Host memory mappings visible to the game (below 2 GB): Linux (mmap) and Windows (VirtualAlloc).
 *
 */

#if !defined(_SYSMEM_H_INCLUDED_)
#define _SYSMEM_H_INCLUDED_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// read/write memory below 2 GB; with fd >= 0 it contains the file's data from offset (a private copy:
// changes aren't written to the file); NULL on failure
void *sys_map_low(size_t len, int fd, int64_t offset);
void sys_unmap(void *addr, size_t len);
// makes the pages inaccessible (guard pages)
void sys_protect_none(void *addr, size_t len);
// Windows: logs crashes (registers, stacks) to stderr
void sys_install_crash_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* _SYSMEM_H_INCLUDED_ */
