/**
 *
 *  Aliens versus Predator 2: Primal Hunt (1.0.2.2): see game.h.
 *
 */

#include <stddef.h>
#include "game.h"

#ifdef __cplusplus
extern "C" {
#endif

// PrimalHunt.exe's order (avp2src src/launcher/LauncherDlg.cpp, PRIMAL_HUNT): AvP2's archives from AvP2's install
// (InstallDir in AvP2's registry key; the campaigns' and AvP2's MULTI.REZ if they are there), Primal Hunt's
// AVP2X.REZ, multi.rez, AVP2dll.rez, AVP2l.rez, dialogue.rez, the patch archives and custom
static const char *const ph_rez[] = { "avp2:AVP2.REZ", "avp2:SOUNDS.REZ", "avp2?:ALIEN.REZ", "avp2?:MARINE.REZ",
                                      "avp2?:PREDATOR.REZ", "AVP2X.REZ", "MULTI.REZ", "AVP2DLL.REZ", "AVP2L.REZ",
                                      "DIALOGUE.REZ", "AVP2P.REZ", "AVP2P1.REZ", "custom", NULL };

const game_info game = {
    "Aliens versus Predator 2: Primal Hunt",
    ph_rez,
    "# Settings of the recompiled game. Environment variables AVP2_<KEY> (upper case) override them.\n"
    "\n"
    "# Primal Hunt needs Aliens versus Predator 2's AVP2.REZ and SOUNDS.REZ: AvP2's directory (default: this one)\n"
    "#avp2_dir = ../Aliens versus Predator 2\n"
    "# window title\n"
    "#window_title = Aliens versus Predator 2: Primal Hunt\n"
    "# window system: godot (the Godot renderer) or sdl (no 3D)\n"
    "#host = godot\n"
    "# Godot's rendering driver (vulkan, opengl3)\n"
    "#rendering_driver = vulkan\n"
    "# frame rate and frame times on screen (1 = on)\n"
    "#show_fps = 0\n"
    "# LibGodot and the embedded Godot project (default: next to the program)\n"
    "#libgodot = libgodot.so\n"
    "#godot_project = godot/project.godot\n"
    "# network: the local address to bind to (default all)\n"
    "#net_bind_ip = 0.0.0.0\n",
    1
};

#ifdef __cplusplus
}
#endif
