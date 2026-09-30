/**
 *
 *  Settings: SR-I76.cfg in the game directory, overridden by environment variables I76_<KEY>.
 *  A commented default file is written if none exists.
 *
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_FILE "SR-I76.cfg"
#define MAX_ENTRIES 64

static struct { char key[32]; char value[128]; } entries[MAX_ENTRIES];
static int num_entries, loaded;

static const char default_config[] =
    "# Interstate '76 (SR-I76) settings. Environment variables I76_<KEY> override these.\n"
    "\n"
    "# renderer: glide (hardware 3D) or software\n"
    "renderer = glide\n"
    "# Windows: graphics API: d3d11 (default) or opengl (OpenGL 3.3 is always used on Linux)\n"
    "graphics_api = d3d11\n"
    "# 3D render resolution: auto = the window's height (4:3), or 1-8 = 640x480 * glide_scale\n"
    "glide_scale = auto\n"
    "# anti-aliasing (MSAA samples): 0 = off, 2, 4, 8\n"
    "antialiasing = 4\n"
    "# anisotropic texture filtering (1 = off, 2-16 = filter quality); sharpens textures seen at a\n"
    "# grazing angle (roads, walls)\n"
    "anisotropy = 8\n"
    "# post-processing of the 3D picture: gamma (brightness, 0.5-2.5, 1.0 = unchanged), sharpen (0.0-1.0),\n"
    "# fxaa (1 = FXAA edge smoothing, also usable with antialiasing = 0)\n"
    "gamma = 1.0\n"
    "sharpen = 0.0\n"
    "fxaa = 0\n"
    "# texture memory of the emulated 3Dfx card in MB (2-512); the original had 2 (more texture reloads)\n"
    "texture_memory = 64\n"
    "# texture pack: directory with replacement images <w>x<h>_<hash>.png; texture_dump = 1 writes the\n"
    "# game's textures to textures_dump/ (names = replacement file names)\n"
    "texture_pack = textures\n"
    "texture_dump = 0\n"
    "# initial window size = game resolution * window_scale\n"
    "window_scale = 2\n"
    "# start in full screen (Alt+Enter toggles)\n"
    "fullscreen = 0\n"
    "# frame rate limit: the game logic was made for ~20 FPS (physics, AI and weapons misbehave at high\n"
    "# frame rates); 0 = unlimited\n"
    "fps = 20\n"
    "# Linux video driver: x11 (default, also on Wayland desktops via XWayland), wayland or auto\n"
    "video_driver = x11\n"
    "# vertical sync (1 = on, 0 = off)\n"
    "vsync = 1\n"
    "# sound (1 = on, 0 = off)\n"
    "sound = 1\n"
    "# CD drive: 2 = audio CD (music from music/*.mp3), 1 = game CD, 0 = no CD drive\n"
    "cd = 2\n"
    "# joystick / gamepad (1 = on, 0 = off)\n"
    "joystick = 1\n"
    "# joystick backend: sdl, or evdev (Linux: read /dev/input directly)\n"
    "joystick_backend = sdl\n";

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
            fputs(default_config, f);
            fclose(f);
        }
        parse(default_config);
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

    snprintf(env, sizeof(env), "I76_%s", key);
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
