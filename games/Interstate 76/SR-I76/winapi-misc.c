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
void CCALL CoUninitialize_c(void) {}

uint32_t CCALL DirectDrawEnumerateA_c(void *lpCallback, void *lpContext)
{
    return 0;
}
