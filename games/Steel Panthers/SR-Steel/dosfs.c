/*
 * DOS services (INT 21h) for the recompiled program.
 *
 * Drive C: is the game folder (names are matched without regard to case), drive D: is the
 * game CD read directly from the BIN/CUE image (ISO 9660 data track + Red Book audio tracks).
 * Pointers in registers are flat 32-bit addresses, as under DOS/4GW.
 */
#define _GNU_SOURCE
#include "sp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ CD image */

typedef struct { int number; int audio; uint32_t file_frame; uint32_t disc_lba; } CdTrack;

static uint8_t con_get(void);

static int cd_fd = -1;
static uint32_t cd_frames;          /* frames in the BIN file */
static CdTrack cd_tracks[100];
static int cd_ntracks;
static uint32_t cd_root_lba, cd_root_size;

static uint32_t msf_to_frames(const char *s)
{
    int m = 0, sec = 0, f = 0;
    sscanf(s, "%d:%d:%d", &m, &sec, &f);
    return (uint32_t)((m * 60 + sec) * 75 + f);
}

static int cd_read_sector(uint32_t lba, uint8_t *buf)  /* data track, MODE1/2352 */
{
    if (cd_fd < 0) return 0;
    return pread(cd_fd, buf, 2048, (off_t)lba * 2352 + 16) == 2048;
}

static int cd_open(const char *cue_path)
{
    FILE *f = fopen(cue_path, "rt");
    if (!f) return 0;
    char line[512], bin[512] = "";
    uint32_t pregap_total = 0;
    CdTrack *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncasecmp(p, "FILE", 4)) {
            char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2) { *q2 = 0; snprintf(bin, sizeof bin, "%s", q1 + 1); }
        } else if (!strncasecmp(p, "TRACK", 5) && cd_ntracks < 99) {
            cur = &cd_tracks[cd_ntracks++];
            cur->number = atoi(p + 5);
            cur->audio = strstr(p, "AUDIO") != NULL;
        } else if (!strncasecmp(p, "PREGAP", 6) && cur) {
            pregap_total += msf_to_frames(p + 6 + strspn(p + 6, " \t"));
        } else if (!strncasecmp(p, "INDEX 01", 8) && cur) {
            cur->file_frame = msf_to_frames(p + 8 + strspn(p + 8, " \t"));
            cur->disc_lba = cur->file_frame + pregap_total;
        }
    }
    fclose(f);
    if (!bin[0] || cd_ntracks == 0) return 0;

    char path[1024];
    snprintf(path, sizeof path, "%s", cue_path);
    char *slash = strrchr(path, '/');
    if (slash) slash[1] = 0; else path[0] = 0;
    strncat(path, bin, sizeof path - strlen(path) - 1);
    cd_fd = open(path, O_RDONLY);
    if (cd_fd < 0) { sp_log("cd: cannot open %s\n", path); return 0; }
    struct stat st;
    fstat(cd_fd, &st);
    cd_frames = (uint32_t)(st.st_size / 2352);

    uint8_t pvd[2048];
    if (!cd_read_sector(16, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) {
        sp_log("cd: no ISO 9660 volume in %s\n", path);
        close(cd_fd); cd_fd = -1;
        return 0;
    }
    memcpy(&cd_root_lba, pvd + 156 + 2, 4);
    memcpy(&cd_root_size, pvd + 156 + 10, 4);
    sp_log("cd: %s, %d tracks, root at %u\n", path, cd_ntracks, cd_root_lba);
    return 1;
}

int dosfs_cd_present(void) { return cd_fd >= 0; }

uint32_t dosfs_cd_tracks(uint32_t *starts, uint32_t max, uint32_t *leadout)
{
    uint32_t n = 0;
    for (int i = 0; i < cd_ntracks && n < max; i++) starts[n++] = cd_tracks[i].disc_lba;
    uint32_t pregap = cd_ntracks ? cd_tracks[cd_ntracks - 1].disc_lba - cd_tracks[cd_ntracks - 1].file_frame : 0;
    if (leadout) *leadout = cd_frames + pregap;
    return n;
}

/* read raw audio frames (2352 bytes each) addressed by disc LBA */
int dosfs_cd_read_audio(uint32_t lba, uint8_t *buf, uint32_t frames)
{
    if (cd_fd < 0) return 0;
    /* translate disc LBA to file frame using the track that contains it */
    int t = -1;
    for (int i = 0; i < cd_ntracks; i++)
        if (cd_tracks[i].disc_lba <= lba) t = i;
    if (t < 0) return 0;
    int64_t file_frame = (int64_t)lba - (cd_tracks[t].disc_lba - cd_tracks[t].file_frame);
    if (file_frame < 0) { memset(buf, 0, (size_t)frames * 2352); return (int)frames; }
    ssize_t got = pread(cd_fd, buf, (size_t)frames * 2352, (off_t)file_frame * 2352);
    if (got < 0) got = 0;
    if ((size_t)got < (size_t)frames * 2352) memset(buf + got, 0, (size_t)frames * 2352 - got);
    return (int)(got / 2352);
}

typedef struct { uint32_t lba, size; int dir; uint8_t date[7]; } IsoEntry;

/* iterate a directory extent; callback returns nonzero to stop */
static int iso_iterate(uint32_t lba, uint32_t size, int (*cb)(const char *name, const IsoEntry *e, void *ctx), void *ctx)
{
    uint8_t sec[2048];
    for (uint32_t off = 0; off < size; off += 2048) {
        if (!cd_read_sector(lba + off / 2048, sec)) return 0;
        for (uint32_t i = 0; i < 2048;) {
            uint8_t len = sec[i];
            if (len == 0) break;
            uint8_t nlen = sec[i + 32];
            char name[256];
            if (nlen == 1 && (sec[i + 33] == 0 || sec[i + 33] == 1)) {
                snprintf(name, sizeof name, "%s", sec[i + 33] == 0 ? "." : "..");
            } else {
                memcpy(name, sec + i + 33, nlen);
                name[nlen] = 0;
                char *semi = strchr(name, ';');
                if (semi) *semi = 0;
                size_t l = strlen(name);
                if (l && name[l - 1] == '.') name[l - 1] = 0;
            }
            IsoEntry e;
            memcpy(&e.lba, sec + i + 2, 4);
            memcpy(&e.size, sec + i + 10, 4);
            e.dir = (sec[i + 25] & 2) != 0;
            memcpy(e.date, sec + i + 18, 7);
            if (cb(name, &e, ctx)) return 1;
            i += len;
        }
    }
    return 0;
}

typedef struct { const char *want; IsoEntry found; int ok; } IsoFind;

static int iso_find_cb(const char *name, const IsoEntry *e, void *ctx)
{
    IsoFind *f = (IsoFind *)ctx;
    if (strcasecmp(name, f->want) == 0) { f->found = *e; f->ok = 1; return 1; }
    return 0;
}

/* path: components separated by '\' without drive and leading '\' */
static int iso_lookup(const char *path, IsoEntry *out)
{
    IsoEntry cur = { cd_root_lba, cd_root_size, 1, {0} };
    char buf[260];
    snprintf(buf, sizeof buf, "%s", path);
    char *save = NULL;
    for (char *c = strtok_r(buf, "\\", &save); c; c = strtok_r(NULL, "\\", &save)) {
        if (!cur.dir) return 0;
        IsoFind f = { c, {0}, 0 };
        iso_iterate(cur.lba, cur.size, iso_find_cb, &f);
        if (!f.ok) return 0;
        cur = f.found;
    }
    *out = cur;
    return 1;
}

/* ------------------------------------------------------------------ paths */

static char game_dir[1024];
static char c_cwd[260];             /* current directory on C:, without leading '\' */
static int cur_drive = 2;           /* 0 = A:, 2 = C:, 3 = D: */
static uint32_t dta_addr;

/* Split a DOS path into drive and an absolute path ('\'-separated, no leading '\', uppercase). */
static int dos_normalize(const char *in, int *drive, char *out, size_t outsz)
{
    char tmp[520];
    int d = cur_drive;
    const char *p = in;
    if (p[0] && p[1] == ':') { d = toupper((unsigned char)p[0]) - 'A'; p += 2; }
    if (d != 2 && d != 3) return 0;
    if (*p == '\\' || *p == '/') snprintf(tmp, sizeof tmp, "%s", p + 1);
    else if (d == 2 && c_cwd[0]) snprintf(tmp, sizeof tmp, "%s\\%s", c_cwd, p);
    else snprintf(tmp, sizeof tmp, "%s", p);

    /* collapse ., .. and separators */
    char *comps[64]; int n = 0;
    for (char *s = tmp; *s; s++) if (*s == '/') *s = '\\';
    char *save = NULL;
    for (char *c = strtok_r(tmp, "\\", &save); c; c = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(c, ".")) continue;
        if (!strcmp(c, "..")) { if (n) n--; continue; }
        if (n < 64) comps[n++] = c;
    }
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        if (i) strncat(out, "\\", outsz - strlen(out) - 1);
        strncat(out, comps[i], outsz - strlen(out) - 1);
    }
    for (char *s = out; *s; s++) *s = (char)toupper((unsigned char)*s);
    *drive = d;
    return 1;
}

/* Find a host path for a normalized C: path; missing last component is allowed when create != 0. */
static int host_path(const char *norm, char *out, size_t outsz, int create)
{
    char buf[520];
    snprintf(buf, sizeof buf, "%s", norm);
    snprintf(out, outsz, "%s", game_dir);
    char *save = NULL;
    char *c = strtok_r(buf, "\\", &save);
    while (c) {
        char *next = strtok_r(NULL, "\\", &save);
        DIR *dir = opendir(out);
        char match[256] = "";
        if (dir) {
            struct dirent *de;
            while ((de = readdir(dir))) {
                if (strcasecmp(de->d_name, c) == 0) { snprintf(match, sizeof match, "%s", de->d_name); break; }
            }
            closedir(dir);
        }
        if (!match[0]) {
            if (next || !create) return 0;
            snprintf(match, sizeof match, "%s", c);
        }
        strncat(out, "/", outsz - strlen(out) - 1);
        strncat(out, match, outsz - strlen(out) - 1);
        c = next;
    }
    return 1;
}

/* ------------------------------------------------------------------ handles */

enum { H_FREE, H_CONSOLE, H_HOST, H_CD };
typedef struct { int kind; int fd; uint32_t lba, size, pos; } Handle;
#define MAX_HANDLES 64
static Handle handles[MAX_HANDLES];

static int new_handle(void)
{
    for (int i = 5; i < MAX_HANDLES; i++) if (handles[i].kind == H_FREE) return i;
    return -1;
}

static void dos_error(Regs *r, uint16_t code)
{
    r->eax = code;
    set_cf(r, 1);
}

static void dos_ok(Regs *r) { set_cf(r, 0); }

static void unix_to_dos_time(time_t t, uint16_t *dtime, uint16_t *ddate)
{
    struct tm tm;
    localtime_r(&t, &tm);
    *dtime = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    *ddate = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

static void do_open(Regs *r, int create)
{
    const char *name = (const char *)G2H(r->edx);
    char norm[520], host[1100];
    int drive;
    if (!dos_normalize(name, &drive, norm, sizeof norm)) { dos_error(r, 3); return; }
    int h = new_handle();
    if (h < 0) { dos_error(r, 4); return; }

    if (drive == 3) {
        IsoEntry e;
        if (create || (AL(r) & 3) != 0) { dos_error(r, 5); return; }
        if (!iso_lookup(norm, &e) || e.dir) { if (sp_debug) sp_log("open D:\\%s failed\n", norm); dos_error(r, 2); return; }
        handles[h] = (Handle){ H_CD, -1, e.lba, e.size, 0 };
        r->eax = (uint32_t)h;
        dos_ok(r);
        if (sp_debug) sp_log("open D:\\%s -> %d\n", norm, h);
        return;
    }

    if (!host_path(norm, host, sizeof host, create)) {
        if (sp_debug) sp_log("open C:\\%s: not found\n", norm);
        dos_error(r, 2);
        return;
    }
    int flags;
    if (create) flags = O_RDWR | O_CREAT | O_TRUNC;
    else switch (AL(r) & 3) { case 0: flags = O_RDONLY; break; case 1: flags = O_WRONLY; break; default: flags = O_RDWR; }
    int fd = open(host, flags, 0644);
    if (fd < 0 && !create && (AL(r) & 3) == 2) fd = open(host, O_RDONLY);
    if (fd < 0) { dos_error(r, errno == ENOENT ? 2 : 5); return; }
    handles[h] = (Handle){ H_HOST, fd, 0, 0, 0 };
    r->eax = (uint32_t)h;
    dos_ok(r);
    if (sp_debug) sp_log("%s C:\\%s -> %d\n", create ? "create" : "open", norm, h);
}

static Handle *get_handle(Regs *r)
{
    uint32_t h = BX(r);
    if (h >= MAX_HANDLES || handles[h].kind == H_FREE) { dos_error(r, 6); return NULL; }
    return &handles[h];
}

static void do_read(Regs *r)
{
    Handle *h = get_handle(r);
    if (!h) return;
    uint8_t *buf = (uint8_t *)G2H(r->edx);
    uint32_t n = r->ecx;
    if (h->kind == H_CONSOLE) {
        uint32_t i = 0;
        while (i < n) {
            uint8_t c = con_get();
            buf[i++] = c;
            if (c == '\r') { if (i < n) buf[i++] = '\n'; break; }
        }
        r->eax = i; dos_ok(r); return;
    }
    if (h->kind == H_CD) {
        if (h->pos >= h->size) { r->eax = 0; dos_ok(r); return; }
        if (n > h->size - h->pos) n = h->size - h->pos;
        uint32_t done = 0;
        uint8_t sec[2048];
        while (done < n) {
            uint32_t s = (h->pos + done) / 2048, o = (h->pos + done) % 2048;
            uint32_t take = 2048 - o;
            if (take > n - done) take = n - done;
            if (!cd_read_sector(h->lba + s, sec)) break;
            memcpy(buf + done, sec + o, take);
            done += take;
        }
        h->pos += done;
        r->eax = done; dos_ok(r); return;
    }
    ssize_t got = read(h->fd, buf, n);
    if (got < 0) { dos_error(r, 5); return; }
    r->eax = (uint32_t)got;
    dos_ok(r);
}

static void do_write(Regs *r)
{
    Handle *h = get_handle(r);
    if (!h) return;
    const uint8_t *buf = (const uint8_t *)G2H(r->edx);
    uint32_t n = r->ecx;
    if (h->kind == H_CONSOLE) {
        if (n) (void)!write(BX(r) == 2 ? 2 : 1, buf, n);
        r->eax = n; dos_ok(r); return;
    }
    if (h->kind == H_CD) { dos_error(r, 5); return; }
    if (n == 0) {   /* truncate at the current position */
        off_t pos = lseek(h->fd, 0, SEEK_CUR);
        if (ftruncate(h->fd, pos) != 0) { dos_error(r, 5); return; }
        r->eax = 0; dos_ok(r); return;
    }
    ssize_t put = write(h->fd, buf, n);
    if (put < 0) { dos_error(r, 5); return; }
    r->eax = (uint32_t)put;
    dos_ok(r);
}

static void do_seek(Regs *r)
{
    Handle *h = get_handle(r);
    if (!h) return;
    int32_t off = (int32_t)(((uint32_t)CX(r) << 16) | DX(r));
    int whence = AL(r);
    uint32_t pos;
    if (h->kind == H_CONSOLE) { pos = 0; }
    else if (h->kind == H_CD) {
        int64_t np = whence == 0 ? off : whence == 1 ? (int64_t)h->pos + off : (int64_t)h->size + off;
        if (np < 0) { dos_error(r, 0x19); return; }
        h->pos = (uint32_t)np;
        pos = h->pos;
    } else {
        off_t np = lseek(h->fd, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
        if (np < 0) { dos_error(r, 0x19); return; }
        pos = (uint32_t)np;
    }
    r->eax = (r->eax & 0xffff0000u) | (pos & 0xffff);
    r->edx = (r->edx & 0xffff0000u) | (pos >> 16);
    dos_ok(r);
}

/* ------------------------------------------------------------------ find first / next */

typedef struct { char name[13]; uint8_t attr; uint32_t size; uint16_t time, date; } FindEntry;
typedef struct { int used; FindEntry *e; int n, next; } Search;
#define MAX_SEARCH 16
static Search searches[MAX_SEARCH];
static uint32_t search_seq;

static int wild_match(const char *pat, const char *name)
{
    /* DOS-style: name and extension matched separately */
    char pn[9] = "", pe[4] = "", nn[9] = "", ne[4] = "";
    const char *d = strchr(pat, '.');
    snprintf(pn, sizeof pn, "%.*s", d ? (int)(d - pat) : (int)strlen(pat), pat);
    if (d) snprintf(pe, sizeof pe, "%s", d + 1);
    d = strchr(name, '.');
    snprintf(nn, sizeof nn, "%.*s", d ? (int)(d - name) : (int)strlen(name), name);
    if (d) snprintf(ne, sizeof ne, "%s", d + 1);
    const char *pp[2] = { pn, pe }, *np[2] = { nn, ne };
    for (int part = 0; part < 2; part++) {
        const char *p = pp[part], *s = np[part];
        if (part == 1 && !d && !strchr(pat, '.')) return 1;
        while (*p) {
            if (*p == '*') { s = s + strlen(s); p++; break; }
            if (*p == '?') { if (*s) s++; p++; continue; }
            if (toupper((unsigned char)*p) != toupper((unsigned char)*s)) return 0;
            p++; s++;
        }
        if (*p == 0 && *s != 0 && p[-1] != '*') return 0;
    }
    return 1;
}

static int valid_83(const char *n)
{
    const char *d = strchr(n, '.');
    size_t base = d ? (size_t)(d - n) : strlen(n);
    if (base == 0 || base > 8) return 0;
    if (d && (strlen(d + 1) > 3 || strchr(d + 1, '.'))) return 0;
    for (const char *s = n; *s; s++) if (*s == ' ' || (unsigned char)*s > 126) return 0;
    return 1;
}

typedef struct { Search *s; const char *pat; uint16_t attr; int cap; } IsoCollect;

static void search_add(Search *s, int *cap, const char *name, uint8_t attr, uint32_t size, uint16_t t, uint16_t d)
{
    if (s->n == *cap) { *cap = *cap ? *cap * 2 : 64; s->e = (FindEntry *)realloc(s->e, *cap * sizeof(FindEntry)); }
    FindEntry *e = &s->e[s->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    for (char *c = e->name; *c; c++) *c = (char)toupper((unsigned char)*c);
    e->attr = attr; e->size = size; e->time = t; e->date = d;
}

static int iso_collect_cb(const char *name, const IsoEntry *e, void *ctx)
{
    IsoCollect *c = (IsoCollect *)ctx;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    if (e->dir && !(c->attr & 0x10)) return 0;
    if (!valid_83(name) || !wild_match(c->pat, name)) return 0;
    uint16_t t = (uint16_t)((e->date[3] << 11) | (e->date[4] << 5) | (e->date[5] / 2));
    uint16_t d = (uint16_t)(((e->date[0] - 80) << 9) | (e->date[1] << 5) | e->date[2]);
    search_add(c->s, &c->cap, name, (uint8_t)(e->dir ? 0x10 : 0x01), e->size, t, d);
    return 0;
}

static void fill_dta(Search *s, int id)
{
    uint8_t *dta = (uint8_t *)G2H(dta_addr);
    FindEntry *e = &s->e[s->next++];
    memset(dta, 0, 43);
    memcpy(dta, &id, 4);
    dta[0x15] = e->attr;
    memcpy(dta + 0x16, &e->time, 2);
    memcpy(dta + 0x18, &e->date, 2);
    memcpy(dta + 0x1a, &e->size, 4);
    memcpy(dta + 0x1e, e->name, 13);
}

static void do_findfirst(Regs *r)
{
    const char *spec = (const char *)G2H(r->edx);
    char norm[520];
    int drive;
    if (!dos_normalize(spec, &drive, norm, sizeof norm)) { dos_error(r, 3); return; }
    char *slash = strrchr(norm, '\\');
    char dirpart[520] = "", pat[260];
    if (slash) { *slash = 0; snprintf(dirpart, sizeof dirpart, "%s", norm); snprintf(pat, sizeof pat, "%s", slash + 1); }
    else snprintf(pat, sizeof pat, "%s", norm);

    int id = (int)(search_seq++ % MAX_SEARCH);
    Search *s = &searches[id];
    free(s->e);
    memset(s, 0, sizeof *s);
    s->used = 1;
    int cap = 0;
    uint16_t attr = CX(r);

    if (drive == 3) {
        IsoEntry dir;
        if (!iso_lookup(dirpart, &dir) || !dir.dir) { dos_error(r, 3); return; }
        IsoCollect c = { s, pat, attr, 0 };
        iso_iterate(dir.lba, dir.size, iso_collect_cb, &c);
    } else {
        char host[1100];
        if (!host_path(dirpart, host, sizeof host, 0)) { dos_error(r, 3); return; }
        DIR *d = opendir(host);
        if (!d) { dos_error(r, 3); return; }
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (!valid_83(de->d_name) || !wild_match(pat, de->d_name)) continue;
            char full[1400];
            snprintf(full, sizeof full, "%s/%s", host, de->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;
            int isdir = S_ISDIR(st.st_mode);
            if (isdir && !(attr & 0x10)) continue;
            uint16_t t, dt;
            unix_to_dos_time(st.st_mtime, &t, &dt);
            search_add(s, &cap, de->d_name, (uint8_t)(isdir ? 0x10 : 0x20), (uint32_t)st.st_size, t, dt);
        }
        closedir(d);
    }
    if (sp_debug) sp_log("findfirst %c:\\%s\\%s -> %d entries\n", 'A' + drive, dirpart, pat, s->n);
    if (s->n == 0) { dos_error(r, 0x12); return; }
    fill_dta(s, id);
    r->eax = 0;
    dos_ok(r);
}

static void do_findnext(Regs *r)
{
    uint8_t *dta = (uint8_t *)G2H(dta_addr);
    int id;
    memcpy(&id, dta, 4);
    if (id < 0 || id >= MAX_SEARCH || !searches[id].used) { dos_error(r, 0x12); return; }
    Search *s = &searches[id];
    if (s->next >= s->n) { dos_error(r, 0x12); return; }
    fill_dta(s, id);
    r->eax = 0;
    dos_ok(r);
}

/* ------------------------------------------------------------------ INT 21h */

void dosfs_init(const char *dir, const char *cue)
{
    snprintf(game_dir, sizeof game_dir, "%s", dir);
    size_t l = strlen(game_dir);
    if (l > 1 && game_dir[l - 1] == '/') game_dir[l - 1] = 0;
    for (int i = 0; i < 5; i++) handles[i] = (Handle){ H_CONSOLE, i, 0, 0, 0 };
    if (cue && *cue) cd_open(cue);
    static uint8_t default_dta[128];
    dta_addr = H2G(lowmem_alloc(128));
    (void)default_dta;
}

static void path_op(Regs *r, int op, const char *name)
{
    char norm[520], host[1100];
    int drive;
    if (!dos_normalize(name, &drive, norm, sizeof norm) || drive != 2) { dos_error(r, drive == 3 ? 5 : 3); return; }
    int exists = host_path(norm, host, sizeof host, op == 0x39);
    switch (op) {
    case 0x39: if (!exists || mkdir(host, 0755) != 0) { dos_error(r, 5); return; } break;
    case 0x3a: if (!exists || rmdir(host) != 0) { dos_error(r, 5); return; } break;
    case 0x3b: {
        struct stat st;
        if (!exists || stat(host, &st) != 0 || !S_ISDIR(st.st_mode)) { dos_error(r, 3); return; }
        snprintf(c_cwd, sizeof c_cwd, "%s", norm);
        break; }
    case 0x41: if (!exists || unlink(host) != 0) { dos_error(r, 2); return; } break;
    }
    if (sp_debug) sp_log("path op %02x C:\\%s\n", op, norm);
    dos_ok(r);
}

/* DOS console input (INT 21h 01h/06h/07h/08h/0Bh, reads from CON). DOS returns a key without an
   ASCII code (F-keys, Home/End/PgUp/PgDn/Ins/Del, Alt+letter) as 0 followed by its scan code, but
   Steel Panthers never reads that second byte: it would take it for a letter hotkey or type it
   (Delete gave "S"). Such keys are dropped here; the arrow keys scroll the map (video.c). */
static int con_peek(void)
{
    uint16_t k;
    while (kbd_peek(&k)) {
        if ((uint8_t)k != 0 && (uint8_t)k != 0xe0) return 1;
        kbd_get();
    }
    return 0;
}

static uint8_t con_get(void)
{
    for (;;) {
        uint8_t c = (uint8_t)kbd_get();
        if (c != 0 && c != 0xe0) return c;
    }
}

void dos_int21(Regs *r)
{
    uint8_t ah = AH(r);
    switch (ah) {
    case 0x01: case 0x07: case 0x08: AL(r) = con_get(); break;
    case 0x02: { char c = (char)DL(r); (void)!write(1, &c, 1); break; }
    case 0x06:
        if (DL(r) == 0xff) {
            if (con_peek()) { AL(r) = con_get(); set_zf(r, 0); }
            else { AL(r) = 0; set_zf(r, 1); sp_idle_poll(); }
        } else { char c = (char)DL(r); (void)!write(1, &c, 1); }
        break;
    case 0x09: {
        const char *s = (const char *)G2H(r->edx);
        const char *e = strchr(s, '$');
        (void)!write(1, s, e ? (size_t)(e - s) : strlen(s));
        break; }
    case 0x0b: AL(r) = con_peek() ? 0xff : 0; if (!AL(r)) sp_idle_poll(); break;
    case 0x0e: if (DL(r) == 2 || DL(r) == 3) cur_drive = DL(r); AL(r) = 26; break;
    case 0x19: AL(r) = (uint8_t)cur_drive; break;
    case 0x1a: dta_addr = r->edx; break;
    case 0x25: break;                            /* set vector: nothing to hook */
    case 0x2a: {
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        CX(r) = (uint16_t)(tm.tm_year + 1900); DH(r) = (uint8_t)(tm.tm_mon + 1); DL(r) = (uint8_t)tm.tm_mday; AL(r) = (uint8_t)tm.tm_wday;
        break; }
    case 0x2c: {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm tm; localtime_r(&ts.tv_sec, &tm);
        CH(r) = (uint8_t)tm.tm_hour; CL(r) = (uint8_t)tm.tm_min; DH(r) = (uint8_t)tm.tm_sec; DL(r) = (uint8_t)(ts.tv_nsec / 10000000);
        break; }
    case 0x2f: r->ebx = dta_addr; break;
    case 0x30: AL(r) = 6; AH(r) = 22; BX(r) = 0; CX(r) = 0; break;
    case 0x33: if (AL(r) == 0) DL(r) = 0; break;
    case 0x35: r->ebx = 0; break;
    case 0x36: AX(r) = 64; BX(r) = 0x7fff; CX(r) = 512; DX(r) = 0xffff; break;
    case 0x37: if (AL(r) == 0) DL(r) = '/'; break;
    case 0x39: case 0x3a: case 0x3b: case 0x41:
        path_op(r, ah, (const char *)G2H(r->edx));
        return;
    case 0x3c: do_open(r, 1); return;
    case 0x3d: do_open(r, 0); return;
    case 0x3e: {
        Handle *h = get_handle(r);
        if (!h) return;
        if (h->kind == H_HOST) close(h->fd);
        if (BX(r) >= 5) h->kind = H_FREE;
        dos_ok(r);
        return; }
    case 0x3f: do_read(r); return;
    case 0x40: do_write(r); return;
    case 0x42: do_seek(r); return;
    case 0x43:
        if (AL(r) == 0) {
            char norm[520], host[1100]; int drive;
            const char *name = (const char *)G2H(r->edx);
            if (!dos_normalize(name, &drive, norm, sizeof norm)) { dos_error(r, 3); return; }
            if (drive == 3) {
                IsoEntry e;
                if (!iso_lookup(norm, &e)) { dos_error(r, 2); return; }
                CX(r) = e.dir ? 0x11 : 0x01;
            } else {
                struct stat st;
                if (!host_path(norm, host, sizeof host, 0) || stat(host, &st) != 0) { dos_error(r, 2); return; }
                CX(r) = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
            }
        }
        dos_ok(r);
        return;
    case 0x44: {
        uint8_t al = AL(r);
        if (al == 0) {
            Handle *h = get_handle(r);
            if (!h) return;
            DX(r) = h->kind == H_CONSOLE ? (BX(r) == 0 ? 0x81 : 0x82) | 0x80 : (h->kind == H_CD ? 3 : 2);
            AX(r) = DX(r);
        } else if (al == 1) {
        } else if (al == 0x08) { AX(r) = BL(r) == 4 ? 0 : 1;
        } else if (al == 0x09) { DX(r) = BL(r) == 4 ? 0x1000 : 0;
        } else if (al == 0x0a) { DX(r) = 0;
        } else { if (sp_debug) sp_log("INT 21h 44%02xh\n", al); }
        dos_ok(r);
        return; }
    case 0x45: {
        Handle *h = get_handle(r);
        if (!h) return;
        int n = new_handle();
        if (n < 0) { dos_error(r, 4); return; }
        handles[n] = *h;
        if (h->kind == H_HOST) handles[n].fd = dup(h->fd);
        r->eax = (uint32_t)n;
        dos_ok(r);
        return; }
    case 0x47: {
        char *buf = (char *)G2H(r->esi);
        int d = DL(r) ? DL(r) - 1 : cur_drive;
        strcpy(buf, d == 2 ? c_cwd : "");
        dos_ok(r);
        return; }
    case 0x4c:
        sp_exit(AL(r));
    case 0x4e: do_findfirst(r); return;
    case 0x4f: do_findnext(r); return;
    case 0x56: {
        char n1[520], n2[520], h1[1100], h2[1100]; int d1, d2;
        if (!dos_normalize((const char *)G2H(r->edx), &d1, n1, sizeof n1) || !dos_normalize((const char *)G2H(r->edi), &d2, n2, sizeof n2) || d1 != 2 || d2 != 2) { dos_error(r, 5); return; }
        if (!host_path(n1, h1, sizeof h1, 0)) { dos_error(r, 2); return; }
        if (!host_path(n2, h2, sizeof h2, 1) || rename(h1, h2) != 0) { dos_error(r, 5); return; }
        dos_ok(r);
        return; }
    case 0x57: {
        Handle *h = get_handle(r);
        if (!h) return;
        if (AL(r) == 0) {
            struct stat st;
            time_t t = time(NULL);
            if (h->kind == H_HOST && fstat(h->fd, &st) == 0) t = st.st_mtime;
            uint16_t ti, da; unix_to_dos_time(t, &ti, &da);
            CX(r) = ti; DX(r) = da;
        }
        dos_ok(r);
        return; }
    case 0x62: BX(r) = 0x24; break;
    default:
        sp_log("INT 21h AH=%02xh (AX=%04x) not implemented\n", ah, AX(r));
        dos_error(r, 1);
        return;
    }
    dos_ok(r);
}
