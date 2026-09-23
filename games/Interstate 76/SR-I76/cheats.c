/**
 *
 *  Port-specific cheat codes, typed like the game's own: hold Ctrl+Shift during a mission and type the code.
 *
 *  getup - finishes the current mission successfully (does what a mission script's "successAll" command
 *          does: sub_412CE0 opcode 0xF). The game's own "getdown" ends the mission as if the player was
 *          knocked out.
 *
 *  The game variables are exported from the recompiled code with global_aliases.sci.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "winapi.h"

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

EXTERN_C_BEGIN

extern int32_t i76_net_game;             // sub_452D20: multiplayer game
extern uint32_t i76_player_vehicle;      // sub_457530: player vehicle (non-zero during a mission)
extern float i76_mission_time;           // sub_49C7E0: mission time (s)
extern float i76_mission_end_time;       // successAll/failAll: mission ends at this time
extern int32_t i76_mission_end_success;  // 1 = success, 0 = failure
extern int32_t i76_objective_state;      // sub_45EB30(-1): all objectives

static void cheat_getup(void)
{
    if (i76_net_game || (i76_player_vehicle == 0))
    {
        if (winapi_debug) eprintf("cheat getup: not in a single player mission\n");
        return;
    }
    // successAll 2: the mission ends successfully in 2 seconds
    i76_mission_end_success = 1;
    i76_mission_end_time = i76_mission_time + 2.0f;
    i76_objective_state = -2;
    if (winapi_debug) eprintf("cheat getup: mission success\n");
}

// called for every key press (virtual key code) with the modifier state
void cheats_key(uint32_t vk, int ctrl, int shift)
{
    static char typed[16];
    static const struct { const char *code; void (*func)(void); } codes[] = {
        { "getup", cheat_getup },
    };
    size_t len, i;

    if (!ctrl || !shift || (vk < 'A') || (vk > 'Z')) return;
    len = strlen(typed);
    if (len == sizeof(typed) - 1)
    {
        memmove(typed, typed + 1, len);
        len--;
    }
    typed[len] = (char)(vk - 'A' + 'a');
    typed[len + 1] = 0;
    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
    {
        size_t n = strlen(codes[i].code);
        if ((len + 1 >= n) && (strcmp(typed + len + 1 - n, codes[i].code) == 0))
        {
            typed[0] = 0;
            codes[i].func();
        }
    }
}

EXTERN_C_END
