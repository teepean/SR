/**
 *
 *  Miles Sound System (mss32.dll) for the engine's sound manager (clientsoundmgr.cpp, sndinst2.cpp).
 *
 *  First stage: the system starts, but no digital driver opens, so the engine runs without sound
 *  (as with "ForceNoSound"). Memory, timer and preference calls are real.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include "platform.h"
#include "winapi.h"

EXTERN_C_BEGIN

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

static int32_t preferences[64];

uint32_t CCALL AIL_startup_c(void) { return 1; }
void CCALL AIL_shutdown_c(void) { }
void CCALL AIL_lock_c(void) { }
void CCALL AIL_unlock_c(void) { }

int32_t CCALL AIL_set_preference_c(uint32_t number, int32_t value)
{
    int32_t old;
    if (number >= 64) return 0;
    old = preferences[number];
    preferences[number] = value;
    return old;
}

int32_t CCALL AIL_get_preference_c(uint32_t number)
{
    return (number < 64) ? preferences[number] : 0;
}

uint32_t CCALL AIL_ms_count_c(void) { return SDL_GetTicks(); }

void * CCALL AIL_mem_alloc_lock_c(uint32_t size) { return game_malloc((size != 0) ? size : 1); }
void CCALL AIL_mem_free_lock_c(void *ptr) { if (ptr != NULL) game_free(ptr); }

// AIL_waveOutOpen(HDIGDRIVER *drvr, LPHWAVEOUT *lphWaveOut, int wDeviceID, LPWAVEFORMAT lpFormat): 0 = success
int32_t CCALL AIL_waveOutOpen_c(uint32_t *drvr, void *lphWaveOut, int32_t wDeviceID, void *lpFormat)
{
    static int reported;
    if (drvr != NULL) *drvr = 0;
    if (!reported)
    {
        reported = 1;
        eprintf("miles: no digital driver (sound is not implemented yet)\n");
    }
    return -1;
}

void CCALL AIL_waveOutClose_c(void *drvr) { }

// no 3D providers
uint32_t CCALL AIL_enumerate_3D_providers_c(uint32_t *next, uint32_t *dest, uint32_t *name) { return 0; }

EXTERN_C_END
