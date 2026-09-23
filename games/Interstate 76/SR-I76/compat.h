/**
 *
 *  Functions missing from some platforms' C libraries (Windows / MinGW).
 *
 */

#if !defined(_COMPAT_H_INCLUDED_)
#define _COMPAT_H_INCLUDED_

#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

#define FNM_NOMATCH 1
#define FNM_CASEFOLD 16

// shell wildcard match (*, ?, [...]); only FNM_CASEFOLD is supported
int fnmatch(const char *pattern, const char *string, int flags);
char *strcasestr(const char *haystack, const char *needle);

#ifdef __cplusplus
}
#endif

#else

#include <fnmatch.h>

#endif

#endif /* _COMPAT_H_INCLUDED_ */
