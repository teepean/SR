/**
 *
 *  Joystick backends for joystick.c (winmm joy* API): SDL (joystick.c) and Linux evdev (joystick_evdev.c).
 *
 */

#if !defined(_JOYSTICK_BACKEND_H_INCLUDED_)
#define _JOYSTICK_BACKEND_H_INCLUDED_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JOY_MAX_DEVICES 4

typedef struct {
    uint32_t axis[6];       // X Y Z R U V, 0..65535
    uint32_t buttons;       // bit n = button n
    uint32_t pov;           // hundredths of a degree, 0xFFFF = centered
} joy_state;

extern int joy_debug;

#if defined(__linux__)
int evdev_joystick_count(uint32_t now);
const char *evdev_joystick_name(int index);
void evdev_joystick_caps(int index, int *axes, int *buttons, int *hat);
int evdev_joystick_read(int index, joy_state *st);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _JOYSTICK_BACKEND_H_INCLUDED_ */
