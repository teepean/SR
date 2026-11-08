/**
 *
 *  Aliens versus Predator 2 (1.0.9.6): see game.h.
 *
 */

#include <stddef.h>
#include "game.h"

#ifdef __cplusplus
extern "C" {
#endif

// AVP2.exe's order (the 1.0.9.6 patch adds AVP2P1.REZ through its "Update Command Line")
static const char *const avp2_rez[] = { "AVP2.REZ", "SOUNDS.REZ", "ALIEN.REZ", "MARINE.REZ", "PREDATOR.REZ", "MULTI.REZ",
                                        "AVP2DLL.REZ", "AVP2L.REZ", "custom", "AVP2P.REZ", "AVP2P1.REZ", NULL };

#define CONFIG_COMMON \
    "# Settings of the recompiled game. Environment variables AVP2_<KEY> (upper case) override them.\n" \
    "\n" \
    "# window title\n" \
    "#window_title = Aliens versus Predator 2\n" \
    "# window system: godot (the Godot renderer) or sdl (no 3D)\n" \
    "#host = godot\n" \
    "# Godot's rendering driver (vulkan, opengl3)\n" \
    "#rendering_driver = vulkan\n" \
    "# frame rate and frame times on screen (1 = on)\n" \
    "#show_fps = 0\n" \
    "# LibGodot and the embedded Godot project (default: next to the program)\n" \
    "#libgodot = libgodot.so\n" \
    "#godot_project = godot/project.godot\n" \
    "# network: the local address to bind to (default all)\n" \
    "#net_bind_ip = 0.0.0.0\n"

const game_info game = {
    "Aliens versus Predator 2",
    avp2_rez,
    CONFIG_COMMON,
    0
};

#ifdef __cplusplus
}
#endif
