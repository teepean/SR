/**
 *
 *  Small Win32 APIs: winmm (time; joystick: joystick.c, aux/mci: cdaudio.c), ole32, DirectX creation functions
 *  (DirectDraw: ddraw.c, DirectSound: dsound.c, CD audio: cdaudio.c).
 *
 */

#include <stdint.h>
#include <stdio.h>
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
