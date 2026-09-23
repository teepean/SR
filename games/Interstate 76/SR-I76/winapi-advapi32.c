/**
 *
 *  ADVAPI32 emulation - registry, stored in a text file in the game directory.
 *
 *  File format (one entry per line):
 *    K <key path>
 *    V <key path>|<value name>|<type>|<data as hex>
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "platform.h"
#include "ptr32.h"
#include "Game-Memory.h"
#include "winapi.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#define REGISTRY_FILE "SR-I76.reg"

#define HKEY_CLASSES_ROOT   0x80000000u
#define HKEY_CURRENT_USER   0x80000001u
#define HKEY_LOCAL_MACHINE  0x80000002u
#define HKEY_USERS          0x80000003u

#define ERROR_SUCCESS 0
#define ERROR_MORE_DATA 234

typedef struct reg_value {
    struct reg_value *next;
    char *key;
    char *name;
    uint32_t type;
    uint32_t size;
    uint8_t *data;
} reg_value;

typedef struct reg_key {
    struct reg_key *next;
    char *path;
} reg_key;

typedef struct {
    uint32_t magic;
    char path[512];
} key_handle;

#define KEY_MAGIC 0x4b455921

static reg_value *values;
static reg_key *keys;
static int loaded;

static const char *root_name(uint32_t hkey)
{
    switch (hkey)
    {
        case HKEY_CLASSES_ROOT: return "HKCR";
        case HKEY_CURRENT_USER: return "HKCU";
        case HKEY_LOCAL_MACHINE: return "HKLM";
        case HKEY_USERS: return "HKU";
    }
    return NULL;
}

static reg_key *find_key(const char *path)
{
    reg_key *k;
    for (k = keys; k != NULL; k = k->next)
    {
        if (0 == strcasecmp(k->path, path)) return k;
    }
    return NULL;
}

static void add_key(const char *path)
{
    reg_key *k;
    char tmp[512], *p;

    if (find_key(path) != NULL) return;

    // parents first
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    p = strrchr(tmp, '\\');
    if (p != NULL)
    {
        *p = 0;
        add_key(tmp);
    }

    k = (reg_key *) malloc(sizeof(reg_key));
    k->path = strdup(path);
    k->next = keys;
    keys = k;
}

static reg_value *find_value(const char *key, const char *name)
{
    reg_value *v;
    for (v = values; v != NULL; v = v->next)
    {
        if ((0 == strcasecmp(v->key, key)) && (0 == strcasecmp(v->name, name))) return v;
    }
    return NULL;
}

static void set_value(const char *key, const char *name, uint32_t type, const uint8_t *data, uint32_t size)
{
    reg_value *v;

    v = find_value(key, name);
    if (v == NULL)
    {
        v = (reg_value *) calloc(1, sizeof(reg_value));
        v->key = strdup(key);
        v->name = strdup(name);
        v->next = values;
        values = v;
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = (uint8_t *) malloc(size ? size : 1);
    if (size) memcpy(v->data, data, size);
}

static void save_registry(void)
{
    FILE *f;
    reg_key *k;
    reg_value *v;
    uint32_t i;

    f = fopen(REGISTRY_FILE, "wt");
    if (f == NULL) return;
    for (k = keys; k != NULL; k = k->next) fprintf(f, "K %s\n", k->path);
    for (v = values; v != NULL; v = v->next)
    {
        fprintf(f, "V %s|%s|%u|", v->key, v->name, v->type);
        for (i = 0; i < v->size; i++) fprintf(f, "%02x", v->data[i]);
        fprintf(f, "\n");
    }
    fclose(f);
}

static void load_registry(void)
{
    FILE *f;
    char line[2048];

    loaded = 1;

    // keys that exist on every installation
    add_key("HKLM\\SOFTWARE\\Activision");
    add_key("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion");

    f = fopen(REGISTRY_FILE, "rt");
    if (f == NULL) return;

    while (fgets(line, sizeof(line), f) != NULL)
    {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;

        if (line[0] == 'K' && line[1] == ' ')
        {
            add_key(line + 2);
        }
        else if (line[0] == 'V' && line[1] == ' ')
        {
            char *key, *name, *type, *hex;
            uint8_t data[1024];
            uint32_t size;

            key = line + 2;
            name = strchr(key, '|'); if (name == NULL) continue; *name++ = 0;
            type = strchr(name, '|'); if (type == NULL) continue; *type++ = 0;
            hex = strchr(type, '|'); if (hex == NULL) continue; *hex++ = 0;

            for (size = 0; hex[0] && hex[1] && size < sizeof(data); hex += 2)
            {
                unsigned int b;
                sscanf(hex, "%2x", &b);
                data[size++] = (uint8_t) b;
            }
            set_value(key, name, (uint32_t) atoi(type), data, size);
        }
    }
    fclose(f);
}

static int key_path(uint32_t hKey, const char *subkey, char *out, size_t size)
{
    const char *root;

    if (!loaded) load_registry();

    root = root_name(hKey);
    if (root != NULL)
    {
        snprintf(out, size, "%s", root);
    }
    else if ((hKey != 0) && ((key_handle *)(uintptr_t)hKey)->magic == KEY_MAGIC)
    {
        snprintf(out, size, "%s", ((key_handle *)(uintptr_t)hKey)->path);
    }
    else
    {
        return 0;
    }

    if ((subkey != NULL) && (subkey[0] != 0))
    {
        size_t len = strlen(out);
        snprintf(out + len, size - len, "\\%s", subkey);
    }
    // remove trailing backslash
    {
        size_t len = strlen(out);
        while (len && out[len - 1] == '\\') out[--len] = 0;
    }
    return 1;
}

static uint32_t new_handle(const char *path)
{
    key_handle *h = (key_handle *) x86_malloc(sizeof(key_handle));
    h->magic = KEY_MAGIC;
    strncpy(h->path, path, sizeof(h->path) - 1);
    h->path[sizeof(h->path) - 1] = 0;
    return (uint32_t)(uintptr_t) h;
}

int32_t CCALL RegOpenKeyExA_c(uint32_t hKey, const char *lpSubKey, uint32_t ulOptions, uint32_t samDesired, uint32_t *phkResult)
{
    char path[512];

    if (!key_path(hKey, lpSubKey, path, sizeof(path))) return ERROR_INVALID_HANDLE;
    if (find_key(path) == NULL)
    {
        if (winapi_debug) eprintf("RegOpenKeyExA: %s -> not found\n", path);
        return ERROR_FILE_NOT_FOUND;
    }
    if (winapi_debug) eprintf("RegOpenKeyExA: %s\n", path);
    if (phkResult != NULL) *phkResult = new_handle(path);
    return ERROR_SUCCESS;
}

int32_t CCALL RegCreateKeyExA_c(uint32_t hKey, const char *lpSubKey, uint32_t Reserved, char *lpClass, uint32_t dwOptions, uint32_t samDesired, void *lpSecurityAttributes, uint32_t *phkResult, uint32_t *lpdwDisposition)
{
    char path[512];
    int existed;

    if (!key_path(hKey, lpSubKey, path, sizeof(path))) return ERROR_INVALID_HANDLE;
    existed = (find_key(path) != NULL);
    if (!existed)
    {
        add_key(path);
        save_registry();
    }
    if (winapi_debug) eprintf("RegCreateKeyExA: %s\n", path);
    if (lpdwDisposition != NULL) *lpdwDisposition = existed ? 2 : 1; // REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY
    if (phkResult != NULL) *phkResult = new_handle(path);
    return ERROR_SUCCESS;
}

int32_t CCALL RegCloseKey_c(uint32_t hKey)
{
    if (root_name(hKey) != NULL) return ERROR_SUCCESS;
    if ((hKey != 0) && ((key_handle *)(uintptr_t)hKey)->magic == KEY_MAGIC)
    {
        ((key_handle *)(uintptr_t)hKey)->magic = 0;
        x86_free((void *)(uintptr_t)hKey);
        return ERROR_SUCCESS;
    }
    return ERROR_INVALID_HANDLE;
}

int32_t CCALL RegQueryValueExA_c(uint32_t hKey, const char *lpValueName, uint32_t *lpReserved, uint32_t *lpType, uint8_t *lpData, uint32_t *lpcbData)
{
    char path[512];
    reg_value *v;

    if (!key_path(hKey, NULL, path, sizeof(path))) return ERROR_INVALID_HANDLE;
    v = find_value(path, (lpValueName != NULL) ? lpValueName : "");
    if (winapi_debug) eprintf("RegQueryValueExA: %s\\%s -> %s\n", path, (lpValueName != NULL) ? lpValueName : "", (v != NULL) ? "found" : "not found");
    if (v == NULL) return ERROR_FILE_NOT_FOUND;

    if (lpType != NULL) *lpType = v->type;
    if (lpData != NULL)
    {
        if ((lpcbData == NULL) || (*lpcbData < v->size))
        {
            if (lpcbData != NULL) *lpcbData = v->size;
            return ERROR_MORE_DATA;
        }
        memcpy(lpData, v->data, v->size);
    }
    if (lpcbData != NULL) *lpcbData = v->size;
    return ERROR_SUCCESS;
}

int32_t CCALL RegSetValueExA_c(uint32_t hKey, const char *lpValueName, uint32_t Reserved, uint32_t dwType, const uint8_t *lpData, uint32_t cbData)
{
    char path[512];

    if (!key_path(hKey, NULL, path, sizeof(path))) return ERROR_INVALID_HANDLE;
    set_value(path, (lpValueName != NULL) ? lpValueName : "", dwType, lpData, (lpData != NULL) ? cbData : 0);
    save_registry();
    if (winapi_debug) eprintf("RegSetValueExA: %s\\%s (%u bytes)\n", path, (lpValueName != NULL) ? lpValueName : "", cbData);
    return ERROR_SUCCESS;
}

EXTERN_C_END
