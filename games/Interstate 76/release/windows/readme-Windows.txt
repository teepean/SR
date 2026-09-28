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
glide_scale       Glide internal resolution = 640x480 * glide_scale (1-8)
window_scale      initial window size = game resolution * window_scale
fullscreen        1 = start in fullscreen
fps               frame rate limit (default 20; the game's physics, AI and weapons were made for ~20 FPS,
                  higher values make the game misbehave), 0 = unlimited
vsync             1 = vertical sync
sound             0 = no sound
cd                2 = audio CD with music from the music\*.mp3 files (GOG), 1 = game CD, 0 = no CD drive
joystick          0 = don't use joysticks / gamepads
joystick_device   name substring (e.g. T150) or SDL device index; only matching devices are exposed
                  (empty = all devices, wheels/gamepads first: the game binds to joystick 1)
joystick_merge    1 = joystick 1 combines all devices (buttons/axes), so the "Press any key or button..."
                  control mapping reacts to any pad/wheel; 0 = one device per joystick

Command line parameters /glide or /gdi select the renderer (overriding the configuration).


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
