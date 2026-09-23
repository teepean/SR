/**
 *
 *  Mapping of Windows paths used by the game to host paths.
 *
 */

#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

// find entry "name" in directory "dir" case-insensitively; on success copy the real name into "name"
static int find_entry(const char *dir, char *name)
{
    DIR *d;
    struct dirent *entry;
    int found;

    d = opendir((dir[0] != 0) ? dir : ".");
    if (d == NULL) return 0;

    found = 0;
    while ((entry = readdir(d)) != NULL)
    {
        if (0 == strcasecmp(entry->d_name, name))
        {
            strcpy(name, entry->d_name);
            found = 1;
            break;
        }
    }

    closedir(d);
    return found;
}

int vfs_resolve(const char *winpath, char *hostpath, size_t size)
{
    char component[260];
    const char *src;
    size_t len, gamedir_len;
    int exists, clen;

    src = winpath;

    // absolute path with drive letter: X:\...
    if ((((src[0] | 0x20) >= 'a') && ((src[0] | 0x20) <= 'z')) && (src[1] == ':'))
    {
        src += 2;

        // strip the (fake) game directory
        gamedir_len = strlen(VFS_GAME_DIR) - 2;
        if ((0 == strncasecmp(src, VFS_GAME_DIR + 2, gamedir_len)) && ((src[gamedir_len] == 0) || (src[gamedir_len] == '\\') || (src[gamedir_len] == '/')))
        {
            src += gamedir_len;
        }
    }

    while ((*src == '\\') || (*src == '/')) src++;

    hostpath[0] = 0;
    len = 0;
    exists = 1;

    while (*src != 0)
    {
        clen = 0;
        while ((*src != 0) && (*src != '\\') && (*src != '/'))
        {
            if (clen < (int)sizeof(component) - 1) component[clen++] = *src;
            src++;
        }
        component[clen] = 0;
        while ((*src == '\\') || (*src == '/')) src++;

        if ((clen == 0) || (0 == strcmp(component, "."))) continue;

        if (0 == strcmp(component, ".."))
        {
            // remove last component (if any), otherwise keep ".."
            char *slash;

            if (len != 0 && 0 != strcmp(hostpath, "..") && (len < 3 || 0 != strcmp(hostpath + len - 3, "/..")))
            {
                slash = strrchr(hostpath, '/');
                if (slash != NULL)
                {
                    *slash = 0;
                    len = slash - hostpath;
                }
                else
                {
                    hostpath[0] = 0;
                    len = 0;
                }
                continue;
            }
        }
        else if (exists)
        {
            if (!find_entry(hostpath, component))
            {
                exists = 0;
            }
        }

        if (len + (len != 0 ? 1 : 0) + strlen(component) + 1 > size)
        {
            break;
        }

        if (len != 0) hostpath[len++] = '/';
        strcpy(hostpath + len, component);
        len += strlen(component);
    }

    if (len == 0)
    {
        strcpy(hostpath, ".");
    }

    return exists;
}

#ifdef __cplusplus
}
#endif
