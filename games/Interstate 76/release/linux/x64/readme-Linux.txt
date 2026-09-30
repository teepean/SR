Interstate '76 for Linux (x64)
Version 1.0.0

Original Interstate '76 (Gold Edition / Arsenal) is required for playing.
(version from GOG.com can be used for playing)

Installation
------------

Install game (or copy installed game from another computer).
Put files from this archive into the installed game's directory.
Run I76.sh in the installed game's directory.

The GOG version can be installed with Wine or extracted with innoextract
(innoextract setup_interstate_76_*.exe).

Configuration
-------------

Configuration is stored in the file SR-I76.cfg (a file with default values is created on first start).
Every setting can also be given as an environment variable I76_<SETTING>, e.g. I76_FPS=30 ./I76.sh

renderer          glide = hardware 3D (Glide emulated with OpenGL 3.3), software = the game's software renderer
widescreen        widescreen 3D view: auto (default) = the desktop's aspect ratio, off = 4:3, or a ratio like
                  16:9, 16:10, 21:9. The view gets wider (the cockpit too), menus and videos stay 4:3.
glide_scale       3D render resolution: auto (default) = the window's height;
                  1-8 = 640x480 * glide_scale
antialiasing      multisample anti-aliasing: 0 = off, 2, 4 (default), 8
anisotropy        anisotropic texture filtering (1 = off, 2-16, default 8), sharpens textures seen
                  at a grazing angle (roads, walls)
gamma             brightness of the 3D picture (0.5-2.5, default 1.0 = unchanged)
sharpen           sharpening of the 3D picture (0.0-1.0, default 0)
fxaa              1 = FXAA edge smoothing of the 3D picture (default 0)
texture_pack      directory with replacement textures (default "textures"); see "Texture packs" below
texture_dump      1 = write every texture the game uses to textures_dump\ as PNG
texture_memory    texture memory of the emulated 3Dfx card in MB (default 64; the original had 2)
window_scale      initial window size = game resolution * window_scale
fullscreen        1 = start in fullscreen
fps               frame rate limit (default 20; the game's physics, AI and weapons were made for ~20 FPS,
                  higher values make the game misbehave), 0 = unlimited
video_driver      x11 (default, works on Wayland desktops through XWayland), wayland or auto
vsync             1 = vertical sync
sound             0 = no sound
cd                2 = audio CD with music from the music/*.mp3 files (GOG), 1 = game CD, 0 = no CD drive
joystick          0 = don't use joysticks / gamepads
joystick_backend  sdl (default) or evdev (reads /dev/input directly)

Command line parameters /glide or /gdi select the renderer (overriding the configuration).


Texture packs
-------------

The game's textures can be replaced with images of any resolution (PNG, TGA, BMP or JPG).
1. Set texture_dump = 1 and play: every texture the game uses is written to textures_dump/ as
   <width>x<height>_<hash>.png (palette variants and animation frames are separate files).
2. Edit or upscale the images and put them into the texture pack directory (textures/) with the same names.
   Keep the exact transparent (chroma key) color where the original has it; use nearest-neighbour
   scaling for those edges.
3. Set texture_dump = 0 again. The textures are replaced when the game loads them.


Controls
--------

Keyboard, mouse and joystick / gamepad controls are the game's own and can be changed in the game's
controls menu (see Manual.pdf).

Alt+Enter          toggle fullscreen
Ctrl+Shift+getup   (type during a mission) finish the mission successfully - a cheat added by this port
                   (the game's own cheats also work, e.g. Ctrl+Shift+getdown)

If the joystick doesn't react, make sure no other program (e.g. a running Wine program) has it open.


Fixes of original game bugs
---------------------------

* mission 13 crash (the car arrives without wheels after mission 12) - bug of the GOG version's
  i76shell.dll, fixed
* crash in mission scripts when an object is missing (sub_467400) - fixed


Misc
----

The game requires following 64-bit libraries: SDL2, freetype, OpenGL 3.3 driver
On debian based distributions these libraries are in following packages: libsdl2-2.0-0 libfreetype6

Multiplayer is not supported (yet).

Debug output: I76_DEBUG=1 ./I76.sh (I76_DEBUG=2 for more)

Source code is available on GitHub: https://github.com/M-HT/SR


Changes
-------

v1.0.0 (2026-09-23)
first Linux (x64) version
