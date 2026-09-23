/**
 *
 *  GDI32 emulation.
 *
 */

#if !defined(_WINAPI_GDI32_H_INCLUDED_)
#define _WINAPI_GDI32_H_INCLUDED_

#include <stdint.h>
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

void * CCALL GetDC_c(void *hWnd);
int32_t CCALL ReleaseDC_c(void *hWnd, void *hDC);
uint32_t CCALL DestroyWindow_c(void *hWnd);

void winapi_gdi32_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _WINAPI_GDI32_H_INCLUDED_ */
