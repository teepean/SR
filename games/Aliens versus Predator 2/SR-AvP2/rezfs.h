/**
 *
 *  Read access to the game's resource archives (the engine's -rez list) for the runtime's own use.
 *
 */

#if !defined(_REZFS_H_INCLUDED_)
#define _REZFS_H_INCLUDED_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// an archive or directory of the -rez list, in command-line order (a later one overrides an earlier one)
void rezfs_add(const char *winpath);
// a resource ("Music\M1Theme\M1Control.txt", any case) read into a malloc'd buffer (NUL-terminated), NULL if absent
void *rezfs_load(const char *name, size_t *size);

#ifdef __cplusplus
}
#endif

#endif /* _REZFS_H_INCLUDED_ */
