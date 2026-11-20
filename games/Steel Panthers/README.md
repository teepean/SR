# Steel Panthers (SSI 1995) – native Linux x86-64 version

The original `STEEL.EXE` v1.2 (SSI's final patch; DOS/4GW, Watcom C) statically recompiled with SR, plus a native
SDL3 runtime in `SR-Steel/`. It plays the game exactly as the original, because it runs the original
code: menus, rules, AI, scenarios, campaigns, editor, save games, films and music.

## What the runtime provides

| Original | Native replacement |
|---|---|
| DOS/4GW, DOS (INT 21h) | file access with case-insensitive names (C: = game folder), find first/next, dates, DOS/DPMI memory below 4 GB |
| CD-ROM (MSCDEX, D:) | the game CD read straight from the BIN/CUE image: ISO 9660 files (films) and Red Book music tracks |
| Miles VFX driver `VESA480.DLL` | 13 native driver functions (window/pane refresh, palette, vblank) drawing into a 640x480 frame buffer |
| VESA BIOS / linear frame buffer | the same frame buffer (used by the Smacker film player) |
| Miles AIL 3.03 sound | SDL3 audio stream: samples, double-buffered streams, timers |
| SSI CD audio library | native Red Book player |
| Mouse (INT 33h + real-mode event handler), keyboard (INT 16h), PIT, VGA ports | SDL3 input, emulated ports |

## Building

1. Recompile the executable (needs SR from https://github.com/mxvisor/SR in `~/sorsa/sr-mxvisor`, NASM ≥ 2.16, SCons):

       ../../SR-games/Steel\ Panthers/SR/build-x64.sh

   It builds `SR.exe` (x64 output, with `sr-x64-dos-instructions.patch`) if needed and expects the
   v1.2 `STEEL.EXE` (md5 702ee2143a12c8b344a95d3d3a596ca4) in that folder; the game folder needs the
   v1.2 data files too (SSI's `sp_v12_ssi.exe` patch applied to a v1.1 install). See `SR-games/Steel Panthers/NOTES.md`.

2. Build the program (needs gcc, NASM, SDL3 development files):

       SR-Steel/build.sh

## Running

    cd /path/to/game/folder && /path/to/SR-Steel/steelpanthers

or `steelpanthers --game-dir DIR [--cd FILE.CUE]`. The CD image defaults to the first `.cue` in the
game folder. `--debug` (or `SP_DEBUG=1`) logs DOS/DPMI/AIL activity.

Controls beyond the original's:

- A click in the window captures the mouse (like DOSBox's autolock), so the map scrolls when the
  pointer is pushed against a screen edge. **Ctrl+Alt** releases it, as does switching to another
  window. (DOSBox's Ctrl+F10 also works, except under KDE, which uses it for Present Windows.)
- The **arrow keys** (and the keypad arrows with NumLock off) scroll the map, at the same rate as
  the screen edges: 2 hexes every 0.1 s. Under DOS they only produced stray letter hotkeys.
- **Alt+Enter** toggles full screen.
- The keypad digits 1/3/4/6/7/9 (NumLock on) are the original's hex-direction keys.

Testing aids: `SP_AUTOMATE=script` (commands `sleep MS`, `key NAME`, `click X Y [right]`, `move X Y`,
`shot NAME`, `quit`; screenshots go to `SP_SHOT_DIR`), `SP_STATS=1` (interrupt/port histogram).
