# Aliens versus Predator 2 + Primal Hunt — porting notes

Target: Linux x64 first (Windows x64 to follow, as for Interstate '76). Branch `AvP2` (from `I76_64bit`).

## The games

| | AvP2 1.0.9.6 (Europe) | Primal Hunt 1.0.2.2 (Europe) |
|---|---|---|
| retail | `avp2src/rez/games/avp2-eu` | `avp2src/rez/games/avp2-ph-eu` |
| engine | `lithtech.exe` (950,272 bytes, **no relocations**, base 0x400000) | `lithtech.exe` (1,007,616, no relocations) |
| server | `server.dll` (relocations) | `server.dll` |
| renderer | `d3d.ren` (DirectDraw 7 / Direct3D 7) | `d3d.ren` |
| game | `CShell.DLL`, `Object.LTO` inside `AVP2DLL.REZ` | `CSHELL.DLL`, `OBJECT.LTO` inside its `AVP2DLL.REZ` |

The two engines are not interchangeable: Primal Hunt added slots to `ILTCommon`, `ILTServer` and `ILTClient`
(avp2src `docs/PLAN.md`, "7. engine"), so AvP2's game DLLs need AvP2's engine and vice versa.

Reference material:
- `/home/teemu/sorsa/avp2src` (nolffan/avp2src): Monolith's 1.0.9.6 game source (`src/AVP2`, `src/LT2` SDK and
  lithshared), and reconstructions of both engines, renderers, servers and Primal Hunt's game DLLs; scored
  byte-for-byte against retail (game 95-97 %, engines 69-77 %).
- `/home/teemu/sorsa/AVP2-Reconstructed` (lemiur): AvP2 1.0.9.6 `lithtech.exe` (90.5 % matched) and `d3d.ren`
  (53 %); `include/renderstruct.h` is the renderer interface. **Its binary is another build** (937,984 bytes,
  SHA-1 432a8d41..., distribution not stated) than avp2src's EU 1.0.9.6 (950,272 bytes, fda88197...), so its
  addresses (`config/symbols.csv`) don't apply to ours; its source and structures do.
- Neither project links a complete engine on its own yet (the gaps are filled with retail bytes).

## Architecture (hybrid; decided 2026-11-07)

One Linux x64 executable, non-PIE, everything that touches engine data below 4 GB:

```
 +--------------------- AvP2 (host executable) ----------------------+
 | SRW x64 recompiled:  lithtech.exe  +  server.dll   (AvP2 1.0.9.6) |
 |        ^  ILTClient / ILTServer / ... (generated bridge)  |        |
 |        v                                                  v        |
 | native C++: game DLLs from Monolith's source       renderer:       |
 |   CShell  (client shell)                           RenderStruct    |
 |   Object.lto (server objects)                      on Godot        |
 | runtime: Win32 subset, Winsock (hostnet), REZ/files, Miles, DInput |
 +----------------------------------+--------------------------------+
                                    | LibGodot 4.7 (libgodot.so)
                              Godot: window, rendering, audio, input, video
```

- **Engine and server: SRW x64 recompile** of the retail binaries (no complete source exists). The code/data
  partition comes from AVP2-Reconstructed's `symbols.csv`; `lithtech.exe`'s relocations are reconstructed (as
  `i76.exe`'s were). Reconstructed source replaces recompiled functions where useful, as Interstate '76's game
  fixes do.
- **Game DLLs: native**, compiled with GCC from Monolith's 1.0.9.6 source (with avp2src's matching fixes) — fixable
  and extensible in C++ (widescreen, FOV, bug fixes behind a modern switch).
- **Low-memory model**: native code keeps every pointer it shares with the engine below 4 GB — the executable is
  linked low, the game's heap comes from a MAP_32BIT arena, the game runs on a stack allocated below 4 GB. The
  game's pointer-to-uint32 habits (user data, handles) then lose nothing, and the engine can hold native
  pointers in its 32-bit fields. Structures shared with the engine keep the engine's 32-bit layout (`ptr32`
  members in the bridge's copy of the SDK structures).
- **Bridge, generated from the SDK headers**: native `ILTClient`/`ILTServer`/`ILTCommon`/`ILTModel`/... objects whose
  methods call the recompiled engine (emulated x86 stack, `call_game`), and asm entry stubs for the engine's
  calls into the game (`GetClientShellFunctions`, `ObjectDLLSetup`, `IClientShell`/`IServerShell` vtables, class
  `EngineMessageFn`s, callbacks).
- **Godot via LibGodot**: the host creates the `GodotInstance` (`libgodot_create_godot_instance`) and steps it once
  per engine frame. `d3d.ren` is replaced by a native `RenderStruct` (`RenderDLLSetup`) on Godot's
  `RenderingServer`: world BSP + lightmaps -> meshes, models, sprites, particles, 2D surfaces/blits -> canvas.
  DirectInput, Miles (`mss32`, 89 functions), DirectMusic and Bink/Smacker map to Godot's input, audio and
  video.
- **Primal Hunt**: its engine and server recompiled the same way; its game DLLs (Third Law's code, never
  released; avp2src's reconstruction still has unwritten functions) are recompiled with SRW until that source is
  complete.

## Milestones

1. SRW x64 recompile of AvP2's `lithtech.exe` and `server.dll`; links with stubs.
2. Host + LibGodot: a Godot window; the engine starts and reaches its console with a null renderer.
3. Native game DLLs + bridge: the main menu (2D through the renderer's surfaces).
4. Godot renderer: worlds, models, sprites, effects; a mission is playable.
5. Sound, music, input, movies.
6. Multiplayer; Primal Hunt.

## Log

### 2026-11-07 — survey
- `tools/rez.py` (avp2work): RezMgr v1 reader (layout from `lithshared/rezmgr/rezmgr.cpp` `ReadDirBlock`): directory
  entries (1: pos, size, time, name), resources (0: pos, size, time, id, type, key count, name, description,
  keys); the type is the extension's 4 characters reversed. `AVP2DLL.REZ` holds only the two game DLLs.
- Game DLL exports: `CShell.DLL` GetClientShellFunctions, GetClientShellVersion, SetInstanceHandle;
  `Object.LTO` GetServerShellFunctions, GetServerShellVersion, ObjectDLLSetup, SetInstanceHandle. Both base
  0x10000000 with relocations.
- `d3d.ren` exports RenderDLLSetup, GetSupportedModes, FreeModeList (REA ev_7286ed94...).
- Monolith's game source: 885 files, 370k lines; 56 explicit pointer-to-integer casts (harmless under the
  low-memory model).

### 2026-11-07 — milestone 1: SRW translates AvP2's lithtech.exe
- Function table: avp2src `tools/enumfuncs.py` (plain Python) on the EU binary: 96.2 % of .text in functions.
- Relocations: IDA 9.3 headless + Interstate '76's `tools/ida_export.py` and `gen_relocs.py` (19,543;
  `SRW-lithtech/relocations.csv`). First try from AVP2-Reconstructed's symbol map produced nonsense in places,
  which is how the build mismatch showed. `bssborder.csv`: .data's uninitialized part from 0x4e2000.
- SRW fixes (SRW/SR_full_x64_instr.c): x87 `fldl2e`, `fldl2t`, `fldpi`, `fprem`, `fxam`, `fldenv`, `fnstenv` were
  unknown to the x64 translator; image-base relocations (`,i`: `lea eax, [edi+0x400000]`, RVA arithmetic)
  crashed it (it indexed `section[]` with the image base).
- `instruction_replacements.sci`: 0x4b91bd `cmp esi, fs:[0]` (CRT SEH unwind; SRW translates only mov/push/pop
  with fs:) -> `x86_read_fs_dword` + `cmp esi, r9d`.
- Result: SRW rc 0; nasm (after short-jump repair) leaves only imports, runtime helpers (x86_int, x86_mov_*)
  and four labels inside the CRT's memcpy/memmove (jump tables interleaved with code).
- Next: the statically linked CRT (libcmt, 0x4b42e0-0x4c3675, 481 functions incl. startup) is redirected to
  runtime implementations (external_procedures.sci) and its bodies excluded (ignored_areas.sci), as for the
  Interstate '76 DLLs; then server.dll.
- LibGodot: Godot 4.7.2-stable (`/home/teemu/sorsa/godot`), `scons platform=linuxbsd target=template_debug
  library_type=shared_library` -> `bin/libgodot.linuxbsd.template_debug.x86_64.so` (2.5 minutes, 32 cores).

### 2026-11-07 — milestone 2 (first part): the recompiled engine boots to the client shell
- Code roots: SRW doesn't take the PE entry point (or anything only reachable from it) as code, so WinMain and
  everything below it was never translated at first. `fixup_interpret_as_code.sci` = every IDA function start
  + the entry; `global_aliases.sci` loc_4B9F1B = lithtech_start (main.c calls the CRT startup, which runs the
  C++ constructors and WinMain as on Windows).
- The statically linked CRT (libcmt, 0x4b73d0-0x4c6775, identified with avp2src's tools/libscan.py against
  toolchains/msvc-6.00-8966/LIB/libcmt.lib) is translated as is; only memcpy/memmove (jump tables between the
  code) go to the runtime (external_procedures.sci + ignored_areas.sci). 0x4c6290-0x4c64a0 is lithshared code.
- Relocations: `relocations_include.csv` adds what gen_relocs.py's heuristics rejected but are real: a C++
  initializer at 0x410000 (round values look like constants), three vtable slots, a console command entry.
- SRW fix: `mov fs:[0], esp` (every SEH frame) was translated with the native esp instead of r11d
  (X86REGSTR_ESP); I76 had it too, but nothing there dereferenced the SEH chain (the CRT's setjmp does).
- Runtime (games/Aliens versus Predator 2/SR-AvP2, from I76's): lithtech.exe's image is mapped at 0x400000
  as data (the CRT reads its headers, the resource functions its .rsrc); resource-only DLLs (ltmsg.dll) are
  mapped by LoadLibrary (peimage.c); real threads (pthreads, each with its own emulated stack - asm-cpu.c's
  stack is thread-local), events, waits, critical sections (mutex pointer in the struct's first dword), per
  thread TLS and SEH frame, heap lock; the CRT startup's kernel32 functions; the launcher's command line
  (-rez archives) is built when none is given.
- Imports spec: tools/mkspec.py (IDA prototypes; reference: Interstate '76's spec). A pointer to a 64-bit type
  is one stack slot (QueryPerformanceCounter was wrong and unbalanced the emulated stack).
- avp2src's reconstructed engine source (src/engine/lithtech/clientwin.cpp ...) explains the code at each
  address, but its AvP2 reference is again another build (addresses shifted).
- Now: the engine opens all REZ archives, extracts cshell.dll from AVP2DLL.REZ into temp\ and calls its
  GetClientShellFunctions - the game DLLs are next.
- Code roots, second round: SRW translates only what it reaches from its roots, and a function referenced only
  from data (vtables, the engine's interface tables - filled by code) was emitted as data bytes and then run as
  x64 code. Roots now = IDA's functions + avp2src tools/enumfuncs.py's starts (1,169 more for the engine) +
  every code address stored in data (relocations; for DLLs their base relocations).
- CShell.DLL (stand-in until the native build): SRW-cshell (has relocations; its own libcmt: memcpy/memmove
  redirected), exports prefixed cshell_ (tools/prefix_exports.py: the game DLLs export the same names),
  registered in kernel32's module table; LoadLibrary runs its _DllMainCRTStartup (DLL_PROCESS_ATTACH).
  SRW: WSOCK32 ordinal 151 (__WSAFDIsSet).
- winsock.c back from Interstate '76's history (UDP), TCP calls fail for now.
- dinput.c: DirectInput 7 keyboard and mouse from SDL events. The engine builds its own data format from
  EnumObjects (one dword per object, clientinput.cpp), so objects are reported as on Windows: keys = buttons
  with the DIK code as instance/offset (bindings "##<DIK>"), the mouse as c_dfDIMouse (X/Y/Z 0/4/8, buttons
  12-15, "##x-axis" etc. match by GUID/offset, diblit.cpp).
- Now: the client shell initializes, reads autoexec.cfg and sets the render mode: LoadLibrary("d3d.ren") +
  GetProcAddress("RenderDLLSetup") - the renderer (Godot) is next; then Bink for the movies.

### 2026-11-07 — the main menu
- render.c replaces d3d.ren (registered as module "d3d.ren", RenderDLLSetup fills the RenderStruct's
  function slots 0x70-0x104): 2D for now - 32-bit surfaces in game memory, CPU BlitToScreen (stretch, colour
  key, the optimized-2D blend modes and colour), Clear, LockScreen, SwapBuffers -> display.c (SDL window,
  XRGB framebuffer) with a frame limiter (AVP2_FPS, default 60). The 3D calls draw nothing yet (Godot).
- Cursors: CLTCursor::LoadCursorBitmapResource asks the engine hook "cres_hinstance" for cres.dll and calls
  LoadImageA(IMAGE_CURSOR) - RT_GROUP_CURSOR/RT_CURSOR resources become SDL colour cursors. A recompiled
  module's handle (module_info) now also maps its DLL file for its resources.
- Strings: the engine's FormatString is LoadString (cres.dll) + FormatMessageA(FROM_STRING) with a 32-bit
  va_list; FormatMessageA now does inserts (%1!d!), %n, message tables. With the stub every key binding name
  was "Error 0", so Profiles\Player_0.txt was written wrong (delete it when that happens).
- miles.c: first stage - AIL_startup works, AIL_waveOutOpen fails, so the engine runs without sound.
- Also: CreateFontA, character spacing, INI files, directories, OpenFile, the desktop window, keyboard state,
  CoCreateInstance (no classes: DirectMusic's class is asked for).
- Now: the main menu is drawn (frames dumped with AVP2_DUMP_FRAMES under SDL_VIDEODRIVER=offscreen).
  Starting a game needs server.dll and object.lto (both still original binaries) and the 3D renderer.

### 2026-11-07 — a level runs (no 3D yet)
- object.lto (server shell + game objects) and server.dll recompiled like CShell.DLL (SRW-object, SRW-server;
  IDA + enumfuncs roots, CRT memcpy/memmove to the runtime, exports prefixed object_/server_). lithtech.exe
  runs the server itself for single player; server.dll is the dedicated server's (AVP2Serv.exe).
- SRW: x87 `ftst` (pass-through).
- Roots, third round (tools/mkroots.py): code addresses taken by code (`mov [x], offset thunk`, e.g. the vcall
  thunks of member function pointers - CHudMgr's draw functions) were emitted as data bytes. Relocations inside
  .text whose target is an IDA instruction head are roots now (jump tables in .text are IDA data heads, their
  case labels instruction heads); for lithtech.exe from relocations.csv.
- `+runworld Worlds\SinglePlayer\m1s1` on the command line starts a level (GameClientShell.cpp).
- user32: a focus change cancels the opposite one still queued (SDL reports a short focus loss while the window
  is set up; the engine restarts the renderer on each WM_ACTIVATEAPP(0)).
- Now: m1s1 runs with the HUD (motion tracker blips move, training messages); the world isn't drawn.

### 2026-11-07 — milestone 2b: Godot owns the window
- host.h: the window system under user32/dinput/display, two backends. host_godot.cpp: LibGodot (window, the
  2D framebuffer as a texture on a canvas item - a shader swaps B and R -, input events turned into SDL_Event
  structures, cursors, mouse capture); host.c: SDL (AVP2_HOST=sdl, e.g. with SDL_VIDEODRIVER=offscreen).
  One Godot frame (GodotInstance::iteration) per SwapBuffers, vsync paces it; while the game only processes
  messages (loading) a frame every 100 ms.
- libgodot is dlopen'ed (RTLD_LOCAL | RTLD_DEEPBIND) and must be built with godot/libgodot.map (only the
  libgodot_* entry points exported): Godot's statically linked libstdc++ exports STB_GNU_UNIQUE symbols
  (num_get<char>::id) that the dynamic linker unifies with the system libstdc++'s even across DEEPBIND -
  glslang crashed reading a float. godot/build_libgodot.sh builds it and godot-cpp.
- godot-cpp: no godot types as statics (their constructors call into the engine before it exists); RTTI
  for host_godot.cpp (godot-cpp's postinitialize uses dynamic_cast); Godot's --path changes the current
  directory (restored).
- The engine draws from other threads too (loading screen); Godot is only called on the main thread - other
  threads' frames and window changes wait for it.
- AVP2_DUMP_GODOT=<dir> saves Godot's picture (GPU readback) every 2 s.

### 2026-11-07 — milestone 4 (first part): the world in Godot
- render3d.cpp (render.c calls it for CreateContext/DeleteContext/RenderScene/textures): the bound world
  (RenderContextInit::m_pWorld, MainWorld) becomes Godot meshes when first drawn (on the main thread); each
  RenderScene sets a Camera3D from the SceneDesc (LithTech is left-handed: z negated, quaternion (-x, -y, z, w)).
  Layouts from AVP2-Reconstructed's headers (renderstruct.h, scenedesc.h, de_mainworld.h, de_world.h,
  de_objects.h, dtxmgr.h) and the LithTech 2 SDK (avp2src src/LT2/sdk/inc: SURF_ flags) - they fit this build.
- Geometry: every BSP of MainWorld::m_WorldModels but the VisBSP; world models' points are in world space
  (the canyon walls of m1s1 are one, "first_terrain_walls"). A poly whose vertex list was reallocated has
  m_nExtraVertices vertices there (not m_nVertices + m_nExtraVertices). Vertex UVs are in texels of the DTX's
  base size.
- Light comes from the light animations ("LightAnim_BASE" and the rest; WorldPoly::m_pLMAnimRefs = (animation,
  its poly) pairs): lightmapped polies get the frames' run-length lightmaps (d3d_DecompressLM32, at most 32x32,
  the first layer as is, the others added at their intensity), packed into 2048x2048 atlas pages; the lightmap
  axes are normal.Cross(base) and right.Cross(normal) with LithTech's operand order (b x a); Gouraud polies get
  the frames' per-vertex R/G/B (summed). SPolyVertex colours are B, G, R.
- Shading like D3D7: texture * light in sRGB values, lightmaps MODULATE2X, converted to linear for Godot.
- The 2D frame is an overlay (premultiplied ARGB): RenderScene clears its rectangle to transparent.
- Reference pictures: tools in avp2work (wine_ref.sh: the original lithtech.exe under GE-Proton in its own prefix,
  window captured with ImageMagick's import; +windowed 1; the prefix needs Proton's vkd3d DLLs for wined3d).
- Now: m1s1's canyon renders like the original (night, moonlight, lamps); objects (models, sprites, particles,
  the weapon), dynamic lights, sky and fog are next.

### 2026-11-07 — first user test: two fixes
- The game hung when its window was closed in a level: CGameServerShell's destructor (object.lto) stops its
  worker threads and waits for their handles (WaitForSingleObject, INFINITE). The DLLs' C runtime ends every
  thread through _endthreadex -> ExitThread, which ended the pthread without marking the handle finished.
  ExitThread now signals the thread's handle (and keeps the exit code) like a returning thread.
- Leaving Options > Graphics crashed (CFolderDisplay::SetRenderer indexing an empty resolution list): the page
  lists only modes whose internal name is the CardDesc console variable ("display" - DirectDraw's name for the
  primary device in the original), and ours were "SR-AvP2". GetSupportedModes now names the device "display" /
  "Primary Display Driver" (more 4:3 sizes too), and Init writes them back into the mode like the original (the
  engine's current mode, which the page compares with the list to select the current one; without that it
  selected 640x480x16 and switched to it on leaving). Changing resolution in a level works with Godot: the
  renderer restarts and render3d rebuilds the world for the new context.
- AVP2_INPUT_SCRIPT has `close` (WM_CLOSE, like the window's close button) and `skey|sdown|sup <SDL key name>`
  (synthetic key events through the same path as real keys, so DirectInput sees them). In m1s1 Escape opens
  the pause menu only after the level start (weapon select, fade-in) - ~30 s after launch here.

### 2026-11-07 — milestone 4 (second part): world model objects, models
- render3d walks the client's objects every frame: the world the renderer is bound to is CClientMgr::m_World, and
  the ObjectMgr follows it (+0x1f0; its m_ObjectLists, an LTList per OT_ type, at +0x220). Layouts from
  AVP2-Reconstructed's de_objects.h / model.h fit this build (checked with AVP2_DEBUG=2 dumps).
- World models: only the main world (PhysicsBSP, WIF_MAINWORLD) and the terrain are static; every other BSP has its
  own mesh, shown where its object (OT_WORLDMODEL / OT_CONTAINER, FLAG_VISIBLE) is: WorldModelInstance::m_Transform
  (+0x1f8) takes the BSP from where it was placed to the object. The canyon walls of m1s1 are such an object.
- Models: ModelInstance::GetTransforms (vtable slot 0x48, thiscall: c2asm's c_call_asm_this / call_game_this put
  `this` in ecx) gives the node transforms in model space; a vertex is the sum of its weights' node-space offsets
  through their nodes over the weight sum (d3d.ren's skinning). Light like d3d.ren's ModelDraw: ambient from the
  world's light grid (MainWorld+0xfc, trilinear), lerped towards the sun's colour by N.L (rest pose normals) when
  rays toward the sun reach a sky poly (the engine's i_IntersectSegment, 0x438C9D, exported from the recompiled
  engine as lithtech_i_IntersectSegment through global_aliases.sci), dynamic lights with (r^2 - d^2) falloff.
  Meshes are rebuilt only when the pose or the light changes.
- Attachments (a marine's rifle and lamp) only move when the renderer calls RenderStruct::ProcessAttachment for
  the objects it draws - done before drawing, like d3d_ProcessAttachments.
- FLAG_REALLYCLOSE (the player's weapon): its position is in the camera's space; drawn as a child of the camera
  with its depth squeezed to [0.9, 1] (reverse z) so the world never covers it.
- AVP2_INPUT_SCRIPT `rel <dx> <dy>` (relative mouse for DirectInput), AVP2_R3D_CAMERA (a fixed debugging camera).
- Sprites (OT_SPRITE): the SpriteTracker's current frame (+0x1b8) is the texture; a quad of 2 x the texture's base
  size x the scale (world units), facing the camera in the vertex shader (FLAG_GLOWSPRITE: x0.1 at 10 units ..
  x2 at 500; FLAG_SPRITEBIAS: depth 20 units nearer), or turned by the object (FLAG_ROTATEABLESPRITE); colour
  times the light grid unless FLAG_NOLIGHT; FLAG2_ADDITIVE / FLAG2_MULTIPLY blending.
- Particle systems: a ring of PSParticle through m_ParticleHead (+0x1b0), positions in the system's space; each
  a square of 4 x m_Size (d3d_DrawParticleBatch's screen extent), colour = system colour x particle colour.
- Stray reads: object.lto's sub_1018A370 can index a per-character table with -1 (a character without an id).
  Windows' contiguous heap makes that a harmless garbage read; here the 32-bit heap is separate mappings and it
  crashed once while m1s1 loaded. sysmem.c's SIGSEGV handler (Linux, 64-bit) maps a page of zeros under a read
  of unmapped memory below 2 GB (logged "memory: read of unmapped game memory"); writes still crash.
- Sky: SceneDesc::m_SkyObjects (+0xb4, count +0xb8; m1s1: 11 world models - sky box, cloud layers, a tint) are
  drawn from the sky camera: SkyDef's ViewMin + (ViewMax - ViewMin) x the viewer's place in the world's extents
  (MainWorld+0x144/0x150), turned like the camera (d3d_SetupSkyStuff). In Godot: copies of their meshes under a
  node moved by (camera - sky camera), materials at the far plane (POSITION.z = 1e-7 w, reverse z) without depth
  writes, ordered by render priority (d3d_DrawSkyObjects draws them without the z buffer, in order); surfaces
  that aren't translucent are opaque even with an alpha texture, like the original. SURF_SKY polies of the world
  aren't drawn, so the sky shows through them.
- Fog: the renderer's console variables (FogEnable, FogNearZ/FarZ, FogR/G/B, SkyFogNearZ/FarZ) come from
  RenderStruct::GetParameter + GetParameterValueString (the float getter returns in st0); Godot global shader
  parameters. Linear by distance (d3d_CalcDistanceFogAlpha), the sky's by depth (d3d_CalcSkyFogAlpha: m1s1's
  0..150 turns the clouds into the dark blue (8, 16, 24) of the original); additive surfaces fade to black.
- Dynamic lights on the world (OT_LIGHT without FLAG_ONLYLIGHTOBJECTS; m1s1: the marines' shoulder lamps, radius
  100): per pixel in the world shader, the nearest 32 in a 32 x 2 float texture (global shader parameter).
  d3d.ren's formulas: lightmapped polies get an added light texture (LMDynamic: (max(c, 127) - 127) x (1 -
  (in-plane distance / 1.25 r)^2 - (plane distance / r)^2), not times the surface's texture: BuildDynamicLightmap-
  Pixels, LMDynamicScale 2.5), vertex lit ones (2c - 255) x (1 - d / r) in their vertex colours.
- Light animations that change (m1s1: two "Flicker1WaveLightFX" layers over ~6700 polies each, single frame, the
  game moves their blend): every animation but LightAnim_BASE with one frame is a layer of its own - up to 4 per
  lightmapped poly, placed next to the poly's base lightmap in the page, 2 per vertex lit poly (vertex
  attributes). The shader adds them with their current (blend, on) from a float texture: lightmaps like
  UpdatePolyAnimatedLightmap (the first layer that is on as is, unless it follows a base layer; the others times
  their blend, saturating), vertex colours like d3d_AddLightAnimVertexColors (the frames that are on, unscaled).
- 640x480 runs at ~185 frames/s here (RTX 3090).

### 2026-11-07 — sound (Miles on SDL)
- miles.c implements the 89 mss32.dll imports on the SDL mixer of the Interstate '76 port (mixer.c, 44.1 kHz stereo;
  AVP2_AUDIO_DUMP=<file> writes the output as raw S16). The engine (AVP2-Reconstructed src/sound: soundmgr.cpp,
  soundinstance.cpp, soundbuffer.cpp) brackets every call with AIL_lock/AIL_unlock (the mixer lock). AvP2 names no 3D
  provider, so every sound is a 2D "software" sample whose volume and pan the engine computes; a provider ("Miles 2D
  Stereo Positional Audio", the name the engine maps to its Miles 3D id) with simple 3D samples is listed anyway.
- Samples: 8/16-bit PCM from AIL_set_sample_file (a WAV image) or AIL_set_sample_address + type, IMA ADPCM decoded
  when set; playback rate, volume and pan 0-127, loop blocks and counts, master volume. Handles are game memory.
- Streams: AIL_open_stream gets "\\\\<handle>" - the engine's file handle at a WAV file inside a REZ - read whole.
- AIL_decompress_ADPCM / AIL_decompress_ASI (MP3, minimp3) return PCM WAV images in game memory.
- m1s1: ambience loops and the motion tracker's beeps (pitch and volume by distance) play. Music ("musictype ima" =
  DirectMusic, the Interactive Music Architecture) is still silent: CoCreateInstance has no DirectMusic classes.
- Tests run with SDL_AUDIODRIVER=disk (SDL_DISKAUDIOFILE=/dev/null) so nothing is heard.

### 2026-11-07 — vision modes, other campaigns
- p1s1 (Predator) and a1s1 (Alien) load and play their opening cutscenes (dialogue: MP3 streams); p1s1 plays after
  the cutscene (V cycles the Predator's vision modes).
- Vision modes (ClientShellDLL ViewModeMGR.cpp, Attributes/VisionModeButes.txt) set the renderer's console
  variables InvertHack, DrawSky and LMFullBright, change the fog, and give textures D3D state changes per mode
  (SharedTexture::m_pStateChange: colour ops, blend modes - not done yet). d3d.ren ends each scene with
  d3d_DrawLightScalePoly (the picture x the light scale, or the inverted picture with InvertHack) and
  d3d_DrawLightAddPoly: render3d now draws the 3D picture into a SubViewport shown under the 2D frame (a canvas
  layer below the host's) through a post shader doing both.
- Crash fixed: display.c allocated the frame with calloc - for 640x480 above 4 GB - and RenderStruct::LockScreen
  handed it to the engine truncated to 32 bits (a full-screen fill when the vision mode changed wrote to it). The
  frame is in low memory now (sys_map_low).
- Texture states: the engine calls IClientShell::OnTextureLoad(&SharedTexture::m_pStateChange, m_pCommandLine)
  right after RenderStruct::BindTexture; the game (TextureModeMGR) registers the texture's "ViewModes ..." token and
  later points m_pStateChange (+0x34) at the D3D StateChange of the current vision mode. The command line (+0x38)
  is only set once the texture's data is loaded - d3d.ren loads it in BindTexture, so render.c's BindTexture now
  calls RenderStruct::GetTexture too (without it nothing was registered). StateChange = two STLport vectors
  (RenderState {type, value}, TextureState {stage, type, value}). p1s1 uses FOGENABLE 0 + COLOROP SELECTARG1
  ("NoFog": unlit, unfogged), FOGENABLE 1 + FOGCOLOR with FOGSTART = FOGEND = 0 (the surface becomes that colour:
  heat vision's blue world), and two blend overrides (not done). Materials get the state as uniforms, updated
  when a texture's m_pStateChange changes.
- Canvases (OT_CANVAS: the game's PolyLineFX etc. draw with their own function): d3d.ren hands the draw function
  (Canvas+0x1b0, cdecl (ILTCustomDraw *, HLOCALOBJ, void *user)) an ILTCustomDraw, a C++ interface - its five
  virtual methods are thiscall. x64/canvas-asm.asm has thiscall stubs (ecx = this to the C function's first
  argument, the callee pops) behind a vtable in 32-bit memory; render3d collects the triangle fans by state
  (blend, z read, texture by name through RenderStruct::GetSharedTexture) into the canvas's mesh each frame.
- Poly grids and line systems are drawn too (no level seen with poly grids yet: AvP2's water is translucent world
  geometry). Lines: d3d_DrawLineSystem's colours, alpha x the object's alpha, no fog.
- Memory over a 4-minute m1s1 run stays at ~860 MB RSS.

### 2026-11-08 — Primal Hunt
- Primal Hunt (1.0.2.2) has its own engine (lithtech.exe, server.dll) and game DLLs (CSHELL.DLL, OBJECT.LTO in its
  AVP2DLL.REZ): recompiled the same way into x64/ph-<module>/ (`GAME=ph gen_all.sh`; SRW-ph-<module>/, IDA exports
  in avp2work/ida-ph-*, avp2src enumfuncs roots). The modules keep their names and exported symbols, so the whole
  runtime is shared: SConstruct links a second program, SR-AvP2PH, with game_ph.c instead of game_avp2.c
  (game.h: title, the launcher's archive list, default settings, layout differences).
- PH engine: entry 0x4c283b, i_IntersectSegment 0x43B77D (same code as AvP2's 0x438C9D), CRT memcpy/memmove
  0x4c0b10/0x4c3530, `cmp esi, fs:[0]` in _longjmp at 0x4c1b25. Three relocations gen_relocs.py rejected are
  real (a CRT function table entry, two SEH scope handlers).
- relocations_exclude.csv (both engines): gen_relocs.py took words in the CRT's _getch key table for pointers
  (one even pointed past .data and SRW refused it); relocations.csv = gen_relocs.py's output - exclude + include.
- bssborder: SRW splits .data at the border into two sections that the linker doesn't keep together. PH's .data
  raw size ends (0x4f0000) inside a 0x101-byte table at 0x4effbc, whose memset then ran into the client shell's
  C++ initializer table (the next section in the executable) - the border is 0x4f00c0, after the table.
- Archives: PH runs on AvP2's install. PrimalHunt.exe (avp2src's launcher, PRIMAL_HUNT) puts AvP2's AVP2.REZ and
  SOUNDS.REZ (and the campaigns' if there) first, then its own AVP2X, MULTI, AVP2DLL, AVP2L, DIALOGUE, the patch
  archives and custom. Setting avp2_dir = AvP2's directory (relative, or absolute - turned into ..\ up to the root,
  as every game path is inside the game directory); default the game directory.
- PH's client shell calls GetUserNameA (a developer's name skips the intro movies): "Player".
- render3d: PH's ModelInstance has an environment map after the four skins (+0x1d0), so the animation tracker
  (model pointer 0x1fc) and the hidden pieces (0x260) moved (avp2src d3d/ph/drawmodel_env.cpp, d3d_modelvb.cpp);
  the other object layouts render3d reads are the same in avp2src's PH d3d.ren.
- The default settings file (SR-AvP2.cfg) was still Interstate '76's text: now each game's (game.h).
- host_godot: with window_scale 1 the root viewport kept the engine's first window size (320x200) - the window
  went 320x200 and back to 640x480 before Godot saw the first resize, and no event came for the second; the
  size is asked again (or nudged by a pixel) while window and viewport differ in the 2 s after a change.
- Now: exp1s1 (Predator campaign) plays with HUD and the wrist blades at ~170 frames/s.

### 2026-11-08 — Bink movies
- The engine's BinkVideoMgr loads binkw32.dll and resolves 13 functions with GetProcAddress (decorated names,
  `_BinkOpen@8`, ...). binkw32.dll is now a built-in module (winapi-kernel32.c's module table -> imports.spec
  "static" stdcall stubs -> bink.c). bink.c decodes with FFmpeg (libavformat/libavcodec, found with pkg-config by
  SConstruct; without it BinkOpen fails and the engine skips the movies): the video frame by frame as the engine
  asks (BinkDoFrame / BinkCopyToBuffer, YUV -> the surface's format), the audio track whole at BinkOpen, played as
  a mixer source and used as the clock for BinkWait. The BINK structure in game memory has what the engine reads
  (Width, Height, Frames, FrameNum from 1; the movie ends at FrameNum == Frames - 1).
- The video manager draws through d3d.ren's DirectDraw: RenderStruct::GetHook("LPDIRECTDRAW") / ("BACKBUFFER").
  render.c has stand-ins (com.spec IDirectDraw7 / IDirectDrawSurface7): GetDisplayMode (32-bit RGB),
  CreateSurface (game memory), Lock/Unlock, Blt (colour fill, stretch) and BltFast; the back buffer is the 2D
  frame, written opaque (it's premultiplied ARGB over the 3D picture).
- AvP2 plays foxlogo, sierralogo, ltlogo and lithlogo at startup (with their sound). Primal Hunt has no movies
  (its Third Law logo is a bitmap).

### 2026-11-08 — music
- The music is DirectMusic (Music/<theme>/: a control file, style-based and sequence segments, styles with bands,
  DLS instrument sets). The engine's CLTDirectMusicMgr (lithtech.exe) fails without DirectMusic; music.c replaces
  it: at startup music_install points its vtable (23 slots, ILTDirectMusicMgr's order with the two two-argument
  motif methods last; AvP2 0x4ca674, Primal Hunt 0x4d56a4, exported as lithtech_DirectMusicMgr_vtbl through
  global_aliases.sci; it's in read-only data: mprotect) at x64/music-asm.asm's thiscall stubs.
- Playback is GothicKit's dmusic (a C re-implementation of DirectMusic's segment/style/band/DLS playback, MIT),
  built static from a checkout (SConstruct: DMUSIC=<dir>, HAVE_DMUSIC) with dmusic-patches/avp2.patch: sequence
  tracks (48 AvP2 segments), GUID-only style references, missing instruments not fatal, empty style parts, and a
  segment-end callback so the next segment starts exactly where one ends. All 168 segments render.
- music.c does what the engine's manager did with the control file: intensities (segment lists, repeat count,
  the intensity that follows), transitions (enact time; a manual transition's segment), ChangeIntensity from the
  game, Stop/Pause/volume (hundredths of dB plus VOLUMEOFFSET). Each level gets its own performance and loader;
  rendering is a mixer source. Secondary segments and motifs aren't played (no control file uses them but one
  MOTIF line). AVP2_MUSIC_INTENSITY=<n> starts a level with that intensity (testing); setting music = 0 or
  AVP2_NOMUSIC leaves the engine's (silent) manager.
- rezfs.c: the runtime reads the archives itself (the engine's -rez list from the final command line, later ones
  override, custom is a directory).
- Test directories: autoexec.cfg's MusicVolume was 0 (the retail default is 50).

### 2026-11-08 — smoke test, warped blits, animated surfaces
- Smoke test: every AvP2 single-player world (50) loaded with +runworld for 25 s (scratchpad smoke.sh): no crash,
  170-190 frames/s. Two game instances at once (the same game directory, and the window focus: AvP2 pauses and
  draws black when it loses it) spoil each other's runs - tests run one at a time.
- RenderStruct::WarpToScreen (the Alien HUD's warped surfaces): the warp points' polygon (source xy -> screen xy,
  d3d_WarpOptimized's triangle fan) rasterized into the 2D frame with BlitToScreen's pixel rules.
- SURF_SPRITEANIMATE world surfaces (the engine sets the surface's texture to its sprite's current frame each
  update; p3s2/p3s3 have a few): their batches are keyed by the surface and get their own material, whose texture
  follows the surface's (update_animated_surfaces).
- rezfs: custom is recognized as a directory (fopen succeeds on directories on Linux).
- Music: the game asks for death motifs (PlayerDeathMotifB, AIDeathMotifA) that no style or control file has -
  the original engine fails them too.
- Detail textures and environment maps on world surfaces (SharedTexture::m_pLinkedTexture, +0x2c link type: 0
  detail, 1 env map, 2 env map with alpha): AVP2-Reconstructed's d3dren (unk/100098d0.cpp d3d_DrawWorldTextureBucket,
  d3d_DrawDualTextureWorldBucket) has d3d.ren's arithmetic. Detail UV = the base texel coordinate turned by the
  base DTX's angle x the detail texture's texel size x DetailTextureScale (autoexec.cfg 1.0) x the base DTX's detail
  scale (m_Extra[6..9] + 1); stage 1 ADDSIGNED (DetailTextureAdd 1) or MODULATE. Env maps: sphere-map UVs from the
  reflected view vector (d3d_CalcWorldReflectionUVs) x DetailTextureScale. Detail sampled trilinear (no anisotropy).
- Texel sizes (RTexture +4/+8, g_TextureStageTexelSizes) are 2^UIMipmapOffset / mip 0's size (DtxHeader m_Extra[4]:
  "a 512 can be mapped as a 256 or a 128"): world, world model and sky UVs and detail coordinates use them - about
  a third of m1s1's textures have offset 1 and were drawn twice too large before. Sprites keep the base size.
- Saturate (autoexec.cfg 1): d3d.ren draws the light (lightmap or gouraud colour) first and the texture over it with
  SRCBLEND DESTCOLOR + DESTBLEND SRCCOLOR = 2 x texture x light, so vertex lit world polies are 2x too (they were
  1x: m1s1's ground was half as bright as the Wine reference; now within ~5%).
- AVP2-Reconstructed (lemiur) reconstructs d3d.ren too (src/d3dren, 53% matched): the first place to look for
  renderer behaviour. The NOLF2 modernizer has no engine/renderer source (SDK + game code only).

### 2026-11-08 — frame times, the engine clock
- Frame timing (host_godot.cpp): per presented frame the interval, split into the game's own work (engine +
  render3d's scene update), the 2D frame's upload and the Godot frame; per second the average, p50/p95/p99, worst
  and hitches (> 25 / > 50 ms). AVP2_FRAMESTATS=1 logs it; show_fps = 1 (AVP2_SHOW_FPS=1) shows it at the top.
  m1s1 while turning and walking (RTX 3090, 180 Hz display, vsync on): a steady 5.56 ms, p99 < 8 ms, no hitches
  (game ~1.3 ms, upload 0.1 ms, Godot ~4.2 ms). The 2-second hitches seen first were the tests' own screenshots
  (AVP2_DUMP_GODOT reads the frame back and writes a PNG).
- Uneven motion: the frames were even but the player moved 1.50 units most frames and 1.80 every ~10th
  (AVP2_R3D_CAMLOG=1 logs the camera per frame). LithTech's clock is time_GetTime() = (timeGetTime() - base) *
  0.001 (Jupiter's kernel/src/sys/win/timemgr.cpp; AvP2 0x49EF10, PH 0x4A6530, time_Init 0x49EF00 / 0x4A6520): in
  whole milliseconds a 5.56 ms frame is 5 or 6 ms, 20% steps (16/17 ms = 6% at 60 Hz). instruction_replacements.sci
  makes time_Init / time_GetTime call x64/time-asm.asm's stubs: SDL's performance counter, the time as a double
  written into time_GetTime's own stack scratch (thread safe) and loaded with fld. Speed variation per frame
  4.9% -> 1.2%, the average unchanged.
- AVP2_KEEP_ACTIVE=1 (tests): the game stays active without the window focus (it pauses and stops drawing
  otherwise; something else on the desktop taking the focus spoiled automated runs).
- Detail textures go on the texture before the light on vertex lit polies too: d3d.ren draws the light first
  (lightmap or gouraud colour; DrawWorldTexturePoly) and every textured pass the same way (white vertex colour,
  2x blend), so it is 2 x light x (texture + detail - 0.5). Adding the detail after the light blackened dark
  surfaces (m1s1's rock block side: 2.6 vs the reference's 7.1; now 7.4). m1s1's start view now matches the Wine
  reference within ~10% on the ground and the rock faces (mean and contrast).
- Models are alpha tested only with FLAG2_CHROMAKEY (d3d.ren: d3d_DrawChromaKeyModels between
  d3d_BeginChromaKeyPolyPass / End, ALPHAFUNC GREATER than the texture's AlphaRef); solid models ignore their
  texture's alpha. a1s2 starts with the camera inside the "bursting_chest" models, whose skins have alpha: with
  alpha testing they vanished and the nest showed; now the view matches the Wine reference (the chest's inside).
- Debugging: AVP2_R3D_DUMPBSP=<name part> (a BSP's polies: surface flags, texture), AVP2_R3D_DUMPMODEL=<file name
  part> (model rebuilds: box, skins, alpha).
- The 3D picture follows d3d.ren's projection: FovX across the scene rectangle's width and FovY across its height,
  independently (m1s1: 90 x 78 degrees, not 4:3). The camera keeps FovY (KEEP_HEIGHT), the SubViewport's aspect is
  tan(FovX/2) / tan(FovY/2) and its picture is stretched into SceneDesc m_Rect (game screen pixels mapped to the
  window; render.c passes the screen size: r3d_set_screen_size). Before, the view was ~94 degrees wide at 4:3; m1s1's
  rock edges now line up with the Wine reference (1 px at 640x480). The player-view weapon's own FOV (ExtraFOVX/
  YOffset, 95 x 115 in AVP2-Reconstructed's formula) made the knife lie flat - left out (AVP2_R3D_WEAPONFOV=x|y|xy).
- Wide-screen and multi-platform research done (local notes): Hor+ plan (pillarboxed 2D first; then wide modes,
  the menu's 4:3 filter, engine-side Hor+ so Get3DCameraPt agrees); Windows via MinGW on I76's base, macOS via
  Rosetta needs NASM Mach-O relocations for 32-bit absolute addresses, native arm64 means SRW's OUT_LLASM.

### 2026-11-08 — macOS (Apple Silicon through Rosetta 2): the recompiled code as an image
- The recompiled code addresses everything with 32 bits; on Linux it is linked into the program at 0x10000000. A
  64-bit Mach-O program is position independent and above 4 GB, and Mach-O has no 32-bit absolute relocations
  (nasm -fmacho64 refuses them; ~18,600 in lithtech alone). Instead the recompiled code (the modules and the
  runtime's asm stubs) is one image at 0x20000000, linked with GNU ld on Linux (tools/mkblob.py): every symbol it
  needs from outside is a C function (688; checked), reached through thunks (jmp [slot]) whose slots the program
  fills when it loads the image (blob.c, before anything else takes low memory). The program finds the 101 image
  symbols it uses (asmsyms.h: the asm2c stubs, entry points, vtables) in a constant table generated per game
  (blob_link_<program>.cpp). The image's code calls C with System V's convention - the same on Linux and macOS.
- With the program above 4 GB, everything the game sees must still be below 2 GB: the runtime's own malloc/calloc/
  realloc/strdup/free go to the low allocator (lowalloc.h, force-included into the runtime's C files; free() of
  a system pointer goes to the system), and the statics whose addresses reach the game (pseudo handles, the process
  heap, command line, environment, modules, GDI stock objects, Miles' listener, winsock's hostent, errno, cvar and
  ray query buffers, the canvas interface) are copies in low memory (lowobj.h: <name>_init + LOW_OBJ).
- `scons blob=1` builds that on Linux (SR-AvP2-x64-blob, position independent): m1s1, p1s1, a1s1, a6s1, the menus
  with the movies and Primal Hunt's exp1s1 run. `scons device=pc64-macos` is the macOS build (clang, x86-64,
  __PAGEZERO 64 KB, the images from a kit made on Linux: tools/mkmackit.sh); BUILDING-macOS.md. Not yet run on a Mac.
- Also portable now: the fault handler (macOS register names, SIGBUS), fixed mappings without MAP_FIXED_NOREPLACE
  (sys_map_fixed; ranges reserved in a constructor, sys_reserve_fixed), the recursive mutex initializer, the
  program's directory (SDL_GetBasePath), libgodot.dylib without RTLD_DEEPBIND; `rendering_method` setting.

### 2026-11-08 — shader preloading
- Godot 4.7 (Forward+) parses a shader when it's set, compiles it for the GPU in worker threads and makes the first
  draw with it wait; the pipelines are compiled when an object is added and waited for when it's drawn. With cold
  caches (the first run, a new Godot or driver; measured with XDG_DATA_HOME and __GL_SHADER_DISK_CACHE_PATH pointing
  to new directories) m1s1's first frame took 117-144 ms in Godot and each new sprite variant stalled its frame by
  33-73 ms (the first shots: muzzle flash, smoke, shells). With warm caches none of that showed.
- Now every shader is made once when the renderer starts (r3d_init from ren_Init) and kept for the whole run (the
  world's were remade for each level). The set is small and fixed: 11 world kinds (world, models, really close,
  sky), 6 sprite shaders (blending x depth test: the rest of the old ~160 variants - placement, glow, bias, really
  close, fog - is the material's `flags` uniform), 2 line shaders. The light scales (lightmap 2x, vertex colour 2x
  with Saturate) became the global uniform lt_scales (set with the fog), so no shader depends on console variables.
- Preloading as Godot's documentation recommends (tutorials/performance/pipeline_compilations.rst, "Reducing
  stutter from shader (pipeline) compilations"): the scene keeps a hidden instance of each shader with each vertex
  format it's used with, for the whole run. Forward+ and Mobile compile the pipelines of every surface in the scene,
  hidden ones too, at the next frame (the "surface" step, in the background; checked in Godot 4.7's
  render_forward_clustered.cpp: _update_dirty_geometry_pipelines has no visibility test), and recompile them all
  when a rendering feature is turned on later (MSAA, ...), which is why they stay. The Compatibility renderer
  (OpenGL) can't precompile; the documented way there is the legacy one: draw them in the view for a frame. So with
  gl_compatibility they're shown for the first 3 frames (zero-area, transparent triangles; an AABB containing every
  camera keeps them from being culled), then hidden. (The first version drew them in front of the camera with every
  renderer.) The documentation's shader baker is for exported projects' shader files, not runtime-made shaders.
- Cold caches, m1s1 with three weapons fired:
  - Forward+: the 46 pipelines in the game's first frame (264 ms once; 57 ms warm), the level's first frame 3.6 ms in
    Godot, no slow frame and no pipeline compilation after the start; before the preload: the level's first frame
    144 ms, 33-73 ms per new effect.
  - Mobile: 38 pipelines at the start, none while drawing.
  - Compatibility (opengl3): 2.0 s once in the first frame, nothing after; before the preload: the level's first
    frame 1.2 s and 80-100 ms per new effect.
  - Also Primal Hunt (exp1s1) and blob mode (a1s1): 46 at the start, none while drawing. a1s1's picture unchanged
    (a matching frame identical to the earlier builds').
- AVP2_FRAMESTATS=1 also logs Godot's pipeline compilations (per second; while drawing = stutter) and each slow
  frame (> 25 ms in Godot or > 50 ms in the game) with its compilations; AVP2_DEBUG logs every shader made (" late"
  after the preload).

### 2026-11-08 — native arm64 (Apple Silicon): the recompiled code through SRW's llasm output
- Why: the macOS build runs the x86-64 code through Rosetta 2. A native arm64 program can't keep the game's memory
  in the low 4 GB, so the 32-bit addresses become offsets from a base (`pointer_offset`) - SRW's llasm output with
  llasm -ptrofs, as Septerra's arm64-darwin build. llasm turns it into LLVM IR, llc into any 64-bit target.
- Done: all 8 modules (AvP2 and Primal Hunt: lithtech, cshell, server, object) translate (SRW 0 errors), go through
  llasm (-m64 -ptrofs -pic) and compile with LLVM 23 to arm64 Mach-O objects (server 5 MB .. object 27 MB; llc
  25-115 s, up to 8 GB) and to x86-64.
  - SRW's llasm backend (the x64 one had them): jecxz; and/or/xor reg, <address>; test/or/and reg, reg when nothing
    needs the flags (the jcc after it compares the register); rol/ror r8 (the existing helper); bt/bts [mem], reg;
    test r8, r8h and cmp [mem], r8h; fld/fstp tword (FLD_TWORD/FSTP_TWORD: the runtime converts the 10 bytes at the
    address); f2xm1 fclex fldl2e fldl2t fldpi fpatan fprem fprem1 frndint fscale fsincos ftst fxam (x87_*_void),
    fldenv/fnstenv (the 28 bytes at the address); (rep) cmpsd (the flags from the last compared dwords, reloaded);
    relocations to the image base (lea eax, [edi+0x400000]: the address itself, as the x64 output; crashed SRW);
    stdcall import names (_AIL_3D_orientation@28 -> _AIL_3D_orientation_28: '@' isn't valid in LLVM); data
    addresses inside a data label (daddr label[offset]; llasm extended to take it). SRW_LIST_ERRORS=1 reports every
    region's error instead of stopping at the first. The x64 output is unchanged (server byte-identical).
  - SR/llasm-support: the new x87 operations, 80-bit <-> double (checked against the x87: 2M doubles both ways and
    1M 80-bit roundings identical), repe/repne cmpsd.
  - SCI files in SRW-<module>/llasm/: instruction_replacements.sci (the fs:[0] compare in llasm's language, up to
    the je target so that it's a label), instruction_flags.sci (~140-280 entries per module, tools/mkiflags.py):
    llasm has no flags register, so a label whose code reads flags set before the jump to it (MSVC's
    `jne L ... L: sbb eax, eax`, ~50-100 per module) needs the jumps to it (and the instruction falling into it) to
    write them. After a call the flags come from the callee: MSVC's CRT _fload returns ZF, so its rets write ZF.
  - Scripts: tools/build_srwll.sh (SRW with OUT_LLASM), gen_all_llasm.sh (-> SR-AvP2/llasm/<module>/, generated,
    not committed), SR-AvP2/llasm/{llasm,macros,extern}.llinc.
- Next (the runtime for llasm): bridges for the ~690 imports (the module objects call e.g. _CloseHandle as llasm
  procedures taking the CPU state: a llasm backend for gen_imports.py), the x64 asm stubs (c2asm, music, time,
  canvas, raw, fs memory) in llasm, game pointers through pointer_offset in the C runtime (ptr32_t, REG2PTR;
  ~230 direct casts), the memory layout (lithtech.exe's image at offset 0x400000, the low allocator inside the
  4 GB window). Built and tested first on Linux x86-64 with -ptrofs, then arm64 macOS.

### 2026-11-08 — the llasm version runs (Linux x86-64, direct low addresses)
- `scons llasm=1` -> SR-AvP2-llasm / SR-AvP2PH-llasm: the recompiled code from the llasm output (llasm/SConscript:
  llasm -O | llc -O=2, all 8 modules in ~40 s), the runtime unchanged (game addresses still low: a non-PIE program at
  0x10000000). It separates the translation's bugs from the pointer offset work (ptrofs=1, next). m1s1 and Primal
  Hunt's exp1s1 play like the x64 build: game code 1.2-1.4 ms per frame (x64 build 1.05-1.3 ms), same load times.
- The runtime's llasm side: gen_imports.py writes llasm/imports.llinc (import name -> <name>_asm2c, included by
  extern.llinc before the modules' "proc <import> external"), imports-asm.llasm / com-asm.llasm (each procedure takes
  its arguments from the emulated stack, calls the C function, eax = result, pops for stdcall; pointer arguments and
  results from the C prototypes - llasm converts them with -ptrofs) and runtime-asm.llasm (music/canvas thiscall
  stubs, time_Init/time_GetTime, call_asm_eax). llasm/llasm-cpu.c: the CPU state per thread (Septerra's), C ->
  recompiled code (call_game: arguments onto the emulated stack, eax = the function, llasm's global procedure pushes
  its return procedure), the helpers' handlers for int/absolute addresses (Raw_UnexpectedHandler).
  The DLL exports get their module prefix as in gen_all.sh (tools/prefix_exports.py, also llasm's c_<name>).
  The llasm procedures are made internal (sed in the build): local symbols loc_<address> for perf and gdb.
- Translation bugs found by running it:
  - llasm -O forgot its temporaries after every call (they're SSA values, valid after it): a store to an absolute low
    address (a helper call) between cmp and jae -> "uninitialized register tmpcnd". Now only the x86 registers are
    reloaded after a call.
  - x87 compares with a NaN: llasm's inline fcom and the helpers' X87_CMP gave "greater" (no bits), the x87 sets
    C3 C2 C0 (unordered).
  - The CRT's _CIacos (in every module): `call <load helper>; call L` with `L: push edx; fstcw; je special` - the je
    reads ZF from the helper's rets (cshell 0x10174498) through a call to a label inside the function. mkiflags.py now also follows
    the calls to a flags label (the instruction before each call, or that call's callee's rets). Without it acos
    took the special-value path with stale flags: an angle of -inf and an endless `while (a < 0) a += 2pi` loop
    while loading m1s1.
- Diagnostics: AVP2_DEBUG=1 reports waits / blocked critical sections of 50 ms or more and Sleeps of 50 ms or more
  with the thread and the recompiled caller (llasm: the return address on the emulated stack, nm -> loc_<address>).
- Both builds spend ~16% of the CPU time in Game-Memory.c's allocator (find_largest_free_block: a linear search of
  the free blocks for the game's HeapAlloc) - worth a better allocator for the choppiness item.

### 2026-11-08 — pointer offsets (scons llasm=1 ptrofs=1) and device=arm64-macos
- The game's addresses are offsets from pointer_offset: a 4 GB window just below the program (Game-Memory.c,
  Septerra's initialize_pointer_offset), the llasm code adds the base to every access. On Linux x86-64 the window is
  at 0x5629... (far above 4 GB): m1s1 and exp1s1 play like the other builds (game code ~1.5 ms per frame). This is
  the memory model native arm64 macOS needs; the same code compiled for arm64 is the Mac build.
- The runtime's side: platform.h's G2H (game address -> pointer, 0 -> NULL), H2G (back), GADDR (no NULL test, the
  hot accessors), in_game_window. Every (uintptr_t) cast between game addresses and pointers (~135: kernel32,
  user32, advapi32, msvcrt, miles, music, render, render3d, dinput, bink, main) now goes through them; ptr32_t fields
  already did. The compiler confirms no other int <-> pointer casts (-Wint-to-pointer-cast). Handles the game sees
  as small numbers (std handles 1-3, INVALID_HANDLE_VALUE, ordinals and MAKEINTRESOURCE, class atoms) are compared
  as game values (H2G). What the game sees is allocated in the window as in blob mode (SR_GAME_WINDOW: lowalloc.h
  forced into the runtime's files, LOW_OBJ); lowalloc tells its blocks from the system's by x86_owns() (the system
  heap can be inside the window too). lithtech.exe's PE image can't be at 0x400000 (the program is there): anywhere
  in the window, GetModuleHandle(NULL) returns it (lithtech_image). The stray-read handler works on window offsets
  and on arm64 (ESR WnR; macOS __es.__esr, Linux's ESR record; 16 KB pages).
- device=arm64-macos: clang -arch arm64, the llasm version with pointer offsets, Homebrew in /opt/homebrew, godot-cpp
  /LibGodot arm64. The llasm objects come from a kit made on Linux (tools/mkarm64kit.sh: llasm -ptrofs + llc
  -mtriple=arm64-apple-macosx11.0.0; llasm_kit=<dir> or mac-kit-arm64/; private, game-derived), so the Mac needs
  only Xcode's clang. The kit mechanism is tested on Linux with an x86-64 kit (TRIPLE=...). The kit's 734 undefined
  symbols are all defined by the runtime. On macOS the window may start inside __PAGEZERO when the program's slide
  leaves less than 1 MB below it (its first MB is never used). Not built on a Mac yet (no SDK here).

### 2026-11-08 — the allocator, 2D blits, dynamic meshes, settings, wide-screen (step 1)
- Game-Memory.c's allocator is TLSF (two-level segregated fit: free blocks in lists by size class, two bitmaps find
  one, merging through boundary tags) over 16 MB pools in the 32-bit window: malloc/free in constant time. The old
  one took the largest free block and walked the free list for the next largest (22% of the CPU in m1s1). Checked by
  a stress test against a model (3M random malloc/free/realloc, the heap's structure walked, ASan/UBSan) and a
  benchmark (26x faster on a fragmented heap); it no longer shows in a profile. The smoke test (all 50 AvP2 and 19
  Primal Hunt worlds) passes with it.
- ren_BlitToScreen_c (the HUD, ~465 blits / 120k pixels a frame): clipped once, a column map instead of a 64-bit
  division per pixel, one loop per blend mode whose store is unconditional (the colour key selects the old pixel), so
  the compilers vectorize it. Bit-identical to the old version on 200k random blits.
- Dynamic meshes (models, particle and line systems, poly grids, canvases): the surfaces are rebuilt only when their
  layout changes (arrays, counts, indices, material); otherwise the vertices are written into Godot's buffers
  (mesh_surface_update_vertex_region / _attribute_region). Our encoding of the layout (RenderingServer's offsets and
  strides) is compared with Godot's own once per array combination (mesh_get_surface) and the surfaces are rebuilt if
  it differs; the instances get custom AABBs (an in-place update doesn't recompute the mesh's). Particle systems and
  canvas batches are padded to a power-of-two room with empty, transparent triangles. In m1s1: models 12040 in place
  / 0 rebuilt per 1200 frames, particles 4800/0, canvases ~95% in place; p1s1/a6s1/exp1s1 poly grids all in place.
- AVP2_FRAMESTATS / show_fps also show draw calls, objects, primitives and GPU time (the 3D view, the window): m1s1
  ~340 draw calls, 28k primitives, GPU 1.0 + 0.3 ms. The 3D SubViewport isn't updated while it's hidden (menus); the
  2D frame is uploaded only when it changed, without the copy-on-write copy of the shared Image; the mouse's
  sub-pixel motion is kept; per-frame shader parameter names are StringNames made once.
- Settings (SR-AvP2.cfg / AVP2_<KEY>): vsync (Godot's modes), max_fps, msaa, render_scale + upscaler (bilinear, fsr,
  fsr2, nearest, metalfx_*), anisotropy, window_size (e.g. 1280x720), aspect.
- Wide-screen step 1 (aspect = wide, the default): in a window that isn't 4:3 the game's 2D frame (HUD, menus,
  movies) keeps its aspect, centred (host ghost_frame_rect; the mouse is mapped through it), and a scene as wide as
  the game's screen spans the window - Hor+: the same vertical field of view, the tangent widened with the width
  (render3d post_update). aspect = 4:3 puts the 3D view in the 4:3 frame too (bars), stretch is the old behaviour.
  The HUD, the vision overlays and the engine's own projection (target brackets) stay in the 4:3 middle; step 2
  (16:9 render modes, engine-side Hor+) is open.

### 2026-11-08 — Windows x64 (MinGW, tested under Wine), additive sprites, the weapon's projection
- `scons device=pc64-windows` builds SR-AvP2.exe / SR-AvP2PH.exe with LibGodot as a DLL (Godot 4.7.2 cross-built with
  MinGW, d3d12=no), godot-cpp, a minimal static FFmpeg (Bink only) and dmusic for MinGW (BUILDING-windows.md). Under
  GE-Proton the menu, the intro movie, m1s1 (192 fps) and Primal Hunt's exp1s1 play. Fixes on the way: the C
  runtime's time functions (gmtime_r/localtime_r/tm_gmtoff -> _s versions, utc_offset), gethostname through hostnet,
  SetFileTime, mkdir; LibGodot loaded with SDL_LoadObject; files opened binary (Windows' C runtime defaults to text:
  the REZ reads were garbled - "Error copying file cshell.dll"); vfs_resolve keeps real drive paths (avp2_dir); the
  stray-read zero page in the vectored exception handler (64 KB granules); sys_dump_low_map with VirtualQuery.
- lithtech.exe's image at 0x400000: under Wine heaps and the locale files were there before main(). The program is
  now linked at 0x3f0000 with an uninitialized section first (x64/lithimg.asm, a linker script made from MinGW ld's
  default) that covers 0x3f1000 - 0x510000; peimage.c copies the image into it.
- Additive sprites (and particles, poly grids, canvases with ONE/ONE): d3d.ren blends ONE, ONE - the alpha isn't
  used - but our shader wrote ALPHA = the texture's x the object's alpha and Godot's blend_add multiplies by it: the
  marines' shoulder lamp glows (object alpha 9/255) were invisible. ALPHA = 1 for additive, as the world shader.
- The player-view weapon (models with m_bFovOffset): the game's d3d.ren (0x1001af52) builds diag(tan(fov / 2)) with
  fov = ExtraFOVX/YOffset degrees + the model's offset + the view's FOV (95 x 115 in m1s1) and uses it IN PLACE of the
  view's mFOVScale (1 / tan(view fov / 2)): the weapon is scaled by tan(weapon / 2) x tan(view / 2) - x 1.09, y 1.27.
  The knife's blade angle then matches the Wine reference (27 degrees; the ratio tan / tan made it 14, none 31).

### 2026-11-08 — wide-screen step 2: wide game resolutions with Hor+ in the engine
- game_resolution = 1920x1080 (SR-AvP2.cfg / AVP2_GAME_RESOLUTION) writes the render mode into autoexec.cfg
  (SCREENWIDTH/SCREENHEIGHT, GameScreenWidth/GameScreenHeight) before the engine reads it - the game's display menu
  offers only 4:3 modes, but the engine and the game run in any (the HUD is in native pixels, anchored to the edges;
  the menus are centred). Without more the 4:3 fields of view are spread over the wide screen (stretched).
- camera.c: Hor+ where the engine keeps the FOV, the camera object, so the scene, Get3DCameraPt (target brackets,
  the vision overlays' size) and culling all agree: ILTClient's SetCameraFOV stores FovX with tan(FovX / 2) x the
  screen's aspect / (4/3) (FovY as it is), GetCameraFOV returns the game's own value (its zoom reads and re-sets the
  FOV: no compounding). ILTClient is a function table the engine fills (lithtech.exe 0x4089b0, PH 0x409xxx; the
  entries at +0x2a4/+0x2a8 hold the recompiled functions); cshell gets it in CreateClientShell, which the engine
  takes from cshell's GetClientShellFunctions - our GetProcAddress returns a wrapper of that, which hands out a
  wrapper of CreateClientShell, which changes the two entries. Bridges: x64/camera-asm.asm, the llasm ones from
  gen_imports.py (runtime-asm.llasm), asmsyms.h for blob mode. m1s1 at 1600x900: FovX 90 -> 106.3 degrees.
- The weapon's projection (render3d) takes the game's own FovX, so the weapon keeps its 4:3 look on a wide screen.

### 2026-11-08 — vision modes: a texture state's own blending
- The game switches textures' state changes per vision mode from their DTX command strings ("ViewModes <mode>
  <state> ..."). The Predator's (p2s1) only set fog and the colour op; the Alien's Hunting / Nav visions give the
  prey's skins (marines, their attachments, other aliens: "Hunting NavAliens", "NavVision NavAliens") the NavAliens
  state: ALPHABLENDENABLE with SRCBLEND = DESTBLEND = INVSRCCOLOR - src (1 - src) + dest (1 - src) per channel, the
  bright parts dark, the dark ones see-through. render3d parses the blend (also ONE ONE and alpha) and models whose
  skin has one are drawn with that kind; INVSRCCOLOR as mix blending with alpha = src's luminance and colour
  src (1 - src) / alpha (Godot's blending can't weigh per channel). The skins' blend is in the model's rebuild key.
  Not compared with a reference (the Wine reference can't be steered into a vision mode).

### 2026-11-08 — the world's alpha as d3d.ren, vertical fog
- d3d.ren alpha tests only in its chroma key pass (d3d_BeginChromaKeyPolyPass: ALPHATESTENABLE / COLORKEYENABLE, off
  again at its end): the FLAG2_CHROMAKEY world models and models. The main world is drawn without it - opaque, also
  where a texture is a cut-out ("ColorKey 0 0 0;AlphaRef 128;": 8 such textures in m1s1's main world) - and world
  models without the flag too. We alpha tested every texture with alpha (m1s1: 418 main world polies, a few hundred
  to 8128 per level). Now: the main world opaque; world model instances without FLAG2_CHROMAKEY get the opaque
  material for their alpha tested surfaces (set_surface_override_material; the BSP's mesh is shared).
  AVP2_R3D_WORLDALPHA=1: the old behaviour; AVP2_R3D_DUMPALPHA=1 lists the main world's alpha textures. Not compared
  with a reference (no level shows one at its start, where the Wine reference can look).
- Vertical fog (VFog, VFogMinY/MaxY, VFogMinYVal/MaxYVal, VFogMax, VFogDensity): d3d_CalcVerticalFogAlpha ported to
  GLSL, by vertex as d3d.ren (its fog is the vertices' specular alpha), instead of the distance fog when VFog is on.
  m2s1 turns it on (y -2000 .. -1500, density 1). Sprites: by pixel.
- GPU times vary with other GPU users (a vLLM engine shares this GPU): a1s1's cutscene measured 1 ms and 5.6 ms
  with the same code; m1s1 0.2-1 ms.

### 2026-11-08 — multi-frame and shadow-map light animations
- d3d.ren (UpdatePolyAnimatedLightmap and its builders): a plain animation's layer is its two current frames
  (m_iFrames[2] +0x34) mixed by m_PercentBetween (+0x3c, 0-255); a shadow-map animation's frames are coverage masks
  (runs alternating 0 / 255), mixed the same way, then every lightmap texel is lit by the animation's point light
  (+0x44 position, +0x50 colour, +0x5c radius): the falloff table (1 - d^2 / r^2 in 64 steps, x LightSaturate) x the
  mask x (2c - 255) per channel - so a light with a colour below 128 adds nothing. Layers after the first are scaled
  by m_fBlendPercent (+0x40).
- We baked multi-frame animations as they were when the world was built and left shadow maps out. Now every animation
  but LightAnim_BASE is a changing layer (up to 4 per poly, 40 blocks): its frames (or masks) are blocks next to the
  base in the lightmap page, the vertex attributes give each layer its animation, first block and kind, and the
  shader picks / mixes the current frames from lt_anims (blend, on, frames, mix) and lights the shadow maps from
  lt_anim_lights (by pixel; d3d.ren by texel). No lightmap uploads while the level runs. m6s1, m7s1, m4_open, a3s1,
  A2s2, a7s1, p4s1 and exp1s1 run without errors; not compared with a reference.

### 2026-11-08 — models' environment map pass
- d3d.ren (ModelDraw, setupmodel): a model with FLAG_ENVIRONMENTMAP (bit 5; the cloaked Predators - AIChameleon - and
  env-mapped Props), EnvMapEnable on (the game's "chrome" option) and RenderStruct's m_pEnvMapTexture (+0x10c: the game
  runs "EnvMap WorldTextures\Chrome.dtx") gets a pass of that map before its own: u, v = the vertex normal in view
  space . the view's x, y rows / EnvScale / 2 + EnvPanSpeed x the camera's x, z + 1/2, modulated by the vertex colour,
  alpha blended by the vertex alpha (Chrome.dtx's alpha is 0 everywhere). The model's own pass covers it unless the
  model is translucent - so we draw it for translucent ones only, as a material whose next pass is the model's
  (faces' normals: the skinned vertices have none). EnvMapAll (console variable): every model, the map only - m1s1's
  marines turn chrome with it, as expected. No cloaked Predator at a level start to compare.

### 2026-11-09 — models' light and the world's dynamic lights as d3d.ren; models' detail textures
- Models (d3d.ren SetupModelLight / SkinAndLightPieceVertices): the object colour is x ModelSaturation (2) with Saturate,
  the vertex colour (ambient from the light grid, the sun, the objects' dynamic lights, all x the object colour, +
  the scene's ModelLightAdd) is clamped to 255 after it, and stage 0 modulates the texture 1x. We clamped first and
  doubled after (the world's 2x), so a lit model could reach 2x its texture; and the world's shader added the world's
  dynamic lights to models a second time. m1s1's marines were tan-yellow, the reference's dark: now they match.
- The world's dynamic lights, in the light before the texture multiplies it (d3d.ren draws the light first):
  lightmapped polies' lightmaps get them (d3d_RefreshWorldPolyLightmap -> ApplyPolyDynamicLightsToLightmap: + the
  falloff table (1 - d^2 / r^2, 64 steps, x LightSaturate) x (2c - 255) per texel, clamped; FLAG_SOLIDLIGHT: the
  colour as is); vertex lit polies (LMDynamic + MultipassGouraud: DrawWorldPolyDynamicLightPasses) get a projected
  texture of the light, (max(c, 127) - 127) x (1 - (in-plane / 1.25 r)^2 - (plane distance / r)^2), drawn twice with
  Saturate; FLAG_DONTLIGHTBACKFACING lights skip polies they're behind. We had the vertex lit formula on lightmapped
  polies, added after the texture. m1s1: the marines' lamps now light the ground under them yellow as in the reference.
- Models' detail textures (FLAG_DETAILTEXTURE, a Prop's DetailTexture): the skin's linked texture at the skin's UVs x
  ModelDetailTextureScale (3), ADDSIGNED after the light (DrawModelRenderPasses, FillModelDetailTexCoords). Only two
  props in all of AvP2's worlds set it (milk cartons in a1s2 and m1s4) and their skin has no linked texture, PH none
  (all PropTypes have DetailTexture = 0): no visible change in the shipped games. ModelSpecular: no model has
  SpecularEnable.
- Uncapped (vsync 0) with the GPU otherwise idle: 300-2000 fps, the 3D view 0.1-0.6 ms of GPU; the game's own code
  (1-3 ms) is the limit. The earlier 5.6 ms GPU in a1s1 was the other GPU user.

### 2026-11-09 — p3s2's black crates; music motifs
- p3s2 against a Wine reference (wine_ref.sh now passes +windowed 1 itself): the crates on the starting lift were
  black. Their polies are vertex lit with 79 light animation references (LightAnim_BASE and a LightGroup per light
  group of the room); poly_vertex_light / poly_lightmap gave up above 64 and left the light at 0. d3d.ren has no such
  limit (RelightWorldPolyVertices walks m_nLMAnimRefs): now up to 4096. AVP2_DEBUG's build line counts such polies:
  only m1s2 (4656: its crates and buildings were black, now as in the reference) and p3s2 (1902) have them.
  The level's two sprite-animated polies (SURF_SPRITEANIMATE) are far from the start and look right.
- New debugging aids: AVP2_R3D_DUMPBSP="=name" (exact) / "*sprite" (sprite-animated polies) with each poly's
  lightmap, layers, animation references and vertex light; AVP2_R3D_SKIP=<letters> (object types, frozen light
  animations, no dynamic lights).
- Music motifs: not needed. Music.txt's PlayerDeath / AIDeath events name PlayerDeathMotifA/B and AIDeathMotifA/B
  (EventB MotifB/C), which no style of AvP2 or Primal Hunt has: the original plays nothing for them either. The only
  motif in the styles (P1Ambient1.sty's DrumAccent, EventA) belongs to an event no level triggers; the events levels
  trigger (Event2, Event9, EventM4S1A, ...) are wave files.

### 2026-11-09 — wide resolutions in the menu; gl_compatibility; lightmap filter option
- The Graphics page (CFolderDisplay::GetRendererData) listed only modes with width = height * 4 / 3. The test's
  `cmp ecx, edx` becomes `cmp ecx, ecx` in the copy of cshell SRW translates (SRW-cshell/patches.txt,
  SRW-ph-cshell/patches.txt; tools/patch_pe.py, applied by gen_all.sh and gen_all_llasm.sh - an
  instruction_replacements.sci jump loses its target's label and, in llasm, the flags), and render.c lists 16:9,
  16:10 and 21:9 modes besides the 4:3 ones. Tested with AVP2_INPUT_SCRIPT (+NoMovies 1 skips the intro): the page
  steps 1152x864 -> 1280x720 -> 1280x800, and leaving it switched the game to 1280x800 (camera.c widens the view; the
  menus keep their 640x480 layout, centred, as at any larger resolution).
- gl_compatibility's nearly black 3D: its scene shader takes ALBEDO as sRGB (`albedo = srgb_to_linear(albedo)`) while
  our shaders hand it linear values (to_linear of the D3D-style sRGB result): converted twice. Its conversions are
  approximations (a cubic, then 1.055 x^(1/2.4) - 0.055) that also lose ~40% of the darkest colours, so there
  to_linear solves for the value they turn back into the colour (3 Newton steps; CURRENT_RENDERER in the shaders).
  The shaders that place depth themselves (the sky behind everything, really close objects in front) assumed
  reverse z in 0..1; gl_compatibility has OpenGL's -1..1 (near 1, far -1): lt_depth01 / lt_depth_clip. m1s1 now
  within 2% of Forward+ in gl_compatibility; mobile within 4% (it was "darker" only by comparison with older frames).
- Options (enhancements, off by default): lightmap_filter = bicubic (a B-spline over 4x4 texels from 4 bilinear taps;
  the lightmap blocks then get a 2-texel border, the light animations' block stride follows) - softer light
  gradients, no seams in a1s1; debanding = 1 (Godot's dithering of the 8-bit output).
- p3s2's blue streak + crate flash near the lift's top: a game event (in some runs only, in all builds, also with the
  light animations forced), not the renderer. New debugging aids: AVP2_R3D_ANIMLOG, AVP2_R3D_ANIMON / ANIMOFF,
  AVP2_R3D_DEBUGVIEW (light, vertex colour, light scales, lightmap, inputs).


### 2026-11-14 — the world ran 1.8x real time above 100 fps (cut dialogue); a bigger HUD and subtitles
- Reported in play: characters' speech was cut off. AVP2_SOUNDLOG=1 (miles.c: what the engine does with the 2D
  samples and streams) showed one-shot sounds stopped while playing, at 0.34 s + 0.56 x their length, with the
  player standing still - Sounds/Dialogue/29106.wav (8.5 s, its headers right) stopped at 5.15 s. The engine's
  CServerMgr::Update (AvP2 lithtech.exe 0x485850, Primal Hunt 0x48b5d0) runs a world tick on every call and counts
  it at least 10 ms (m_FrameTime = clamp(time since the last call, 0.01, 0.2)); the client calls it every frame, so
  at 180 fps (5.56 ms frames) the world - AI, physics, timers, the server's sound timers - ran 1.8x real time. A
  lip-synced line is played by the client, but the server keeps a silent copy and ends the lip sync (stopping the
  client's sound) when its own timer runs out (CCharacter::UpdateSounds -> KillVoicedSound).
  instruction_replacements.sci (x64 and llasm, both games) does what avp2src's modern build does (M-P1-23): the last
  tick's time (+0x8e4 / PH +0xae4) is kept only when a tick runs, and the tick runs only when the time since it
  (+0x8e0 / +0xae0) is at least 10 ms or negative (one unsigned compare of the float's bits with 0.01f's); otherwise
  the call goes on to the object queue flush (packets and the network are still handled every call). The
  campaign's opening scene now runs in real time and every line plays to its end (also the llasm build).
- Cinematic dialogue sounds twice at once (the lip-synced sound and the server's local copy, CCharacter::
  PlayVoicedSound with the external camera on): the original code does that too.
- The campaign page's difficulty list: a click picks a level; from the keyboard Up selects the list and Left / Right
  change the level (CListCtrl with EnableMouseClickSelect: Up / Down move between the page's controls) - as the
  original. At 1920x1080 the 640x480 page is small and the chosen level's white hard to tell from the others' green.
- A bigger HUD, menus and subtitles: the game's 2D is drawn in the game's resolution in fixed pixel fonts (the
  subtitles: the message font, Subtitle.cpp), so at 1920x1080 they are a third of their 640x480 size. The 3D view is
  drawn at the window's own size whatever the game's resolution, so game_resolution = 960x540 with window_scale = 2
  gives a 1920x1080 window with the 2D doubled and the 3D at full resolution. New: the 2D frame is drawn with nearest
  filtering at whole multiples (ui_filter = auto | linear | nearest: sharp text), and window_scale steps down when the
  window wouldn't fit on the screen (a resolution picked in the game's menu that is already the screen's).
