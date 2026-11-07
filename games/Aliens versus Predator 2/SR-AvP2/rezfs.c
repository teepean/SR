/**
 *
 *  Read access to the game's resource archives for the runtime's own use (music.c's DirectMusic files): the
 *  engine's -rez list (main.c registers it in command-line order). REZ files (RezMgr v1: a 127-byte text header,
 *  then version, root directory position and size; directory blocks hold sub-directories and resources, a
 *  resource's type is its extension in four reversed bytes) and directories (custom). A later archive overrides
 *  an earlier one, as in the engine.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "platform.h"
#include "vfs.h"
#include "rezfs.h"

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

extern "C" int winapi_debug;

namespace {

struct rez_entry { uint32_t pos, size; };

struct archive {
    std::string winpath;                // as on the command line
    std::string hostpath;
    bool directory;
    bool loaded;
    std::unordered_map<std::string, rez_entry> entries;     // lower case, '/' separators
};

std::vector<archive> archives;

std::string normalize(const char *name)
{
    std::string s;
    for (const char *p = name; *p; p++)
    {
        char c = (*p == '\\') ? '/' : (char)tolower((unsigned char)*p);
        if ((c == '/') && (s.empty() || s.back() == '/')) continue;
        s += c;
    }
    return s;
}

uint32_t u32(const std::vector<uint8_t> &d, size_t o)
{
    return (o + 4 <= d.size()) ? (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24) : 0;
}

bool cstr(const std::vector<uint8_t> &d, size_t &o, std::string &out)
{
    out.clear();
    while ((o < d.size()) && (d[o] != 0)) out += (char)d[o++];
    if (o >= d.size()) return false;
    o++;
    return true;
}

void walk(archive &a, FILE *f, uint32_t pos, uint32_t size, const std::string &path, int depth)
{
    std::vector<uint8_t> d(size);
    if ((depth > 32) || (fseek(f, pos, SEEK_SET) != 0) || (fread(d.data(), 1, size, f) != size)) return;
    size_t o = 0;
    std::string name, desc;
    while (o + 4 <= d.size())
    {
        uint32_t kind = u32(d, o);
        o += 4;
        if (kind == 1)
        {
            uint32_t p = u32(d, o), s = u32(d, o + 4);
            o += 12;
            if (!cstr(d, o, name)) return;
            walk(a, f, p, s, path + normalize(name.c_str()) + "/", depth + 1);
        }
        else if (kind == 0)
        {
            uint32_t p = u32(d, o), s = u32(d, o + 4), type = u32(d, o + 16), nkeys = u32(d, o + 20);
            o += 24;
            if (!cstr(d, o, name) || !cstr(d, o, desc)) return;
            o += 4 * (size_t)nkeys;
            char ext[5] = { 0 };
            int n = 0;
            for (int i = 3; i >= 0; i--)
            {
                char c = (char)((type >> (8 * i)) & 0xff);
                if (c != 0) ext[n++] = (char)tolower((unsigned char)c);
            }
            std::string key = path + normalize(name.c_str());
            if (n > 0) key += std::string(".") + ext;
            a.entries[key] = { p, s };
        }
        else return;
    }
}

void load(archive &a)
{
    a.loaded = true;
    if (a.directory) return;
    FILE *f = fopen(a.hostpath.c_str(), "rb");
    if (f == NULL) return;
    std::vector<uint8_t> h(139);
    if (fread(h.data(), 1, h.size(), f) == h.size() && u32(h, 127) == 1) walk(a, f, u32(h, 131), u32(h, 135), "", 0);
    fclose(f);
    if (winapi_debug) eprintf("rezfs: %s: %u resources\n", a.winpath.c_str(), (unsigned)a.entries.size());
}

}

EXTERN_C_BEGIN

void rezfs_add(const char *winpath)
{
    char host[1024];
    archive a;
    if (winpath == NULL) return;
    a.winpath = winpath;
    vfs_resolve(winpath, host, sizeof(host));
    a.hostpath = host;
    FILE *f = fopen(host, "rb");
    a.directory = (f == NULL);          // custom: a directory (or nothing)
    if (f != NULL) fclose(f);
    a.loaded = false;
    archives.push_back(a);
}

void *rezfs_load(const char *name, size_t *size)
{
    std::string key = normalize(name);
    for (size_t i = archives.size(); i-- > 0;)
    {
        archive &a = archives[i];
        if (!a.loaded) load(a);
        if (a.directory)
        {
            char host[1024];
            std::string path = a.winpath + "\\" + name;
            if (!vfs_resolve(path.c_str(), host, sizeof(host))) continue;
            FILE *f = fopen(host, "rb");
            if (f == NULL) continue;
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            void *data = (n >= 0) ? malloc((size_t)n + 1) : NULL;
            if ((data != NULL) && (fread(data, 1, (size_t)n, f) == (size_t)n))
            {
                fclose(f);
                ((char *)data)[n] = 0;
                *size = (size_t)n;
                return data;
            }
            free(data);
            fclose(f);
            continue;
        }
        auto it = a.entries.find(key);
        if (it == a.entries.end()) continue;
        FILE *f = fopen(a.hostpath.c_str(), "rb");
        if (f == NULL) continue;
        void *data = malloc((size_t)it->second.size + 1);
        if ((data != NULL) && (fseek(f, it->second.pos, SEEK_SET) == 0) && (fread(data, 1, it->second.size, f) == it->second.size))
        {
            fclose(f);
            ((char *)data)[it->second.size] = 0;
            *size = it->second.size;
            return data;
        }
        free(data);
        fclose(f);
    }
    return NULL;
}

EXTERN_C_END
