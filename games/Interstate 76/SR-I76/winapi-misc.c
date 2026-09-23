/**
 *
 *  Small Win32 APIs: winmm (time, joystick, aux, mci), ole32, DirectX creation functions
 *  (DirectDraw is not implemented - the game uses GDI; DirectSound: dsound.c, CD audio: cdaudio.c).
 *
 */

#include <stdint.h>
#include <stdio.h>
#include "platform.h"
#include "winapi.h"

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

#define DSERR_NODRIVER 0x88780078u
#define DDERR_NODIRECTDRAWHW 0x887601FFu
#define MMSYSERR_NODRIVER 6
#define JOYERR_UNPLUGGED 167
#define MCIERR_DEVICE_NOT_INSTALLED 306

uint32_t CCALL timeGetTime_c(void) { return winapi_get_ticks(); }

uint32_t CCALL joyGetNumDevs_c(void) { return 0; }
uint32_t CCALL joyGetDevCapsA_c(uint32_t uJoyID, void *pjc, uint32_t cbjc) { return MMSYSERR_NODRIVER; }
uint32_t CCALL joyGetPosEx_c(uint32_t uJoyID, void *pji) { return JOYERR_UNPLUGGED; }
uint32_t CCALL joyGetPos_c(uint32_t uJoyID, void *pji) { return JOYERR_UNPLUGGED; }

uint32_t CCALL mciGetErrorStringA_c(uint32_t fdwError, char *lpszErrorText, uint32_t cchErrorText)
{
    if ((lpszErrorText != NULL) && (cchErrorText > 0))
    {
        snprintf(lpszErrorText, cchErrorText, "MCI error %u", fdwError);
    }
    return 1;
}

uint32_t CCALL CoInitialize_c(void *pvReserved) { return 0; }
void CCALL CoUninitialize_c(void) {}

uint32_t CCALL DirectDrawCreate_c(void *lpGUID, void **lplpDD, void *pUnkOuter)
{
    if (lplpDD != NULL) *lplpDD = NULL;
    if (winapi_debug) eprintf("DirectDrawCreate: not implemented (GDI is used)\n");
    return DDERR_NODIRECTDRAWHW;
}

uint32_t CCALL DirectDrawEnumerateA_c(void *lpCallback, void *lpContext)
{
    return 0;
}
