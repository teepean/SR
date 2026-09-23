/**
 *
 *  Linux joystick backend: reads the kernel's evdev devices (/dev/input/event*) directly.
 *
 *  Optional (SR-I76.cfg: joystick_backend = evdev), independent of SDL. Events are parsed with the kernel's
 *  16-byte input_event layout for 32-bit processes.
 *
 *  Gamepads (BTN_GAMEPAD, e.g. xpad): X/Y left stick, Z = triggers as one throttle axis (RT forward),
 *  R/U right stick, buttons A B X Y LB RB Back Start LS RS Guide, d-pad (hat or buttons) = POV.
 *  Other joysticks: the first 6 absolute axes (excluding hats), up to 32 buttons, hat 0 = POV.
 *
 */

#if defined(__linux__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include "joystick_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

// struct input_event as the kernel writes it for 32-bit processes
typedef struct {
    uint32_t sec, usec;
    uint16_t type, code;
    int32_t value;
} ev32;

#define NBITS(x) ((((x) - 1) / (8 * sizeof(unsigned long))) + 1)
#define TEST_BIT(bit, array) (((array)[(bit) / (8 * sizeof(unsigned long))] >> ((bit) % (8 * sizeof(unsigned long)))) & 1)

typedef struct {
    int fd;
    char name[64];
    char path[64];
    int gamepad;
    // axis codes for the 6 winmm axes (-1 = none); gamepad: Z uses the two trigger codes
    int axis_code[6];
    int trigger_left, trigger_right;
    struct input_absinfo abs[ABS_CNT];
    int32_t abs_value[ABS_CNT];
    int button_code[32];
    int num_buttons, num_axes, has_hat;
    int dpad_buttons;           // d-pad reported as buttons (BTN_DPAD_*)
    uint8_t key_down[KEY_CNT];
} evdev_device;

static evdev_device devs[JOY_MAX_DEVICES];
static int ndevs;
static uint32_t last_scan;

static void close_all(void)
{
    int i;
    for (i = 0; i < ndevs; i++) if (devs[i].fd >= 0) close(devs[i].fd);
    memset(devs, 0, sizeof(devs));
    ndevs = 0;
}

static int open_device(const char *path, evdev_device *d)
{
    unsigned long evbits[NBITS(EV_CNT)], keybits[NBITS(KEY_CNT)], absbits[NBITS(ABS_CNT)];
    static const int generic_axes[] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_THROTTLE, ABS_RUDDER, ABS_WHEEL, ABS_GAS, ABS_BRAKE };
    int fd, i, code;

    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;

    memset(evbits, 0, sizeof(evbits));
    memset(keybits, 0, sizeof(keybits));
    memset(absbits, 0, sizeof(absbits));
    if ((ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) ||
        !TEST_BIT(EV_ABS, evbits) || !TEST_BIT(EV_KEY, evbits) ||
        (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) ||
        (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0) ||
        !TEST_BIT(ABS_X, absbits))
    {
        close(fd);
        return 0;
    }
    // joysticks and gamepads only (not touchpads, tablets, accelerometers)
    if (!TEST_BIT(BTN_JOYSTICK, keybits) && !TEST_BIT(BTN_GAMEPAD, keybits) && !TEST_BIT(BTN_TRIGGER_HAPPY1, keybits) &&
        !TEST_BIT(BTN_THUMB, keybits) && !TEST_BIT(BTN_TOP, keybits))
    {
        close(fd);
        return 0;
    }

    memset(d, 0, sizeof(*d));
    d->fd = fd;
    snprintf(d->path, sizeof(d->path), "%.63s", path);
    if (ioctl(fd, EVIOCGNAME(sizeof(d->name) - 1), d->name) < 0) strcpy(d->name, "Joystick");
    for (code = 0; code < ABS_CNT; code++)
    {
        if (TEST_BIT(code, absbits))
        {
            ioctl(fd, EVIOCGABS(code), &d->abs[code]);
            d->abs_value[code] = d->abs[code].value;
        }
    }
    for (i = 0; i < 6; i++) d->axis_code[i] = -1;
    d->trigger_left = d->trigger_right = -1;
    d->has_hat = TEST_BIT(ABS_HAT0X, absbits) && TEST_BIT(ABS_HAT0Y, absbits);
    d->dpad_buttons = TEST_BIT(BTN_DPAD_UP, keybits);

    if (TEST_BIT(BTN_GAMEPAD, keybits))
    {
        static const int buttons[11] = { BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR, BTN_MODE };
        d->gamepad = 1;
        d->axis_code[0] = ABS_X;
        d->axis_code[1] = TEST_BIT(ABS_Y, absbits) ? ABS_Y : -1;
        // xpad: triggers ABS_Z / ABS_RZ, right stick ABS_RX / ABS_RY
        if (TEST_BIT(ABS_Z, absbits)) d->trigger_left = ABS_Z;
        if (TEST_BIT(ABS_RZ, absbits)) d->trigger_right = ABS_RZ;
        if (TEST_BIT(ABS_BRAKE, absbits)) d->trigger_left = ABS_BRAKE;
        if (TEST_BIT(ABS_GAS, absbits)) d->trigger_right = ABS_GAS;
        d->axis_code[3] = TEST_BIT(ABS_RX, absbits) ? ABS_RX : -1;
        d->axis_code[4] = TEST_BIT(ABS_RY, absbits) ? ABS_RY : -1;
        d->num_axes = 5;
        for (i = 0; i < 11; i++) d->button_code[i] = buttons[i];
        d->num_buttons = 11;
        if (d->dpad_buttons) d->has_hat = 1;
    }
    else
    {
        int n = 0;
        for (i = 0; (i < (int)(sizeof(generic_axes) / sizeof(generic_axes[0]))) && (n < 6); i++)
        {
            if (TEST_BIT(generic_axes[i], absbits)) d->axis_code[n++] = generic_axes[i];
        }
        d->num_axes = (n < 2) ? 2 : n;
        for (code = BTN_MISC; (code < KEY_CNT) && (d->num_buttons < 32); code++)
        {
            if (TEST_BIT(code, keybits)) d->button_code[d->num_buttons++] = code;
        }
    }
    return 1;
}

static void scan(void)
{
    DIR *dir;
    struct dirent *e;
    char paths[64][32];
    int n = 0, i, j;

    close_all();
    dir = opendir("/dev/input");
    if (dir == NULL) return;
    while (((e = readdir(dir)) != NULL) && (n < 64))
    {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        snprintf(paths[n++], sizeof(paths[0]), "/dev/input/%.20s", e->d_name);
    }
    closedir(dir);
    // stable order: event number
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (atoi(paths[j] + 16) < atoi(paths[i] + 16)) { char t[32]; memcpy(t, paths[i], 32); memcpy(paths[i], paths[j], 32); memcpy(paths[j], t, 32); }

    for (i = 0; (i < n) && (ndevs < JOY_MAX_DEVICES); i++)
    {
        if (open_device(paths[i], &devs[ndevs]))
        {
            if (joy_debug) eprintf("joystick %d: %s (evdev %s, %s, %d axes, %d buttons%s)\n", ndevs, devs[ndevs].name, paths[i],
                                   devs[ndevs].gamepad ? "gamepad" : "joystick", devs[ndevs].num_axes, devs[ndevs].num_buttons, devs[ndevs].has_hat ? ", hat" : "");
            ndevs++;
        }
    }
    if (joy_debug) eprintf("joystick: %d device(s) (evdev)\n", ndevs);
}

static void pump(void)
{
    int i;
    for (i = 0; i < ndevs; i++)
    {
        evdev_device *d = &devs[i];
        ev32 ev[64];
        ssize_t r;
        if (d->fd < 0) continue;
        while ((r = read(d->fd, ev, sizeof(ev))) > 0)
        {
            int k, count = (int)(r / sizeof(ev32));
            for (k = 0; k < count; k++)
            {
                if ((ev[k].type == EV_ABS) && (ev[k].code < ABS_CNT)) d->abs_value[ev[k].code] = ev[k].value;
                else if ((ev[k].type == EV_KEY) && (ev[k].code < KEY_CNT)) d->key_down[ev[k].code] = (ev[k].value != 0);
            }
        }
        if ((r < 0) && (errno == ENODEV))
        {
            // unplugged
            close(d->fd);
            d->fd = -1;
        }
    }
}

int evdev_joystick_count(uint32_t now)
{
    int i, lost = 0;
    for (i = 0; i < ndevs; i++) if (devs[i].fd < 0) lost = 1;
    // rescan on start, after an unplug, and every 3 s while no device is present (hotplug)
    if ((last_scan == 0) || lost || ((ndevs == 0) && (now - last_scan >= 3000)))
    {
        last_scan = now ? now : 1;
        scan();
    }
    return ndevs;
}

const char *evdev_joystick_name(int index)
{
    return ((index >= 0) && (index < ndevs)) ? devs[index].name : "Joystick";
}

void evdev_joystick_caps(int index, int *axes, int *buttons, int *hat)
{
    *axes = devs[index].num_axes;
    *buttons = devs[index].num_buttons;
    *hat = devs[index].has_hat;
}

// absolute axis value -> 0..65535
static uint32_t norm(const evdev_device *d, int code)
{
    int64_t min, max, v;
    if (code < 0) return 32768;
    min = d->abs[code].minimum;
    max = d->abs[code].maximum;
    v = d->abs_value[code];
    if (max <= min) return 32768;
    if (v < min) v = min;
    if (v > max) v = max;
    return (uint32_t)((v - min) * 65535 / (max - min));
}

int evdev_joystick_read(int index, joy_state *st)
{
    evdev_device *d;
    int i;

    pump();
    if ((index < 0) || (index >= ndevs) || (devs[index].fd < 0)) return 0;
    d = &devs[index];

    for (i = 0; i < 6; i++) st->axis[i] = norm(d, d->axis_code[i]);
    if (d->gamepad)
    {
        // one throttle axis from the triggers: right trigger = forward (low values)
        int32_t z = 32768;
        if (d->trigger_right >= 0) z -= (int32_t)(norm(d, d->trigger_right) / 2);
        if (d->trigger_left >= 0) z += (int32_t)(norm(d, d->trigger_left) / 2);
        if (z < 0) z = 0;
        if (z > 65535) z = 65535;
        st->axis[2] = (uint32_t)z;
    }

    st->buttons = 0;
    for (i = 0; i < d->num_buttons; i++) if (d->key_down[d->button_code[i]]) st->buttons |= 1u << i;

    st->pov = 0xFFFF;
    if (d->has_hat)
    {
        int x, y;
        if (d->dpad_buttons && !d->abs[ABS_HAT0X].maximum)
        {
            x = d->key_down[BTN_DPAD_RIGHT] - d->key_down[BTN_DPAD_LEFT];
            y = d->key_down[BTN_DPAD_DOWN] - d->key_down[BTN_DPAD_UP];
        }
        else
        {
            x = (d->abs_value[ABS_HAT0X] > 0) - (d->abs_value[ABS_HAT0X] < 0);
            y = (d->abs_value[ABS_HAT0Y] > 0) - (d->abs_value[ABS_HAT0Y] < 0);
        }
        if (x || y)
        {
            static const uint32_t pov[3][3] = { { 31500, 0, 4500 }, { 27000, 0xFFFF, 9000 }, { 22500, 18000, 13500 } };
            st->pov = pov[y + 1][x + 1];
        }
    }
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
