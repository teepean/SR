/**
 *
 *  Functions missing from some platforms' C libraries (Windows / MinGW).
 *
 */

#include "compat.h"

#ifdef _WIN32

#include <ctype.h>
#include <string.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

static int fold(int c, int flags)
{
    return (flags & FNM_CASEFOLD) ? tolower((unsigned char)c) : (unsigned char)c;
}

int fnmatch(const char *pattern, const char *string, int flags)
{
    const char *star_p = NULL, *star_s = NULL;

    for (;;)
    {
        if (*pattern == '*')
        {
            while (*pattern == '*') pattern++;
            star_p = pattern;
            star_s = string;
            continue;
        }
        if (*string == 0)
        {
            return (*pattern == 0) ? 0 : FNM_NOMATCH;
        }
        if (*pattern == '?')
        {
            pattern++;
            string++;
            continue;
        }
        if (*pattern == '[')
        {
            const char *p = pattern + 1;
            int negate = 0, matched = 0, c = fold(*string, flags);
            if ((*p == '!') || (*p == '^')) { negate = 1; p++; }
            do
            {
                if ((p[1] == '-') && (p[2] != 0) && (p[2] != ']'))
                {
                    if ((c >= fold(p[0], flags)) && (c <= fold(p[2], flags))) matched = 1;
                    p += 3;
                }
                else
                {
                    if (c == fold(*p, flags)) matched = 1;
                    p++;
                }
            } while ((*p != 0) && (*p != ']'));
            if ((*p == ']') && (matched != negate))
            {
                pattern = p + 1;
                string++;
                continue;
            }
        }
        else if ((*pattern != 0) && (fold(*pattern, flags) == fold(*string, flags)))
        {
            pattern++;
            string++;
            continue;
        }
        // mismatch: backtrack to the last '*'
        if (star_p == NULL) return FNM_NOMATCH;
        pattern = star_p;
        string = ++star_s;
    }
}

char *strcasestr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    for (; *haystack; haystack++)
    {
        if (strncasecmp(haystack, needle, n) == 0) return (char *)haystack;
    }
    return (n == 0) ? (char *)haystack : NULL;
}

#ifdef __cplusplus
}
#endif

#endif
