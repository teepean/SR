/**
 *
 *  Mapping of Windows paths used by the game to host paths.
 *
 */

#if !defined(_VFS_H_INCLUDED_)
#define _VFS_H_INCLUDED_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Directory the game sees as its install directory (Windows form, no trailing backslash). */
#define VFS_GAME_DIR "C:\\I76"

/*
 * Converts a Windows path (absolute with drive letter, or relative to the game directory)
 * into a host path relative to the current directory (= game data directory).
 * Every path component is matched case-insensitively against existing files/directories.
 * Returns 1 if the whole path exists, 0 otherwise (the result is still filled in, with the
 * missing components copied as given - suitable for creating new files).
 */
int vfs_resolve(const char *winpath, char *hostpath, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* _VFS_H_INCLUDED_ */
