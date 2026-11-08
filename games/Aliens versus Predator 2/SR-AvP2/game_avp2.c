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
    "# Godot's rendering method (forward_plus; gl_compatibility with opengl3)\n" \
    "#rendering_method = forward_plus\n" \
    "# frame rate and frame times on screen (1 = on)\n" \
    "#show_fps = 0\n" \
    "# the game's resolution (its display menu offers only 4:3 ones), e.g. 1920x1080 for a wide screen: the field\n" \
    "# of view is widened for it (hor_plus = 0: not), the HUD uses the whole screen\n" \
    "#game_resolution = 1920x1080\n" \
    "# the window's size (default: the game's resolution x window_scale), e.g. 1280x720\n" \
    "#window_size = 1280x720\n" \
    "# a window that isn't 4:3 (full screen on a wide screen): wide = the 3D view fills the window's width (Hor+),\n" \
    "# the HUD and the menus in a 4:3 frame in the middle; 4:3 = all in the 4:3 frame (bars at the sides);\n" \
    "# stretch = all stretched over the window\n" \
    "#aspect = wide\n" \
    "# vertical sync: 1 on, 0 off, 2 adaptive, 3 mailbox (Godot's modes)\n" \
    "#vsync = 1\n" \
    "# frame rate limit (0 = none; with vsync the display's rate)\n" \
    "#max_fps = 0\n" \
    "# 3D: multisample anti-aliasing (0, 2, 4, 8)\n" \
    "#msaa = 0\n" \
    "# 3D: resolution scale (0.25-2.0) and upscaler (bilinear, fsr, fsr2, nearest; macOS arm64: metalfx_spatial,\n" \
    "# metalfx_temporal)\n" \
    "#render_scale = 1.0\n" \
    "#upscaler = bilinear\n" \
    "# 3D: anisotropic texture filtering (1 = off, 2, 4, 8, 16)\n" \
    "#anisotropy = 4\n" \
    "# LibGodot and the embedded Godot project (default: next to the program)\n" \
    "#libgodot = libgodot.so\n" \
    "#godot_project = godot/project.godot\n" \
    "# network: the local address to bind to (default all)\n" \
    "#net_bind_ip = 0.0.0.0\n"

const game_info game = {
    "Aliens versus Predator 2",
    avp2_rez,
    CONFIG_COMMON,
    0,
    { 0x2a4, 0x2a8 }    // lithtech.exe 0x4089b0 fills them: 0x40bae0, 0x40bb20
};

#ifdef __cplusplus
}
#endif
