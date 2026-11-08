/**
 *
 *  Small Win32 APIs: winmm (time; joystick: joystick.c, aux/mci: cdaudio.c), ole32, DirectX creation functions
 *  (DirectDraw: ddraw.c, DirectSound: dsound.c, CD audio: cdaudio.c).
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

#define DSERR_NODRIVER 0x88780078u
#define DDERR_NODIRECTDRAWHW 0x887601FFu
#define MMSYSERR_NODRIVER 6
#define JOYERR_UNPLUGGED 167
#define MCIERR_DEVICE_NOT_INSTALLED 306

uint32_t CCALL timeGetTime_c(void) { return winapi_get_ticks(); }

// The engine's clock (LithTech time_Init / time_GetTime: seconds since time_Init, from timeGetTime's whole
// milliseconds) at the performance counter's resolution (x64/time-asm.asm, instruction_replacements.sci): in
// whole milliseconds a 180 Hz frame (5.56 ms) measures 5 or 6 ms and everything moves in uneven steps (up to
// 20%); at 60 Hz the original's steps were 16 or 17 ms (6%).
static uint64_t avp2_time_base;

uint32_t CCALL avp2_time_init_c(void)
{
    avp2_time_base = SDL_GetPerformanceCounter();
    return winapi_get_ticks();
}

void CCALL avp2_time_get_c(void *out)
{
    double t = (double)(SDL_GetPerformanceCounter() - avp2_time_base) / (double)SDL_GetPerformanceFrequency();
    memcpy(out, &t, sizeof(t));
}


uint32_t CCALL mciGetErrorStringA_c(uint32_t fdwError, char *lpszErrorText, uint32_t cchErrorText)
{
    if ((lpszErrorText != NULL) && (cchErrorText > 0))
    {
        snprintf(lpszErrorText, cchErrorText, "MCI error %u", fdwError);
    }
    return 1;
}

uint32_t CCALL CoInitialize_c(void *pvReserved) { return 0; }

// no COM classes are registered (DirectMusic, DirectPlay lobby, ...): REGDB_E_CLASSNOTREG
uint32_t CCALL CoCreateInstance_c(const uint8_t *rclsid, void *pUnkOuter, uint32_t dwClsContext, const uint8_t *riid, uint32_t *ppv)
{
    if (ppv != NULL) *ppv = 0;
    fprintf(stderr, "CoCreateInstance: class {%08x-...} not available\n", (rclsid != NULL) ? *(const uint32_t *)rclsid : 0);
    return 0x80040154u;
}
void CCALL CoUninitialize_c(void) {}

uint32_t CCALL DirectDrawEnumerateA_c(void *lpCallback, void *lpContext)
{
    return 0;
}

EXTERN_C_END
