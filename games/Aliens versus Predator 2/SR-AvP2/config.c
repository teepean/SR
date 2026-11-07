/**
 *
 *  Settings: SR-AvP2.cfg in the game directory, overridden by environment variables AVP2_<KEY>.
 *  A commented default file (game.h: the game's) is written if none exists.
 *
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"
#include "game.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_FILE "SR-AvP2.cfg"
#define MAX_ENTRIES 64

static struct { char key[32]; char value[128]; } entries[MAX_ENTRIES];
static int num_entries, loaded;

static void trim(char *s)
{
    char *p = s, *e;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    e = s + strlen(s);
    while ((e > s) && isspace((unsigned char)e[-1])) *--e = 0;
}

static void parse(const char *text)
{
    char line[256];
    const char *p = text;

    while (*p)
    {
        size_t n = strcspn(p, "\n");
        char *eq;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p += strcspn(p, "\n");
        if (*p) p++;

        if (strchr(line, '#') != NULL) *strchr(line, '#') = 0;
        eq = strchr(line, '=');
        if ((eq == NULL) || (num_entries >= MAX_ENTRIES)) continue;
        *eq = 0;
        trim(line);
        trim(eq + 1);
        if (line[0] == 0) continue;
        snprintf(entries[num_entries].key, sizeof(entries[0].key), "%.31s", line);
        snprintf(entries[num_entries].value, sizeof(entries[0].value), "%.127s", eq + 1);
        num_entries++;
    }
}

void config_load(void)
{
    FILE *f;
    long size;
    char *text;

    if (loaded) return;
    loaded = 1;

    f = fopen(CONFIG_FILE, "rb");
    if (f == NULL)
    {
        f = fopen(CONFIG_FILE, "wb");
        if (f != NULL)
        {
            fputs(game.default_config, f);
            fclose(f);
        }
        parse(game.default_config);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char *)calloc(1, size + 1);
    if (fread(text, 1, size, f) != (size_t)size) text[0] = 0;
    fclose(f);
    parse(text);
    free(text);
}

const char *config_get(const char *key)
{
    char env[48];
    const char *v;
    int i;

    snprintf(env, sizeof(env), "AVP2_%s", key);
    for (i = 4; env[i]; i++) env[i] = (char)toupper((unsigned char)env[i]);
    v = getenv(env);
    if (v != NULL) return v;

    config_load();
    for (i = num_entries - 1; i >= 0; i--)
    {
        if (strcasecmp(entries[i].key, key) == 0) return entries[i].value;
    }
    return NULL;
}

int config_get_int(const char *key, int def)
{
    const char *v = config_get(key);
    return (v != NULL && *v) ? atoi(v) : def;
}

#ifdef __cplusplus
}
#endif
