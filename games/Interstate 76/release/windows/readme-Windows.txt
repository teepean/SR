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
anisotropy        Direct3D 11: anisotropic texture filtering (1 = off, 2-16), sharpens textures
                  seen at a grazing angle (roads, walls); trilinear mipmaps are always used
window_scale      initial window size = game resolution * window_scale
fullscreen        1 = start in fullscreen
fps               frame rate limit (default 20; the game's physics, AI and weapons were made for ~20 FPS,
                  higher values make the game misbehave), 0 = unlimited
vsync             1 = vertical sync
sound             0 = no sound
cd                2 = audio CD with music from the music\*.mp3 files (GOG), 1 = game CD, 0 = no CD drive
joystick          0 = don't use joysticks / gamepads

Command line parameters /glide or /gdi select the renderer (overriding the configuration).


Multiplayer
-----------

Internet / LAN games over TCP/IP: MELEE -> MULTI MELEE -> HOST or JOIN -> INTERNET.
The host starts a game (BROADCAST GAME). The other players add the host in the CONNECT TO SERVER dialog
(NEW, a name and the host's IP address or host name - optionally host:port - then DONE) and join the game.

net_nat = 1 (default): works through NAT routers. Only the host has to forward UDP port 21155 in its
router; the joining players need nothing. All players must use this version of the game with net_nat = 1.
net_nat = 0: the original network code - for LAN or VPN games (e.g. ZeroTier, Radmin VPN) together with
players who use the original game; doesn't work through NAT routers.
net_bind_ip = <address>: use one network interface only.


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


Log file: set the environment variables I76_DEBUG=1 and I76_LOG=SR-I76.log before starting the game.

Source code is available on GitHub: https://github.com/M-HT/SR


Changes
-------

v1.0.0 (2026-09-23)
first Windows (x64) version
