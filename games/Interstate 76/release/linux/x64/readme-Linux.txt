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
glide_scale       Glide internal resolution = 640x480 * glide_scale (1-8)
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
