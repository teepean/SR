/**
 *
 *  DirectInput 7 (dinput.c): the system keyboard and mouse, fed from the window's SDL events.
 *
 */

#if !defined(_DINPUT_H_INCLUDED_)
#define _DINPUT_H_INCLUDED_

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void dinput_event(const SDL_Event *ev);

#ifdef __cplusplus
}
#endif

#endif /* _DINPUT_H_INCLUDED_ */
