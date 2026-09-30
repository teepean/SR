Interstate '76 for Windows (x64)
Version 1.0.0

Original Interstate '76 (Gold Edition / Arsenal) is required for playing.
(version from GOG.com can be used for playing)

Installation
------------

Install game (or copy installed game from another computer).
Put files from this archive into the installed game's directory.
Run SR-I76.exe in the installed game's directory.

Configuration
-------------

Configuration is stored in the file SR-I76.cfg (a file with default values is created on first start).
Every setting can also be given as an environment variable I76_<SETTING>.

renderer          glide = hardware 3D (Glide emulated), software = the game's software renderer
graphics_api      d3d11 (default) = Direct3D 11, opengl = OpenGL 3.3
                  (if Direct3D 11 isn't available, OpenGL is used)
widescreen        widescreen 3D view: auto (default) = the desktop's aspect ratio, off = 4:3, or a ratio like
                  16:9, 16:10, 21:9. The view gets wider (the cockpit too), menus and videos stay 4:3.
glide_scale       3D render resolution: auto (default) = the window's height;
                  1-8 = 640x480 * glide_scale
antialiasing      multisample anti-aliasing: 0 = off, 2, 4 (default), 8
gamma             brightness of the 3D picture (0.5-2.5, default 1.0 = unchanged)
sharpen           sharpening of the 3D picture (0.0-1.0, default 0)
fxaa              1 = FXAA edge smoothing of the 3D picture (default 0)
texture_pack      directory with replacement textures (default "textures"); see "Texture packs" below
texture_dump      1 = write every texture the game uses to textures_dump\ as PNG
texture_memory    texture memory of the emulated 3Dfx card in MB (default 64; the original had 2)
anisotropy        anisotropic texture filtering (1 = off, 2-16, default 8), sharpens textures seen
                  at a grazing angle (roads, walls)
window_scale      initial window size = game resolution * window_scale
fullscreen        1 = start in fullscreen
fps               frame rate limit (default 20; the game's physics, AI and weapons were made for ~20 FPS,
                  higher values make the game misbehave), 0 = unlimited
vsync             1 = vertical sync
sound             0 = no sound
cd                2 = audio CD with music from the music\*.mp3 files (GOG), 1 = game CD, 0 = no CD drive
joystick          0 = don't use joysticks / gamepads

Command line parameters /glide or /gdi select the renderer (overriding the configuration).


Texture packs
-------------

The game's textures can be replaced with images of any resolution (PNG, TGA, BMP or JPG).
1. Set texture_dump = 1 and play: every texture the game uses is written to textures_dump\ as
   <width>x<height>_<hash>.png (palette variants and animation frames are separate files).
2. Edit or upscale the images and put them into the texture pack directory (textures\) with the same names.
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


Fixes of original game bugs
---------------------------

* mission 13 crash (the car arrives without wheels after mission 12) - bug of the GOG version's
  i76shell.dll, fixed
* crash in mission scripts when an object is missing (sub_467400) - fixed


Misc
----

Requires 64-bit Windows 10 or newer. Windows 7 / 8.1 also work with the Universal C Runtime update
(KB2999226); Direct3D 11 needs d3dcompiler_47.dll (included in Windows 10), otherwise OpenGL is used.

Multiplayer is not supported (yet).

Log file: set the environment variables I76_DEBUG=1 and I76_LOG=SR-I76.log before starting the game.

Source code is available on GitHub: https://github.com/M-HT/SR


Changes
-------

v1.0.0 (2026-09-23)
first Windows (x64) version
