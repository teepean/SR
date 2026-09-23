/**
 *
 *  Fixes for bugs of the original game, called from instruction_replacements.sci.
 *
 *  i76shell_part_strncmp - mission 13 crash of the GOG i76shell.dll (fixed in the 1.06 patch's shell).
 *    When the shell (re)builds the car's installed parts from the part names stored in the car record
 *    (sub_10002130, e.g. between missions 12 and 13), it takes the first parts catalogue entry with the
 *    same display name. Wheels of different vehicle classes share names ("14in Rally" is wauto_1b.wdf,
 *    wbtck_1b.wdf, ...) and the catalogue is sorted so that e.g. the truck wheels come first. The shell then
 *    drops the wheels that don't fit the car, the car goes to the mission without wheels and the game
 *    crashes (sub_467470). This compare function skips wheel entries whose .wdf differs from the wheel
 *    files recorded in the car record, if the catalogue has a matching entry with the same name.
 *
 *  The shell's variables are exported from the recompiled code with global_aliases.sci.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "platform.h"
#include "winapi.h"

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

EXTERN_C_BEGIN

extern uint8_t i76shell_cars[];             // car records (2244 bytes each)
extern uint8_t i76shell_catalogue[];        // parts catalogue (84 bytes per entry)
extern int32_t i76shell_catalogue_count;
extern int32_t i76shell_car_index;          // current car record

#define CAR_SIZE 2244
#define CAR_WHEEL_WDF1 0x83A                // wheel files of the car (13 characters each), empty or "null" if unused
#define CAR_WHEEL_WDF2 0x847
#define CAR_WHEEL_WDF3 0x854
#define PART_SIZE 84
#define PART_TYPE 30                        // 5 = wheel
#define PART_FILE 59
#define PART_TYPE_WHEEL 5

static int car_has_wheel_file(const uint8_t *car, const char *file)
{
    static const int offsets[3] = { CAR_WHEEL_WDF1, CAR_WHEEL_WDF2, CAR_WHEEL_WDF3 };
    int index;

    for (index = 0; index < 3; index++)
    {
        if (strncasecmp((const char *)(car + offsets[index]), file, 13) == 0) return 1;
    }
    return 0;
}

int32_t CCALL i76shell_part_strncmp_c(const char *name, const char *part, uint32_t n)
{
    const uint8_t *car;
    int index;
    int32_t result;

    result = strncmp(name, part, n);
    if (result != 0) return result;
    if (*(const int32_t *)(part + PART_TYPE) != PART_TYPE_WHEEL) return 0;
    if (i76shell_car_index < 0) return 0;

    car = i76shell_cars + CAR_SIZE * i76shell_car_index;
    if (car_has_wheel_file(car, part + PART_FILE)) return 0;

    // a wheel with the same name and a matching file?
    for (index = 0; index < i76shell_catalogue_count; index++)
    {
        const char *other = (const char *)(i76shell_catalogue + PART_SIZE * index);
        if ((*(const int32_t *)(other + PART_TYPE) == PART_TYPE_WHEEL) &&
            (strncmp(name, other, n) == 0) &&
            car_has_wheel_file(car, other + PART_FILE))
        {
            if (winapi_debug) eprintf("shell part lookup: %.15s: %s -> %s\n", name, part + PART_FILE, other + PART_FILE);
            return 1;
        }
    }
    return 0;
}

EXTERN_C_END
