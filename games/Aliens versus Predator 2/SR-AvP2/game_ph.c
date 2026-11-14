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
    "# Godot's rendering method (forward_plus; gl_compatibility with opengl3)\n"
    "#rendering_method = forward_plus\n"
    "# frame rate and frame times on screen (1 = on)\n"
    "#show_fps = 0\n"
    "# the game's resolution (also in its Graphics menu, wide ones too), e.g. 1920x1080 for a wide screen: the field\n"
    "# of view is widened for it (hor_plus = 0: not), the HUD uses the whole screen\n"
    "#game_resolution = 1920x1080\n"
    "# the window's size (default: the game's resolution x window_scale), e.g. 1280x720\n"
    "#window_size = 1280x720\n"
    "# the window's size as a multiple of the game's resolution. A bigger HUD, menus and subtitles: a game\n"
    "# resolution of half the window's with window_scale = 2, e.g. game_resolution = 960x540 for a 1920x1080 window\n"
    "# (full screen on 2560x1440: 1280x720) - the 3D view is drawn at the window's own resolution either way\n"
    "#window_scale = 1\n"
    "# the 2D frame's filter when it is scaled: auto (nearest at whole multiples - sharp text -, bilinear otherwise),\n"
    "# linear or nearest\n"
    "#ui_filter = auto\n"
    "# the brightness of the whole picture (gamma; 1: as the game draws it, up to 2). The game's own setting is\n"
    "# Options > Display > Gamma (50..200, saved in autoexec.cfg as DisplayGamma); this overrides it at the start\n"
    "#gamma = 1.0\n"
    "# a window that isn't 4:3 (full screen on a wide screen): wide = the 3D view fills the window's width (Hor+),\n"
    "# the HUD and the menus in a 4:3 frame in the middle; 4:3 = all in the 4:3 frame (bars at the sides);\n"
    "# stretch = all stretched over the window\n"
    "#aspect = wide\n"
    "# vertical sync: 1 on, 0 off, 2 adaptive, 3 mailbox (Godot's modes)\n"
    "#vsync = 1\n"
    "# frame rate limit (0 = none; with vsync the display's rate)\n"
    "#max_fps = 0\n"
    "# 3D: multisample anti-aliasing (0, 2, 4, 8)\n"
    "#msaa = 0\n"
    "# 3D: MSAA also on the edges of cut-out textures (fences, grates): alpha to coverage (1 = on, 0 = off)\n"
    "#msaa_alpha = 1\n"
    "# 3D: screen-space anti-aliasing (none, smaa, fxaa), alone or with MSAA: also edges inside textures\n"
    "#screen_aa = none\n"
    "# 3D: resolution scale (0.25-2.0) and upscaler (bilinear, fsr, fsr2, nearest; macOS arm64: metalfx_spatial,\n"
    "# metalfx_temporal)\n"
    "#render_scale = 1.0\n"
    "#upscaler = bilinear\n"
    "# 3D: anisotropic texture filtering (1 = off, 2, 4, 8, 16)\n"
    "#anisotropy = 4\n"
    "# 3D: the lightmaps' filter: bilinear (as the original) or bicubic (smoother light, an enhancement)\n"
    "#lightmap_filter = bilinear\n"
    "# 3D: dithering against colour banding (0 = off, as the original; 1 = on)\n"
    "#debanding = 0\n"
    "# 3D: an HD texture pack: a directory with manifest.txt and images (tools/hdtex.py export makes one from your\n"
    "# own game's textures, to upscale or edit); textures without an image in it stay as they are\n"
    "#hd_textures = /path/to/pack\n"
    "# 3D: HD textures bigger than this (pixels per side) are halved; the memory for them kept between levels (MB)\n"
    "#hd_textures_max = 4096\n"
    "#hd_textures_cache_mb = 1024\n"
    "# LibGodot and the embedded Godot project (default: next to the program)\n"
    "#libgodot = libgodot.so\n"
    "#godot_project = godot/project.godot\n"
    "# network: the local address to bind to (default all)\n"
    "#net_bind_ip = 0.0.0.0\n",
    1,
    { 0x2a4, 0x2a8 },   // lithtech.exe 0x4095b8: 0x40c2a0, 0x40c2e0
    0x74, 0x78, 0xe0    // CSHELL.DLL 0x100345b0 (SetSliderPos), 0x1002664c (SetSliderIncrement inlined)
};

#ifdef __cplusplus
}
#endif
