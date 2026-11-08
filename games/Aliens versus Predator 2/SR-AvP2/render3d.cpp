/**
 *
 *  The 3D world on Godot: render.c's RenderScene/CreateContext/textures (d3d.ren's job) for the Godot host.
 *
 *  The engine's data is read in place (32-bit game memory; layouts from AVP2-Reconstructed's headers and the
 *  LithTech 2 SDK): a bound world (MainWorld) is turned into Godot meshes once - its main BSP's polygons
 *  grouped by texture - and each RenderScene sets the camera. Textures come from the engine's GetTexture
 *  callback (DTX data) and become Godot textures.
 *
 *  LithTech is left-handed (x right, y up, z forward), Godot right-handed: z is negated.
 *
 *  Done: world geometry with textures, lightmaps (light animation frames, packed into atlas pages) and vertex light
 *  (Gouraud polies: the animations' per-vertex colours), D3D's arithmetic (sRGB values, lightmaps 2x), the camera.
 *  The main world and the terrain are one static mesh; the other BSPs are drawn through their world model objects
 *  (doors, lifts: placed and hidden like the objects). Models (OT_MODEL): skinned on the CPU with the engine's node
 *  transforms (ModelInstance::GetTransforms), lit like d3d.ren (the world's light grid, the sun through ray casts at
 *  the sky, dynamic lights), attachments placed by the engine (RenderStruct::ProcessAttachment), the player's
 *  weapon (FLAG_REALLYCLOSE) in the camera's space in front of everything.
 *  Sprites (camera facing or rotatable quads, glow and depth bias in the vertex shader) and particle systems (one
 *  mesh per system, rebuilt each frame).
 *  The sky (SceneDesc's sky objects seen from the sky box, behind the world, in their order) and fog (the engine's
 *  Fog* console variables: by distance, the sky's by depth from the sky camera).
 *  Dynamic lights on the world (per pixel, d3d.ren's formulas: LMDynamic's added light texture on lightmapped polies,
 *  vertex light on the others).
 *  Changing light animations (flickering lights: single frame, the blend changes) as layers the shader adds.
 *  The 3D picture is drawn in a SubViewport and put under the game's 2D frame through d3d.ren's last passes
 *  (light scale or InvertHack, light add); DrawSky and LMFullBright (vision modes) are followed.
 *  Textures' D3D state changes (SharedTexture::m_pStateChange, switched by the game per vision mode): their fog
 *  (none, all of a colour, a range) and colour op (texture only, add signed) as material uniforms.
 *  Line systems (tracers, beams): lines in the object's space. Poly grids (height grids with a colour table).
 *  Canvases: the game's draw functions get an ILTCustomDraw whose methods are native (x64/canvas-asm.asm).
 *  Detail textures and environment maps on world surfaces (d3d.ren's texel sizes: 2^UIMipmapOffset / mip 0),
 *  sprite-animated surfaces (SURF_SPRITEANIMATE).
 *  Shader preloading as Godot's documentation recommends: a small fixed set of shaders (variants as uniforms) made
 *  when the renderer starts, with a hidden instance of each in the scene (preload_shaders), so that Godot compiles
 *  their pipelines then and not when an effect first appears.
 *  To do: the state changes' blend modes, multi-frame light animations, shadow-map light animations,
 *  vertical fog, model environment maps, model LODs and static lights for models.
 *
 *  Debugging: AVP2_R3D_FULLBRIGHT=1 (textures only), AVP2_R3D_LMSCALE / AVP2_R3D_VCSCALE (light scales),
 *  AVP2_R3D_DUMPLM=<dir> (lightmap pages as PNG), AVP2_R3D_CAMERA="x y z yaw pitch" (a fixed camera),
 *  AVP2_DEBUG=2 (the objects, once), AVP2_R3D_DUMPSTATES=1 (the textures' state changes), AVP2_R3D_DUMPDETAIL=1
 *  (texture headers, detail / environment map links), AVP2_R3D_NOLINKS=1|2|3 (no links / no env maps / no detail).
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <map>
#include <algorithm>
#include <vector>
#include <string>

#include <godot_cpp/godot.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/array.hpp>

#include "platform.h"
#include "config.h"
#include "camera.h"
#include "render3d.h"
#include "game.h"
#include "asmsyms.h"
#include "lowobj.h"

using namespace godot;

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

extern "C" int winapi_debug;
extern "C" int ghost_ready(void);
extern "C" int ghost_aspect_mode(void);
extern "C" void ghost_frame_rect(int game_w, int game_h, int ww, int wh, float r[4]);
enum { HOST_ASPECT_STRETCH, HOST_ASPECT_KEEP, HOST_ASPECT_WIDE };    // host.h
extern "C" int host_is_godot(void);
extern "C" uint32_t call_game(uint32_t func, uint32_t nargs, const uint32_t *args);
extern "C" uint32_t call_game_this(uint32_t func, uint32_t nargs, const uint32_t *args);

/* ------------------------------------------------------------------ */
/* game memory                                                         */

static inline uint32_t g32(uint32_t a) { return *(const uint32_t *)GADDR(a); }
static inline uint16_t g16(uint32_t a) { return *(const uint16_t *)GADDR(a); }
static inline uint8_t g8(uint32_t a) { return *(const uint8_t *)GADDR(a); }
static inline float gf(uint32_t a) { return *(const float *)GADDR(a); }

// LithTech's left-handed coordinates -> Godot's
static inline Vector3 lt_pos(uint32_t a) { return Vector3(gf(a), gf(a + 4), -gf(a + 8)); }

// RenderStruct
#define RS_GETTEXTURE 0x08

// MainWorld
#define MW_WORLDMODELS 0x188        // CMoArray<WorldData*>: +4 array, +8 count
// WorldData
#define WD_ORIGINALBSP 0x04
// WorldBsp
#define WB_WORLDINFOFLAGS 0x48
#define WB_POLIES 0xa0
#define WB_NPOLIES 0xa4
#define WB_NPOINTS 0xac
#define WIF_MAINWORLD (1 << 2)
#define WIF_TERRAIN (1 << 3)
#define WIF_VISBSP (1 << 5)
// WorldPoly
#define WP_SURFACE 0x2c
#define WP_PVERTICES 0x50
#define WP_NVERTICES 0x54
#define WP_NEXTRAVERTICES 0x56
#define WP_VERTICES 0x58
// SPolyVertex (0x18 bytes)
#define PV_VEC 0x00
#define PV_U 0x04
#define PV_V 0x08
#define PV_COLOR 0x14
#define PV_SIZE 0x18
// Surface
#define SF_TEXTURE 0x2c
#define SF_FLAGS 0x30
// SURF_ (ltbasedefs.h)
#define SURF_NONEXISTANT (1 << 1)
#define SURF_INVISIBLE (1 << 2)
#define SURF_TRANSPARENT (1 << 3)
#define SURF_SKY (1 << 4)
#define SURF_BRIGHT (1 << 5)
#define SURF_LIGHTMAP (1 << 7)
#define SURF_ADDITIVE (1 << 19)
#define SURF_SPRITEANIMATE (1 << 14)    // the engine changes the surface's texture to its sprite's current frame
// TextureData
#define TD_BASEWIDTH 0x10           // m_Header.m_BaseWidth, m_BaseHeight
#define TD_BPP 0x22                 // m_Header.m_Extra[2]
#define TD_MIP0 0xcc                // TextureMipData: width, height, data, pitch
#define TD_UIMIPOFFSET 0x24         // m_Header.m_Extra[4]
#define TD_DETAILSCALE 0x26         // m_Header.m_Extra[6..9]
#define TD_DETAILANGLE 0x2a         // m_Header.m_Extra[10..11]
// SharedTexture: the texture the engine links to it (r_LoadSystemTexture, from the DTX command string): a detail
// texture (type 0), an environment map (1) or an environment map by the texture's alpha (2)
#define ST_LINKED 0x28
#define ST_LINKTYPE 0x2c
// SceneDesc
#define SD_RECT 0xbc
#define SD_XFOV 0xcc
#define SD_YFOV 0xd0
#define SD_FARZ 0xd4
#define SD_POS 0xd8
#define SD_ROTATION 0xe4

enum { BPP_8P = 0, BPP_8, BPP_16, BPP_32, BPP_S3TC_DXT1, BPP_S3TC_DXT3, BPP_S3TC_DXT5, BPP_32P };

/* ------------------------------------------------------------------ */
/* state                                                               */

static uint8_t *render_struct;

// worlds the engine has bound (CreateContext), built when first drawn
struct world_context
{
    uint32_t magic;
    uint32_t world;             // MainWorld*
    int built;
};
#define CONTEXT_MAGIC 0x58544352    // "RCTX"

// a texture's D3D state change (SharedTexture::m_pStateChange, set by the game per vision mode), as far as it's done
struct texture_state
{
    int blend;                  // a blend of its own: 0 none, 1 INVSRCCOLOR x2 (MAT_BLEND + st_blend), 2 ONE ONE (MAT_ADD), 3 alpha (MAT_BLEND)
    int colorop;                // 0: texture x light (MODULATE), 1: the texture only (SELECTARG1), 2: texture + light - 0.5 (ADDSIGNED)
    int fog;                    // 0: the scene's, 1: none, 2: all of the fog colour (FOGSTART = FOGEND), 3: its own range
    float fog_color[3], fog_start, fog_end;
};

struct texture_entry
{
    Ref<ImageTexture> texture;  // null: none (missing or unsupported)
    int has_alpha;
    float inv_w, inv_h;         // 1 / the DTX's base size (sprite and poly grid sizes)
    float uv_w, uv_h;           // d3d.ren's texel size (RTexture +4/+8): 2^UIMipmapOffset / mip 0's size - world,
                                // world model and sky UVs and detail coordinates are in these texels
    float detail_scale = 1.0f;  // the DTX header's detail texture scale and angle (radians)
    float detail_angle = 0.0f;
    uint32_t state_ptr = 0;     // the StateChange* the materials have
    texture_state state = { 0, 0, 0, { 0, 0, 0 }, 0, 0 };
    std::vector<Ref<ShaderMaterial>> materials;     // the materials with this texture (their state uniforms follow)
};

// a world model object's mesh instance
struct wm_instance
{
    MeshInstance3D *mi = NULL;
    uint32_t bsp = 0;           // its BSP (the mesh it has)
    int chromakey = -1;         // the surface overrides are for FLAG2_CHROMAKEY (1) or not (0)
    uint32_t frame = 0;         // the last frame it was drawn in
    uint8_t alpha = 255;
    int shown = 0;
};

/* ------------------------------------------------------------------ */
/* dynamic meshes                                                      */

// Models, particle and line systems, poly grids and canvases change every frame. Adding a surface allocates its GPU
// buffers and sets up every instance of the mesh again, so a surface is rebuilt only when its layout changes (its
// arrays, vertex or index count, indices, primitive or material); otherwise its vertices are written into the
// buffers it has (mesh_surface_update_vertex_region / _attribute_region, the Godot documentation's way for meshes that
// change every frame; the AABB isn't updated by that: the callers set the instance's). The data in Godot's layout
// (the RenderingServer's offsets and strides: positions as floats, colours RGBA8, UVs floats) - checked against
// Godot's own encoding once for each combination of arrays; where it differs, the surfaces are rebuilt as before.
struct dyn_arrays
{
    PackedVector3Array v;
    PackedVector2Array uv, uv2;
    PackedColorArray col;
    PackedInt32Array ind;
};

enum { DA_UV = 1, DA_UV2 = 2, DA_COLOR = 4, DA_INDEX = 8, DA_LINES = 16 };

struct dyn_surface
{
    int mask;                   // DA_*
    int64_t count, index_count;
    uint64_t index_hash;
    RID material;
    int64_t format;             // Godot's (ArrayMesh::surface_get_format)
};

struct dyn_part
{
    const dyn_arrays *a;
    Ref<Material> material;
};


// a model object's mesh, rebuilt when its pose or light changes
struct model_instance
{
    MeshInstance3D *mi = NULL;
    Ref<ArrayMesh> mesh;
    uint64_t key = 0;           // hash of what the mesh was built from
    std::vector<dyn_surface> dyn;   // its surfaces' layout (dyn_update)
    uint32_t frame = 0;
    int shown = 0;
    int close = 0;              // parented to the camera
    float sun_pos[3] = { 0, 0, 0 };
    float sun = -1.0f;          // how much of the sun reaches it (< 0: not known yet)
};

// a sprite object's quad
struct sprite_instance
{
    MeshInstance3D *mi = NULL;
    Ref<ArrayMesh> mesh;        // particle and line systems, poly grids, canvases
    std::vector<dyn_surface> dyn;   // its surfaces' layout (dyn_update)
    std::map<uint64_t, uint32_t> rooms;     // canvases: each batch's room (triangles)
    uint32_t tex = 0;
    int variant = -1;
    uint32_t frame = 0;
    int shown = 0;
    int close = 0;
};

struct godot_scene
{
    SubViewport *viewport;      // the 3D picture
    CanvasLayer *layer;
    TextureRect *picture;       // ... on the window, through the post passes
    Ref<ShaderMaterial> post;
    Node3D *root;
    Camera3D *camera;
    world_context *current;
    std::vector<MeshInstance3D *> world_meshes;     // the static world
    std::map<uint32_t, Ref<ArrayMesh>> bsp_meshes;  // world models' BSPs, by WorldBsp*
    std::map<uint32_t, std::vector<uint64_t>> bsp_keys;    // their surfaces' material keys (texture, kind, page)
    Node3D *sky_root;                               // the sky's objects, moved with the camera
    std::map<uint32_t, wm_instance> sky_instances;  // by object
    std::map<uint32_t, wm_instance> wm_instances;   // by object (LTObject*)
    std::map<uint32_t, model_instance> models;        // by object
    std::map<uint32_t, sprite_instance> sprites;      // by object
    std::map<uint32_t, sprite_instance> particles;    // by object
    std::map<uint32_t, sprite_instance> lines;        // line systems, by object
    std::map<uint32_t, sprite_instance> polygrids;    // by object
    std::map<uint32_t, sprite_instance> canvases;     // by object
    std::map<uint64_t, Ref<ShaderMaterial>> sprite_materials;  // by texture and shader variant
    std::map<uint32_t, texture_entry> textures;     // by SharedTexture*
    uint32_t frame;
    int shown;
};
static godot_scene *gs;

static void fog_init(void);

static void preload_shaders(void);
static void update_viewport_mode(void);

// SR-AvP2.cfg's 3D settings: MSAA, the resolution scale and upscaler, anisotropic filtering
static void viewport_settings(SubViewport *vp)
{
    int msaa = config_get_int("msaa", 0);
    vp->set_msaa_3d((msaa >= 8) ? Viewport::MSAA_8X : ((msaa >= 4) ? Viewport::MSAA_4X : ((msaa >= 2) ? Viewport::MSAA_2X : Viewport::MSAA_DISABLED)));
    const char *scale = config_get("render_scale");
    float f = (scale != NULL) ? (float)atof(scale) : 1.0f;
    vp->set_scaling_3d_scale((f < 0.25f) ? 0.25f : ((f > 2.0f) ? 2.0f : f));
    static const struct { const char *name; Viewport::Scaling3DMode mode; } upscalers[] = {
        { "bilinear", Viewport::SCALING_3D_MODE_BILINEAR }, { "fsr", Viewport::SCALING_3D_MODE_FSR }, { "fsr2", Viewport::SCALING_3D_MODE_FSR2 },
        { "nearest", Viewport::SCALING_3D_MODE_NEAREST }, { "metalfx_spatial", Viewport::SCALING_3D_MODE_METALFX_SPATIAL },
        { "metalfx_temporal", Viewport::SCALING_3D_MODE_METALFX_TEMPORAL } };
    const char *up = config_get("upscaler");
    for (const auto &u : upscalers)
        if ((up != NULL) && (strcasecmp(up, u.name) == 0)) vp->set_scaling_3d_mode(u.mode);
    int aniso = config_get_int("anisotropy", 4);
    vp->set_anisotropic_filtering_level((aniso >= 16) ? Viewport::ANISOTROPY_16X : ((aniso >= 8) ? Viewport::ANISOTROPY_8X :
                                        ((aniso >= 4) ? Viewport::ANISOTROPY_4X : ((aniso >= 2) ? Viewport::ANISOTROPY_2X : Viewport::ANISOTROPY_DISABLED))));
    if (winapi_debug)
        eprintf("render3d: 3D view: msaa %d, render scale %.2f (%s), anisotropy %d\n", msaa, (f < 0.25f) ? 0.25f : ((f > 2.0f) ? 2.0f : f), (up != NULL) ? up : "bilinear", aniso);
}

static int ready(void)
{
    if (!host_is_godot() || !ghost_ready()) return 0;
    if (gs == NULL)
    {
        gs = new godot_scene();
        gs->current = NULL;
        gs->frame = 0;
        gs->shown = 0;
        SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
        // the 3D picture is drawn in a viewport of its own and put on the window (under the game's 2D frame) through
        // the scene's last passes: d3d.ren's light scale (or the inverted picture, InvertHack) and light add
        gs->viewport = memnew(SubViewport);
        gs->viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
        gs->viewport->set_size(tree->get_root()->get_size());
        viewport_settings(gs->viewport);
        tree->get_root()->add_child(gs->viewport);
        gs->layer = memnew(CanvasLayer);
        gs->layer->set_layer(-1);
        tree->get_root()->add_child(gs->layer);
        gs->picture = memnew(TextureRect);
        gs->picture->set_texture(gs->viewport->get_texture());
        gs->picture->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        gs->picture->set_stretch_mode(TextureRect::STRETCH_SCALE);
        gs->picture->set_position(Vector2(0, 0));
        gs->picture->set_size(tree->get_root()->get_size());
        Ref<Shader> post;
        post.instantiate();
        post->set_code("shader_type canvas_item;\n"
                       "uniform vec3 scale = vec3(1.0);\n"
                       "uniform vec3 add = vec3(0.0);\n"
                       "uniform float invert = 0.0;\n"
                       "void fragment() {\n"
                       "  vec3 c = texture(TEXTURE, UV).rgb;\n"
                       "  c = (invert > 0.5) ? scale * (vec3(1.0) - c) : min(c * scale, vec3(1.0));\n"
                       "  COLOR = vec4(min(c + add, vec3(1.0)), 1.0);\n"
                       "}\n");
        gs->post.instantiate();
        gs->post->set_shader(post);
        gs->picture->set_material(gs->post);
        gs->layer->add_child(gs->picture);
        gs->picture->set_visible(false);
        gs->root = memnew(Node3D);
        gs->viewport->add_child(gs->root);
        gs->sky_root = memnew(Node3D);
        gs->root->add_child(gs->sky_root);
        gs->camera = memnew(Camera3D);
        gs->root->add_child(gs->camera);
        gs->camera->set_current(true);
        gs->camera->set_keep_aspect_mode(Camera3D::KEEP_HEIGHT);
        RenderingServer::get_singleton()->set_default_clear_color(Color(0, 0, 0));
        fog_init();
        gs->root->set_visible(false);
        preload_shaders();
    }
    return 1;
}

static int r3d_screen_w = 640, r3d_screen_h = 480;

extern "C" void r3d_set_screen_size(int w, int h)
{
    if ((w > 0) && (h > 0)) { r3d_screen_w = w; r3d_screen_h = h; }
}

// the renderer starts (ren_Init): the scene and the shaders (preload_shaders) before the game's first frame
extern "C" void r3d_init(void)
{
    ready();
}

extern "C" void r3d_set_render_struct(uint8_t *rs)
{
    render_struct = rs;
}

/* ------------------------------------------------------------------ */
/* console variables                                                   */

// RenderStruct::GetParameter(name) (an HLTPARAM, 0 if the variable doesn't exist) and GetParameterValueString:
// the value as text (the float one returns in st0, which call_game doesn't give back)
#define RS_GETPARAMETER 0x18
#define RS_GETPARAMETERVALUESTRING 0x20

static float cvar(const char *name, float def)
{
    static char buf_init[64];   // the engine reads it: below 2 GB (LOW_OBJ)
    LOW_SLOT(buf)
    char *buf = LOW_OBJ(buf);
    if (render_struct == NULL) return def;
    strncpy(buf, name, sizeof(buf_init) - 1);
    uint32_t args[1] = { H2G(buf) };
    uint32_t h = call_game(*(uint32_t *)(render_struct + RS_GETPARAMETER), 1, args);
    if (h == 0) return def;
    args[0] = h;
    uint32_t str = call_game(*(uint32_t *)(render_struct + RS_GETPARAMETERVALUESTRING), 1, args);
    return str ? (float)atof((const char *)G2H(str)) : def;
}

/* ------------------------------------------------------------------ */
/* textures                                                            */

static texture_entry make_texture(uint32_t shared)
{
    texture_entry e;
    e.has_alpha = 0;
    e.inv_w = e.inv_h = e.uv_w = e.uv_h = 1.0f / 64.0f;
    if ((shared == 0) || (render_struct == NULL)) return e;

    uint32_t td = call_game(*(uint32_t *)(render_struct + RS_GETTEXTURE), 1, &shared);
    if (td == 0) return e;

    uint32_t base_w = g16(td + TD_BASEWIDTH), base_h = g16(td + TD_BASEWIDTH + 2);
    if (base_w) e.inv_w = 1.0f / (float)base_w;
    if (base_h) e.inv_h = 1.0f / (float)base_h;
    // DtxHeader::GetDetailTextureScale (m_Extra[6..9] + 1) and GetDetailTextureAngle (m_Extra[10..11], degrees)
    float dscale;
    memcpy(&dscale, (const void *)G2H(td + TD_DETAILSCALE), 4);
    if (std::isfinite(dscale) && (dscale > -1.0f) && (dscale < 1000.0f)) e.detail_scale = dscale + 1.0f;
    e.detail_angle = (float)(int16_t)g16(td + TD_DETAILANGLE) * 3.14159265f / 180.0f;
    int bpp = g8(td + TD_BPP);
    if (bpp == 0) bpp = BPP_32;     // DtxHeader::GetBPPIdent
    uint32_t w = g32(td + TD_MIP0), h = g32(td + TD_MIP0 + 4), data = g32(td + TD_MIP0 + 8);
    int32_t pitch = (int32_t)g32(td + TD_MIP0 + 12);
    if ((w == 0) || (h == 0) || (w > 4096) || (h > 4096) || (data == 0)) return e;
    // DtxHeader::GetUIMipmapScale (m_Extra[4]: a 512 texture can be mapped as a 256 or a 128 one)
    uint32_t ui_offset = g8(td + TD_UIMIPOFFSET);
    float ui_scale = (ui_offset < 8) ? (float)(1u << ui_offset) : 1.0f;
    e.uv_w = ui_scale / (float)w;
    e.uv_h = ui_scale / (float)h;
    if (getenv("AVP2_R3D_DUMPDETAIL"))
        eprintf("render3d: texture %08x: base %ux%u mip0 %ux%u ui mip offset %u scale %.2f angle %d cmd \"%.60s\"\n", shared, base_w, base_h, w, h, g8(td + 0x24), e.detail_scale, (int)(int16_t)g16(td + TD_DETAILANGLE), (const char *)G2H(td + 0x2c));

    Ref<Image> img;
    PackedByteArray px;
    if (bpp == BPP_32)
    {
        px.resize((int64_t)w * h * 4);
        uint8_t *d = px.ptrw();
        for (uint32_t y = 0; y < h; y++)
        {
            const uint8_t *s = (const uint8_t *)G2H(data + y * pitch);
            for (uint32_t x = 0; x < w; x++)
            {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                if (s[3] != 255) e.has_alpha = 1;
                d += 4; s += 4;
            }
        }
        img = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, px);
    }
    else if ((bpp == BPP_S3TC_DXT1) || (bpp == BPP_S3TC_DXT3) || (bpp == BPP_S3TC_DXT5))
    {
        uint32_t block = (bpp == BPP_S3TC_DXT1) ? 8 : 16;
        uint32_t size = ((w + 3) / 4) * ((h + 3) / 4) * block;
        px.resize(size);
        memcpy(px.ptrw(), (const void *)G2H(data), size);
        img = Image::create_from_data(w, h, false, (bpp == BPP_S3TC_DXT1) ? Image::FORMAT_DXT1 : (bpp == BPP_S3TC_DXT3) ? Image::FORMAT_DXT3 : Image::FORMAT_DXT5, px);
        if (img.is_valid()) img->decompress();
        e.has_alpha = (bpp != BPP_S3TC_DXT1);
        if (img.is_valid() && img->detect_alpha() == Image::ALPHA_NONE) e.has_alpha = 0;
        else if (img.is_valid()) e.has_alpha = 1;
    }
    else if (bpp == BPP_16)
    {
        // 16-bit DTX: 4444 ARGB
        px.resize((int64_t)w * h * 4);
        uint8_t *d = px.ptrw();
        for (uint32_t y = 0; y < h; y++)
        {
            const uint16_t *s = (const uint16_t *)G2H(data + y * pitch);
            for (uint32_t x = 0; x < w; x++)
            {
                uint16_t v = s[x];
                d[0] = (uint8_t)(((v >> 8) & 15) * 17);
                d[1] = (uint8_t)(((v >> 4) & 15) * 17);
                d[2] = (uint8_t)((v & 15) * 17);
                d[3] = (uint8_t)(((v >> 12) & 15) * 17);
                if (d[3] != 255) e.has_alpha = 1;
                d += 4;
            }
        }
        img = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, px);
    }
    else
    {
        static int reported[8];
        if ((bpp < 8) && !reported[bpp]) { reported[bpp] = 1; eprintf("render3d: texture format %d not supported yet\n", bpp); }
        return e;
    }
    if (img.is_null() || img->is_empty()) return e;
    img->generate_mipmaps();
    e.texture = ImageTexture::create_from_image(img);
    return e;
}

static texture_entry &get_texture(uint32_t shared)
{
    auto it = gs->textures.find(shared);
    if (it != gs->textures.end()) return it->second;
    return gs->textures[shared] = make_texture(shared);
}

extern "C" void r3d_unbind_texture(void *texture)
{
    if (gs == NULL) return;
    // rebuilt when it's used again (materials keep the old one until then)
    gs->textures.erase(H2G(texture));
}

/* ------------------------------------------------------------------ */
/* materials                                                           */

// texture * light: the lightmap (UV2) for lightmapped polies, else the vertex colour (Gouraud); the colours are
// the game's (sRGB) values, multiplied in linear space (= in sRGB up to the curve's shape)
enum { MAT_OPAQUE = 0, MAT_ALPHA_TEST = 1, MAT_BLEND = 2, MAT_ADD = 3 };
// "really close" objects (the player's weapon): their depth is squeezed in front of everything else
#define MAT_REALLYCLOSE 4
// the sky's objects: at the far plane (behind everything), no depth writes, drawn in their order (render priority)
#define MAT_SKY 8
#define MAT_KINDS 16

// the fog (global shader parameters, set each frame from the console variables FogEnable, FogNearZ, FogFarZ,
// FogR/G/B, SkyFogNearZ, SkyFogFarZ)
#define STATE_UNIFORMS "uniform float st_colorop = 0.0;\nuniform vec4 st_fog = vec4(0.0);\nuniform vec3 st_fog_color = vec3(0.0);\nuniform float st_blend = 0.0;\n"
// (vertical fog: d3d.ren's d3d_CalcVerticalFogAlpha with the VFog console variables, by pixel - lt_vfog: on, VFogMinY,
// VFogMaxY, VFogMinYVal; lt_vfog2: VFogMaxYVal, VFogMax, 255 / VFogDensity. The density is VFogMinYVal below VFogMinY,
// VFogMaxYVal above VFogMaxY, linear between; integrated along the ray from the eye, a crossing point at VFogMaxY when
// they are in different zones (as the original, also for the lower zone); the result 0-255 clamped to VFogMax)
#define VFOG_GLSL "global uniform vec4 lt_vfog;\nglobal uniform vec4 lt_vfog2;\n" \
    "float lt_vfog_density(float y) { return (y <= lt_vfog.y) ? lt_vfog.w : ((y >= lt_vfog.z) ? lt_vfog2.x : (y - lt_vfog.y) * (lt_vfog2.x - lt_vfog.w) / (lt_vfog.z - lt_vfog.y) + lt_vfog.w); }\n" \
    "int lt_vfog_zone(float y) { return (y >= lt_vfog.z) ? 1 : ((y <= lt_vfog.y) ? 0 : 2); }\n" \
    "float lt_vfog_amount(vec3 eye, vec3 p) {\n" \
    "  int ze = lt_vfog_zone(eye.y), zp = lt_vfog_zone(p.y);\n" \
    "  float s = lt_vfog2.z, f;\n" \
    "  if (ze == zp) f = s * distance(eye, p) * ((ze == 2) ? (lt_vfog_density(p.y) + lt_vfog_density(eye.y)) : (lt_vfog_density(eye.y) * 2.0));\n" \
    "  else {\n" \
    "    float t = (lt_vfog.z - eye.y) / (p.y - eye.y);\n" \
    "    vec3 c = vec3((p.x - eye.x) * t + eye.x, lt_vfog.z, (p.z - eye.z) * t + eye.z);\n" \
    "    if (ze == 1) f = s * distance(eye, c) * (lt_vfog2.x * 2.0) + s * distance(c, p) * (lt_vfog_density(p.y) + lt_vfog2.x);\n" \
    "    else f = s * distance(eye, c) * (lt_vfog_density(eye.y) + lt_vfog2.x) + s * distance(c, p) * (lt_vfog2.x * 2.0);\n" \
    "  }\n" \
    "  return clamp(f, 0.0, lt_vfog2.y) / 255.0;\n" \
    "}\n"
#define FOG_UNIFORMS "global uniform vec4 lt_fog;\nglobal uniform vec4 lt_fog_color;\nglobal uniform vec4 lt_sky_fog;\nglobal uniform vec4 lt_misc;\n" VFOG_GLSL
// the dynamic lights that light the world: a 32 x 2 float texture (position (Godot), radius; colour 0-255), count
#define DL_MAX 32
#define DL_UNIFORMS "global uniform sampler2D lt_lights;\nglobal uniform vec4 lt_light_count;\nglobal uniform sampler2D lt_anims;\n"

static Ref<Image> dl_image;
static Ref<ImageTexture> dl_texture;
// the light animations' state (update_anims)
#define ANIM_TEX_W 1024
#define ANIM_TEX_H 4
static Ref<Image> anim_image;
static Ref<ImageTexture> anim_texture;


static void fog_init(void)
{
    RenderingServer *rs = RenderingServer::get_singleton();
    rs->global_shader_parameter_add("lt_fog", RenderingServer::GLOBAL_VAR_TYPE_VEC4, Vector4(0, 0, 2000, 0));
    rs->global_shader_parameter_add("lt_fog_color", RenderingServer::GLOBAL_VAR_TYPE_VEC4, Vector4(1, 1, 1, 1));
    rs->global_shader_parameter_add("lt_sky_fog", RenderingServer::GLOBAL_VAR_TYPE_VEC4, Vector4(0, 2000, 0, 0));
    rs->global_shader_parameter_add("lt_misc", RenderingServer::GLOBAL_VAR_TYPE_VEC4, Vector4(0, 0, 0, 0));     // LMFullBright
    rs->global_shader_parameter_add("lt_vfog", RenderingServer::GLOBAL_VAR_TYPE_VEC4, Vector4(0, 0, 1300, 0.5f));  // vertical fog
    rs->global_shader_parameter_add("lt_vfog2", RenderingServer::GLOBAL_VAR_TYPE_VEC4, Vector4(0, 180, 255.0f / 1800.0f, 0));
    rs->global_shader_parameter_add("lt_scales", RenderingServer::GLOBAL_VAR_TYPE_VEC4, Vector4(2, 2, 0, 0));    // light scales
    dl_image = Image::create_empty(DL_MAX, 2, false, Image::FORMAT_RGBAF);
    dl_texture = ImageTexture::create_from_image(dl_image);
    rs->global_shader_parameter_add("lt_lights", RenderingServer::GLOBAL_VAR_TYPE_SAMPLER2D, dl_texture);
    rs->global_shader_parameter_add("lt_light_count", RenderingServer::GLOBAL_VAR_TYPE_VEC4, Vector4(0, 0, 0, 0));
    anim_image = Image::create_empty(ANIM_TEX_W, ANIM_TEX_H, false, Image::FORMAT_RGBAF);
    anim_texture = ImageTexture::create_from_image(anim_image);
    rs->global_shader_parameter_add("lt_anims", RenderingServer::GLOBAL_VAR_TYPE_SAMPLER2D, anim_texture);
}

static void fog_update(void)
{
    static int n;
    if ((n++ % 15) != 0) return;
    RenderingServer *rs = RenderingServer::get_singleton();
    float on = (getenv("AVP2_R3D_NOFOG") == NULL) && (cvar("FogEnable", 0) != 0.0f);
    rs->global_shader_parameter_set("lt_fog", Vector4(on ? 1.0f : 0.0f, cvar("FogNearZ", 0), cvar("FogFarZ", 2000), 0));
    rs->global_shader_parameter_set("lt_fog_color", Vector4(cvar("FogR", 255) / 255.0f, cvar("FogG", 255) / 255.0f, cvar("FogB", 255) / 255.0f, 1));
    rs->global_shader_parameter_set("lt_sky_fog", Vector4(cvar("SkyFogNearZ", 0), cvar("SkyFogFarZ", 2000), 0, 0));
    // vertical fog (VFog: instead of the distance fog; the defaults are d3d.ren's)
    float vmin = cvar("VFogMinY", 0), vmax = cvar("VFogMaxY", 1300), vd = cvar("VFogDensity", 1800);
    int vfog = (cvar("VFog", 0) != 0.0f) && (vmax > vmin) && (vd > 0.0f);
    rs->global_shader_parameter_set("lt_vfog", Vector4(vfog ? 1.0f : 0.0f, vmin, vmax, cvar("VFogMinYVal", 0.5f)));
    rs->global_shader_parameter_set("lt_vfog2", Vector4(cvar("VFogMaxYVal", 0), cvar("VFogMax", 180), (vd > 0.0f) ? 255.0f / vd : 0.0f, 0));
    static int logged_vfog = -1;
    if (winapi_debug && (vfog != logged_vfog))
    {
        logged_vfog = vfog;
        eprintf("render3d: vertical fog %s (y %.0f - %.0f, density %.2f - %.2f, max %.0f, scale %.0f)\n", vfog ? "on" : "off", vmin, vmax,
                cvar("VFogMinYVal", 0.5f), cvar("VFogMaxYVal", 0), cvar("VFogMax", 180), vd);
    }
    // the light scales (shader_code): lightmaps 2x, vertex colours 2x with Saturate
    float lm = getenv("AVP2_R3D_LMSCALE") ? (float)atof(getenv("AVP2_R3D_LMSCALE")) : 2.0f;
    float vc = getenv("AVP2_R3D_VCSCALE") ? (float)atof(getenv("AVP2_R3D_VCSCALE")) : ((cvar("Saturate", 0) != 0.0f) ? 2.0f : 1.0f);
    rs->global_shader_parameter_set("lt_scales", Vector4(lm, vc, 0, 0));
}

// the scene's last passes (d3d_DrawLightScalePoly with InvertHack, d3d_DrawLightAddPoly), DrawSky, LMFullBright
static int cv_drawsky = 1, cv_invert = 0, cv_fullbright = 0;

static void post_update(uint32_t sd)
{
    static int n;
    if ((n++ % 15) == 0)
    {
        cv_drawsky = cvar("DrawSky", 1) != 0.0f;
        cv_invert = cvar("InvertHack", 0) != 0.0f;
        int fb = cvar("LMFullBright", 0) != 0.0f;
        if (fb != cv_fullbright)
        {
            cv_fullbright = fb;
            RenderingServer::get_singleton()->global_shader_parameter_set("lt_misc", Vector4((float)fb, 0, 0, 0));
        }
    }
    // the light scale: the picture times the scale (DESTCOLOR x SRCCOLOR with the scale x 127.5), or the inverted
    // picture times it (INVDESTCOLOR with the scale x 255)
    Vector3 scale(gf(sd + 0x50), gf(sd + 0x54), gf(sd + 0x58)), add(gf(sd + 0x74), gf(sd + 0x78), gf(sd + 0x7c));
    for (int i = 0; i < 3; i++)
    {
        float f = cv_invert ? scale[i] * 255.0f : scale[i] * 127.5f;
        f = (f < 0.0f) ? 0.0f : ((f > 255.0f) ? 255.0f : floorf(f));
        scale[i] = cv_invert ? f / 255.0f : 2.0f * f / 255.0f;
        if ((gf(sd + 0x50 + 4 * i) > 0.99f) && (gf(sd + 0x50 + 4 * i) < 1.01f) && !cv_invert) scale[i] = 1.0f;
        add[i] = (add[i] < 0.001f) ? 0.0f : floorf(fminf(add[i], 1.0f) * 255.0f) / 255.0f;
    }
    // (the parameter names as StringNames made once: a name from a string is looked up in Godot's table every time)
    static const StringName p_scale("scale"), p_add("add"), p_invert("invert");
    gs->post->set_shader_parameter(p_scale, scale);
    gs->post->set_shader_parameter(p_add, add);
    gs->post->set_shader_parameter(p_invert, cv_invert ? 1.0f : 0.0f);
    // the picture: the scene rectangle (SceneDesc m_Rect, in the game's screen pixels; the 2D frame is stretched over
    // the window), with d3d.ren's projection - FovX across the rectangle's width and FovY across its height,
    // independently: the camera keeps FovY (KEEP_HEIGHT) and the viewport's aspect is tan(FovX/2) / tan(FovY/2), its
    // picture stretched into the rectangle
    // The game's screen is the 2D frame's rectangle in the window (ghost_frame_rect: the whole window, or centred
    // with the game's aspect). Wide (aspect = wide, the default): a scene as wide as the game's screen spans the
    // window's width - Hor+, the same vertical field of view and more to the sides (the tangent grows with the width);
    // its rows stay the scene's (a cutscene's bars span the window)
    SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
    Vector2i win = tree->get_root()->get_size();
    int l = (int)g32(sd + SD_RECT), t = (int)g32(sd + SD_RECT + 4), r = (int)g32(sd + SD_RECT + 8), b = (int)g32(sd + SD_RECT + 12);
    if ((r <= l) || (b <= t) || (l < 0) || (t < 0) || (r > r3d_screen_w) || (b > r3d_screen_h)) { l = 0; t = 0; r = r3d_screen_w; b = r3d_screen_h; }
    float fr[4];
    ghost_frame_rect(r3d_screen_w, r3d_screen_h, win.x, win.y, fr);
    float sx = fr[2] / (float)r3d_screen_w, sy = fr[3] / (float)r3d_screen_h;
    Vector2 pos(floorf(fr[0] + l * sx + 0.5f), floorf(fr[1] + t * sy + 0.5f)), end(floorf(fr[0] + r * sx + 0.5f), floorf(fr[1] + b * sy + 0.5f));
    float fx = gf(sd + SD_XFOV), fy = gf(sd + SD_YFOV);
    float ratio = ((fx > 0.01f) && (fx < 3.1f) && (fy > 0.01f) && (fy < 3.1f)) ? tanf(fx * 0.5f) / tanf(fy * 0.5f) : (end.x - pos.x) / fmaxf(end.y - pos.y, 1.0f);
    if ((ghost_aspect_mode() == HOST_ASPECT_WIDE) && (l == 0) && (r == r3d_screen_w) && (fr[2] + 1.0f < (float)win.x))
    {
        ratio *= (float)win.x / fmaxf(end.x - pos.x, 1.0f);
        pos.x = 0.0f;
        end.x = (float)win.x;
    }
    Vector2i vsize((int)fmaxf(1.0f, floorf((end.y - pos.y) * ratio + 0.5f)), (int)fmaxf(1.0f, end.y - pos.y));
    if (gs->viewport->get_size() != vsize) gs->viewport->set_size(vsize);
    if ((gs->picture->get_position() != pos) || (gs->picture->get_size() != end - pos))
    {
        gs->picture->set_position(pos);
        gs->picture->set_size(end - pos);
    }
    gs->sky_root->set_visible(cv_drawsky != 0);
}

// a shader from its code. Godot parses it here and compiles it for the GPU in its worker threads; the first draw
// with it waits for that (and for its pipelines): AVP2_DEBUG logs each shader and the parse time
static int shaders_preloaded;

static Ref<Shader> make_shader(const char *what, int variant, const std::string &code)
{
    uint64_t t0 = Time::get_singleton()->get_ticks_usec();
    Ref<Shader> sh;
    sh.instantiate();
    sh->set_code(code.c_str());
    if (winapi_debug) eprintf("render3d: shader %s %d (%.1f ms)%s\n", what, variant, (Time::get_singleton()->get_ticks_usec() - t0) / 1000.0, shaders_preloaded ? " late" : "");
    return sh;
}

static const char *shader_code(int kind)
{
    static std::string code[MAT_KINDS];
    if (code[kind].empty())
    {
        int blend = kind & 3;
        std::string modes = "unshaded, cull_disabled";
        if (blend == MAT_ADD) modes += ", blend_add";
        if (kind & MAT_SKY) modes += ", depth_draw_never";
        // D3D's arithmetic: texture * light in the stored (sRGB) values, the lightmap 2x (MODULATE2X, or the
        // multipass blend below); the vertex colour 2x with the Saturate console variable (autoexec.cfg has 1):
        // d3d.ren draws the light (lightmap or gouraud colour) first and the texture over it with SRCBLEND
        // DESTCOLOR + DESTBLEND SRCCOLOR = 2 x texture x light (ZERO + SRCCOLOR = 1x without Saturate). m1s1's
        // vertex lit ground matches the Wine reference with 2x. The result is converted for Godot (linear).
        // The scales are a global uniform (lt_scales: lightmap, vertex colour; fog_update), not in the code: the
        // shaders are made before the console variables are known (preload_shaders).
        std::string c = "shader_type spatial;\nrender_mode " + modes + ";\n"
            "uniform sampler2D albedo : filter_linear_mipmap_anisotropic, repeat_enable;\n"
            "uniform sampler2D lightmap : filter_linear, repeat_disable;\n"
            "uniform float use_lightmap = 0.0;\n"
            "uniform sampler2D detail_tex : filter_linear_mipmap, repeat_enable;\n"
            "uniform float use_detail = 0.0;\nuniform vec4 detail_m = vec4(1.0, 0.0, 0.0, 1.0);\nuniform float detail_add = 1.0;\n"
            "uniform float env_scale = 1.0;\n"
            "global uniform vec4 lt_scales;\n" FOG_UNIFORMS DL_UNIFORMS STATE_UNIFORMS
            "vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n"
            "varying vec4 v_anim;\nvarying vec4 v_lm;\nvarying vec3 v_c0;\nvarying vec3 v_c1;\nvarying float v_vfog;\n";
        c += "void vertex() {\n";
        if (!(kind & MAT_SKY)) c += "  v_anim = CUSTOM0;\n  v_lm = CUSTOM1;\n  v_c0 = CUSTOM2.rgb;\n  v_c1 = CUSTOM3.rgb;\n";
        // the vertical fog by vertex, as d3d.ren (its fog is the vertices' specular alpha)
        if (!(kind & MAT_SKY)) c += "  v_vfog = 0.0;\n  if (lt_vfog.x > 0.5) v_vfog = lt_vfog_amount(CAMERA_POSITION_WORLD, (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz);\n";
        // reverse z (near 1, far 0): the really close objects get [0.9, 1]
        if (kind & MAT_REALLYCLOSE)
            c += "  POSITION = PROJECTION_MATRIX * (MODELVIEW_MATRIX * vec4(VERTEX, 1.0));\n"
                 "  POSITION.z = POSITION.w * (0.9 + 0.1 * clamp(POSITION.z / POSITION.w, 0.0, 1.0));\n";
        // the sky: just in front of the far plane (0), behind any world geometry (near / far >= 1 / 30000)
        if (kind & MAT_SKY)
            c += "  POSITION = PROJECTION_MATRIX * (MODELVIEW_MATRIX * vec4(VERTEX, 1.0));\n"
                 "  POSITION.z = POSITION.w * 1e-7;\n";
        c += "}\n";
        // a detail texture (d3d.ren's second stage: ADDSIGNED, or MODULATE without DetailTextureAdd) on the texture,
        // before the light: d3d.ren draws the light first (the lightmap, or a vertex lit poly's gouraud colour) and
        // queues every textured pass the same way (DrawWorldTexturePoly / the lightmap path: white vertex colour,
        // the 2x blend), so texture + detail - 0.5 is what the light multiplies
        // use_detail 1: a detail texture; 2, 3: an environment map (d3d_CalcEnvMapUV: the view direction reflected
        // by the poly's plane, in LithTech's coordinates; on the texture ADDSIGNED or MODULATE, or by the texture's
        // alpha: MODULATEALPHA_ADDCOLOR)
        c += "void fragment() {\n"
            "  vec4 t = texture(albedo, UV);\n"
            "  vec3 dt = vec3(0.5);\n"
            "  if ((use_detail > 0.5) && (use_detail < 1.5)) dt = texture(detail_tex, vec2(dot(detail_m.xy, UV), dot(detail_m.zw, UV))).rgb;\n"
            "  if ((use_detail > 0.5) && (use_detail < 1.5)) t.rgb = (detail_add > 0.5) ? clamp(t.rgb + dt - 0.5, 0.0, 1.0) : t.rgb * dt;\n"
            "  if (use_detail > 1.5) {\n"
            "    vec3 ewp = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;\n"
            "    vec3 en = normalize(cross(dFdx(ewp), dFdy(ewp))) * vec3(1.0, 1.0, -1.0);\n"
            "    vec3 ei = (ewp - CAMERA_POSITION_WORLD) * vec3(1.0, 1.0, -1.0);\n"
            "    vec3 er = normalize(ei - en * (2.0 * dot(ei, en)));\n"
            "    vec2 euv = vec2(0.5 - ((en.y * en.y > 0.54439 || en.x * en.x < en.z * en.z) ? er.x : er.z) * 0.5, 0.5 - er.y * 0.5) * env_scale;\n"
            "    vec3 env = texture(detail_tex, euv).rgb;\n"
            "    t.rgb = (use_detail > 2.5) ? clamp(t.rgb + t.a * env, 0.0, 1.0) : ((detail_add > 0.5) ? clamp(env + t.rgb - 0.5, 0.0, 1.0) : env * t.rgb);\n"
            "  }\n"
            "  vec3 light;\n";
        if (kind & MAT_SKY)
            c += "  light = (use_lightmap > 0.5) ? texture(lightmap, UV2).rgb * lt_scales.x : COLOR.rgb * lt_scales.y;\n";
        else
            // the changing light animation layers at their current state (lt_anims: blend, on): lightmapped polies'
            // layers are next to the base in the page, the first one that's on is taken as is unless it comes after
            // a base layer, the others are scaled by their blend; vertex lit polies add their layers that are on
            c += "  if (use_lightmap > 0.5) {\n"
                 "    light = texture(lightmap, UV2).rgb;\n"
                 "    bool first = v_lm.z < 0.5;\n"
                 "    int after = int(v_lm.y + 0.5);\n"
                 "    for (int k = 0; k < 4; k++) {\n"
                 "      int a = int(v_anim[k] + 0.5) - 1;\n"
                 "      if (a < 0) break;\n"
                 "      vec4 st = texelFetch(lt_anims, ivec2(a % 1024, a / 1024), 0);\n"
                 "      if (st.y < 0.5) continue;\n"
                 "      float w = (first && ((after >> k) & 1) == 0) ? 1.0 : st.x;\n"
                 "      first = false;\n"
                 "      light += texture(lightmap, UV2 + vec2(v_lm.x * float(k + 1), 0.0)).rgb * w;\n"
                 "    }\n"
                 "    light = (lt_misc.x > 0.5) ? vec3(1.0) : min(light, vec3(1.0)) * lt_scales.x;\n"
                 "  } else {\n"
                 "    light = COLOR.rgb;\n"
                 "    for (int k = 0; k < 2; k++) {\n"
                 "      int a = int(v_anim[k] + 0.5) - 1;\n"
                 "      if (a < 0) break;\n"
                 "      if (texelFetch(lt_anims, ivec2(a % 1024, a / 1024), 0).y > 0.5) light += (k == 0) ? v_c0 : v_c1;\n"
                 "    }\n"
                 "    light = min(light, vec3(1.0)) * lt_scales.y;\n"
                 "  }\n";
        c += "  vec3 add = vec3(0.0);\n";
        // dynamic lights (not on the sky): on lightmapped polies d3d.ren adds a texture of the light (LMDynamic,
        // BuildDynamicLightmapPixels: (max(c, 127) - 127) x (1 - (in-plane distance / 1.25 r)^2 - (plane distance / r)^2),
        // not times the surface's texture); on vertex lit polies the vertex colours get (2c - 255) x (1 - d / r)
        if (!(kind & MAT_SKY))
            c += "  if (lt_light_count.x > 0.5) {\n"
                 "    vec3 wp = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;\n"
                 "    vec3 n = normalize(cross(dFdx(wp), dFdy(wp)));\n"
                 "    for (int i = 0; i < int(lt_light_count.x); i++) {\n"
                 "      vec4 lp = texelFetch(lt_lights, ivec2(i, 0), 0);\n"
                 "      vec3 lc = texelFetch(lt_lights, ivec2(i, 1), 0).rgb;\n"
                 "      vec3 d = lp.xyz - wp;\n"
                 "      float d2 = dot(d, d);\n"
                 "      if (d2 >= lp.w * lp.w * 1.5625) continue;\n"
                 "      if (use_lightmap > 0.5) {\n"
                 "        float pd = dot(d, n);\n"
                 "        float a = 1.0 - (d2 - pd * pd) / (lp.w * lp.w * 1.5625) - (pd * pd) / (lp.w * lp.w);\n"
                 "        if (a > 0.0) add += max(lc - 127.0, vec3(0.0)) / 255.0 * a;\n"
                 "      } else {\n"
                 "        float a = 1.0 - sqrt(d2) / lp.w;\n"
                 "        if (a > 0.0) light += (2.0 * lc - 255.0) / 255.0 * a;\n"
                 "      }\n"
                 "    }\n"
                 "  }\n";
        // the texture's colour op (its state change, for the texture pass): texture x light, the texture alone, or
        // texture + light - 0.5; a lightmap is a pass of its own after it (multipass), so it stays
        c += "  vec3 lit = clamp(t.rgb * clamp(light, 0.0, 2.0) + add, 0.0, 1.0);\n"
             "  vec3 col = (st_colorop > 1.5) ? clamp(t.rgb + clamp(light, 0.0, 2.0) - 0.5 + add, 0.0, 1.0)\n"
             "            : ((st_colorop > 0.5) && (use_lightmap < 0.5)) ? t.rgb : lit;\n";
        if (getenv("AVP2_R3D_FULLBRIGHT")) c += "  col = t.rgb;\n";
        // fog (d3d.ren: by the distance from the viewer, the sky's by its depth from the sky camera; additive
        // surfaces fade to black)
        c += (kind & MAT_SKY) ? "  float f = lt_fog.x * clamp((-VERTEX.z - lt_sky_fog.x) / max(lt_sky_fog.y - lt_sky_fog.x, 0.001), 0.0, 1.0);\n"
                              : "  float f = lt_fog.x * ((lt_vfog.x > 0.5) ? v_vfog : clamp((length(VERTEX) - lt_fog.y) / max(lt_fog.z - lt_fog.y, 0.001), 0.0, 1.0));\n";
        // the texture's own fog: none, all of its colour, or its range (FOGSTART/FOGEND, by depth)
        // (all of the colour: the texture pass is that colour; a lightmap pass still multiplies it)
        c += "  vec3 fc = lt_fog_color.rgb;\n"
             "  if (st_fog.x > 2.5) { f = clamp((-VERTEX.z - st_fog.y) / max(st_fog.z - st_fog.y, 0.001), 0.0, 1.0); fc = st_fog_color; }\n"
             "  else if (st_fog.x > 1.5) { f = 0.0; col = (use_lightmap > 0.5) ? clamp(st_fog_color * clamp(light, 0.0, 2.0) + add, 0.0, 1.0) : st_fog_color; }\n"
             "  else if (st_fog.x > 0.5) f = 0.0;\n";
        c += (blend == MAT_ADD) ? "  col *= 1.0 - f;\n" : "  col = mix(col, fc, f);\n";
        c += "  ALBEDO = to_linear(col);\n";
        if ((kind & MAT_SKY) && (blend == MAT_OPAQUE)) c += "  ALPHA = 1.0;\n";
        if (blend == MAT_ALPHA_TEST) c += "  ALPHA = t.a;\n  ALPHA_SCISSOR_THRESHOLD = 0.5;\n";
        // (a state change's INVSRCCOLOR, INVSRCCOLOR blend - the Alien's Hunting vision, NavAliens: src (1 - src) +
        // dest (1 - src) per channel - as src (1 - src) + dest (1 - its luminance): mix blending can't do per channel)
        if (blend == MAT_BLEND) c += "  ALPHA = t.a * COLOR.a;\n"
                                     "  if (st_blend > 0.5) {\n"
                                     "    float l = clamp(dot(col, vec3(0.299, 0.587, 0.114)), 0.004, 1.0);\n"
                                     "    ALPHA = l;\n"
                                     "    ALBEDO = to_linear(col * (vec3(1.0) - col) / l);\n"
                                     "  }\n";
        if (blend == MAT_ADD) c += "  ALPHA = 1.0;\n";
        c += "}\n";
        code[kind] = c;
    }
    return code[kind].c_str();
}

// the world's shaders (world, world models, models, the sky), by kind: made once (preload_shaders) and kept for the
// whole run (on the heap: never destroyed, Godot is gone before static destructors run)
static Ref<Shader> world_shader(int kind)
{
    static Ref<Shader> *shaders = new Ref<Shader>[MAT_KINDS];
    if (shaders[kind].is_null()) shaders[kind] = make_shader("world", kind, shader_code(kind));
    return shaders[kind];
}

struct lm_page
{
    int size;
    std::vector<uint32_t> px;   // 0x00RRGGBB
    int x, y, row_h;
    Ref<ImageTexture> texture;
};

// a sprite-animated surface's own material (its texture follows the surface's)
struct animated_surface
{
    uint32_t surface;
    uint32_t tex;
    Ref<ShaderMaterial> m;
};

struct world_materials
{
    std::map<uint64_t, Ref<ShaderMaterial>> materials;
    std::map<uint64_t, Ref<ShaderMaterial>> sky_materials;
    std::map<uint64_t, Ref<ShaderMaterial>> surface_materials;     // by batch key (SPRITE_KEY)
    std::vector<animated_surface> animated;
    std::vector<lm_page> pages;
};
static world_materials *wm;

// a batch key with the surface (SURF_SPRITEANIMATE) in place of the texture
#define SPRITE_KEY (1ull << 49)

// D3D7 render and texture stage states (d3dtypes.h)
#define D3DRS_FOGENABLE 28
#define D3DRS_FOGCOLOR 34
#define D3DRS_FOGSTART 36
#define D3DRS_FOGEND 37
#define D3DRS_SRCBLEND 19
#define D3DRS_DESTBLEND 20
#define D3DRS_ALPHABLENDENABLE 27
#define D3DTSS_COLOROP 1
#define D3DTOP_SELECTARG1 2
#define D3DTOP_MODULATE 4
#define D3DTOP_ADDSIGNED 8

// StateChange: two (STLport) vectors - RenderState {type, value} and TextureState {stage, type, value}
static texture_state parse_state(uint32_t sc)
{
    texture_state t = { 0, 0, 0, { 0, 0, 0 }, 0, 0 };
    if (sc == 0) return t;
    int fogenable = -1, have_range = 0, blendenable = -1, src = 0, dst = 0;
    uint32_t color = 0;
    for (uint32_t r = g32(sc), e = g32(sc + 4), n = 0; (r != 0) && (r < e) && (n < 64); r += 8, n++)
    {
        uint32_t type = g32(r), v = g32(r + 4);
        if (type == D3DRS_FOGENABLE) fogenable = (v != 0);
        else if (type == D3DRS_FOGCOLOR) color = v;
        else if (type == D3DRS_FOGSTART) { memcpy(&t.fog_start, &v, 4); have_range = 1; }
        else if (type == D3DRS_FOGEND) { memcpy(&t.fog_end, &v, 4); have_range = 1; }
        else if (type == D3DRS_ALPHABLENDENABLE) blendenable = (v != 0);
        else if (type == D3DRS_SRCBLEND) src = (int)v;
        else if (type == D3DRS_DESTBLEND) dst = (int)v;
    }
    // D3DBLEND: 2 ONE, 4 INVSRCCOLOR, 5 SRCALPHA, 6 INVSRCALPHA
    if (blendenable == 1)
    {
        if ((src == 4) && (dst == 4)) t.blend = 1;
        else if ((src == 2) && (dst == 2)) t.blend = 2;
        else if ((src == 5) && (dst == 6)) t.blend = 3;
    }
    for (uint32_t r = g32(sc + 12), e = g32(sc + 16), n = 0; (r != 0) && (r < e) && (n < 64); r += 12, n++)
    {
        if ((g32(r) != 0) || (g32(r + 4) != D3DTSS_COLOROP)) continue;
        uint32_t v = g32(r + 8);
        t.colorop = (v == D3DTOP_SELECTARG1) ? 1 : ((v == D3DTOP_ADDSIGNED) ? 2 : 0);
    }
    if (fogenable == 0) t.fog = 1;
    else if (fogenable == 1)
    {
        t.fog = (have_range && (t.fog_end > t.fog_start + 0.001f)) ? 3 : 2;
        t.fog_color[0] = ((color >> 16) & 255) / 255.0f;
        t.fog_color[1] = ((color >> 8) & 255) / 255.0f;
        t.fog_color[2] = (color & 255) / 255.0f;
    }
    return t;
}

static void apply_state(const Ref<ShaderMaterial> &m, const texture_state &t)
{
    m->set_shader_parameter("st_colorop", (float)t.colorop);
    m->set_shader_parameter("st_blend", (t.blend == 1) ? 1.0f : 0.0f);
    m->set_shader_parameter("st_fog", Vector4((float)t.fog, t.fog_start, t.fog_end, 0));
    m->set_shader_parameter("st_fog_color", Vector3(t.fog_color[0], t.fog_color[1], t.fog_color[2]));
}

// each frame: the materials follow their textures' state changes (vision modes switch them)
static void update_texture_states(void)
{
    for (auto &kv : gs->textures)
    {
        texture_entry &te = kv.second;
        if ((kv.first == 0) || te.materials.empty()) continue;
        uint32_t sc = g32(kv.first + 0x34);
        if (sc == te.state_ptr) continue;
        te.state_ptr = sc;
        te.state = parse_state(sc);
        for (Ref<ShaderMaterial> &m : te.materials) apply_state(m, te.state);
    }
}

static void track_material(uint32_t shared, const Ref<ShaderMaterial> &m)
{
    texture_entry &te = get_texture(shared);
    if (shared != 0)
    {
        te.state_ptr = g32(shared + 0x34);
        te.state = parse_state(te.state_ptr);
    }
    te.materials.push_back(m);
    apply_state(m, te.state);
}

// the texture's detail texture (d3d_DrawPieceQueue / d3d_DrawDetailPieces): its UVs are the texture's (in the
// texture's texels) over the detail texture's size, times DetailTextureScale and the DTX's own scale, turned by its
// angle
static void set_detail(const Ref<ShaderMaterial> &m, uint32_t shared, const texture_entry &t)
{
    static int dump = -1;
    if (dump < 0) dump = (getenv("AVP2_R3D_DUMPDETAIL") != NULL);
    // debugging: AVP2_R3D_NOLINKS=1 none, 2 no environment maps, 3 no detail textures
    int nolinks = getenv("AVP2_R3D_NOLINKS") ? atoi(getenv("AVP2_R3D_NOLINKS")) : 0;
    if ((shared == 0) || (nolinks == 1)) return;
    uint32_t linked = g32(shared + ST_LINKED), type = g32(shared + ST_LINKTYPE);
    if ((linked == 0) || (type > 2)) return;
    if (((nolinks == 2) && (type != 0)) || ((nolinks == 3) && (type == 0))) return;
    if ((type == 0) && (cvar("DetailTextures", 1) == 0.0f)) return;
    if ((type != 0) && (cvar("EnvMapWorld", 1) == 0.0f)) return;
    texture_entry &d = get_texture(linked);
    if (d.texture.is_null()) return;
    if (type != 0)
    {
        m->set_shader_parameter("detail_tex", d.texture);
        m->set_shader_parameter("env_scale", cvar("DetailTextureScale", 0.2f));
        m->set_shader_parameter("detail_add", (cvar("DetailTextureAdd", 1) != 0.0f) ? 1.0f : 0.0f);
        m->set_shader_parameter("use_detail", (float)(type + 1));
        if (dump) eprintf("render3d: environment map %08x -> %08x (type %u)\n", shared, linked, type);
        return;
    }
    float s = cvar("DetailTextureScale", 0.2f) * t.detail_scale;
    float c = cosf(t.detail_angle), sn = sinf(t.detail_angle);
    float bw = 1.0f / t.uv_w, bh = 1.0f / t.uv_h;
    m->set_shader_parameter("detail_tex", d.texture);
    m->set_shader_parameter("detail_m", Vector4(c * bw * d.uv_w * s, -sn * bh * d.uv_h * s, sn * bw * d.uv_w * s, c * bh * d.uv_h * s));
    m->set_shader_parameter("detail_add", (cvar("DetailTextureAdd", 1) != 0.0f) ? 1.0f : 0.0f);
    m->set_shader_parameter("use_detail", 1.0f);
    if (dump)
    {
        Ref<Image> im = d.texture->get_image();
        Color avg(0, 0, 0, 0);
        if (im.is_valid() && !im->is_empty())
        {
            int n = 0;
            for (int y = 0; y < im->get_height(); y += 8)
                for (int x = 0; x < im->get_width(); x += 8) { avg += im->get_pixel(x, y); n++; }
            if (n) avg = avg / (float)n;
        }
        eprintf("render3d: detail %08x -> %08x: base %.0fx%.0f detail %.0fx%.0f scale %.2f angle %.2f avg %.2f %.2f %.2f m %.3f %.3f %.3f %.3f\n", shared, linked, bw, bh, 1.0f / d.uv_w, 1.0f / d.uv_h, s, t.detail_angle,
                avg.r, avg.g, avg.b, c * bw * d.uv_w * s, -sn * bh * d.uv_h * s, sn * bw * d.uv_w * s, c * bh * d.uv_h * s);
    }
}

// links: the texture's detail texture / environment map (world surfaces; d3d.ren's models do their own)
static Ref<ShaderMaterial> get_material(uint32_t shared, int kind, int page, int links = 1)
{
    if (kind & MAT_SKY) links = 0;
    uint64_t key = ((uint64_t)shared << 16) | ((uint64_t)(page + 1) << 4) | (uint64_t)kind | (links ? 0 : (1ull << 48));
    auto it = wm->materials.find(key);
    if (it != wm->materials.end()) return it->second;

    Ref<ShaderMaterial> m;
    m.instantiate();
    m->set_shader(world_shader(kind));
    texture_entry &t = get_texture(shared);
    if (t.texture.is_valid()) m->set_shader_parameter("albedo", t.texture);
    if ((page >= 0) && (page < (int)wm->pages.size()))
    {
        m->set_shader_parameter("lightmap", wm->pages[page].texture);
        m->set_shader_parameter("use_lightmap", 1.0f);
    }
    if (links) set_detail(m, shared, t);
    wm->materials[key] = m;
    track_material(shared, m);
    return m;
}

// the texture of a batch key (a sprite-animated surface's: its texture now)
static uint32_t key_texture(uint64_t key)
{
    return (key & SPRITE_KEY) ? g32((uint32_t)(key >> 16) + SF_TEXTURE) : (uint32_t)(key >> 16);
}

// a batch's material: the texture's, or a sprite-animated surface's own (update_animated_surfaces)
static Ref<ShaderMaterial> batch_material(uint64_t key)
{
    int kind = (int)(key & 0xf), page = (int)((key >> 4) & 0xfff) - 1;
    if (!(key & SPRITE_KEY)) return get_material((uint32_t)(key >> 16), kind, page);
    auto it = wm->surface_materials.find(key);
    if (it != wm->surface_materials.end()) return it->second;
    uint32_t surface = (uint32_t)(key >> 16), tex = g32(surface + SF_TEXTURE);
    Ref<ShaderMaterial> m = get_material(tex, kind, page)->duplicate();
    wm->surface_materials[key] = m;
    wm->animated.push_back({ surface, tex, m });
    track_material(tex, m);
    return m;
}

// each frame: the sprite-animated surfaces' current frames
static void update_animated_surfaces(void)
{
    if (wm == NULL) return;
    for (animated_surface &a : wm->animated)
    {
        uint32_t t = g32(a.surface + SF_TEXTURE);
        if ((t == a.tex) || (t == 0)) continue;
        a.tex = t;
        texture_entry &te = get_texture(t);
        static const StringName p_albedo("albedo");
        if (te.texture.is_valid()) a.m->set_shader_parameter(p_albedo, te.texture);
    }
}

// a sky object's version of a material (its order among the sky objects in the render priority)
static Ref<ShaderMaterial> sky_material(uint64_t key, int index)
{
    uint64_t k = (key << 8) | (uint64_t)(index & 0xff);
    auto it = wm->sky_materials.find(k);
    if (it != wm->sky_materials.end()) return it->second;
    int kind = (int)(key & 0xf) | MAT_SKY;
    // d3d_DrawSkyObjects draws the surfaces that aren't translucent without blending or alpha test
    if ((kind & 3) == MAT_ALPHA_TEST) kind &= ~3;
    Ref<ShaderMaterial> m = get_material(key_texture(key), kind, (int)((key >> 4) & 0xfff) - 1)->duplicate();
    m->set_render_priority(-120 + ((index < 200) ? index : 200));
    wm->sky_materials[k] = m;
    track_material(key_texture(key), m);
    return m;
}

/* ------------------------------------------------------------------ */
/* lightmaps                                                           */

// MainWorld
#define MW_LIGHTANIMS 0x04          // CMoArray<LightAnim>: +4 array, +8 count
#define MW_LMGRIDSIZE 0xf8
// LightAnim (0x60 bytes)
#define LA_SIZE 0x60
#define LA_FRAMES 0x24              // LAPolyFrame*[frames], each an array per poly
#define LA_NFRAMES 0x28
#define LA_NPOLIES 0x30
#define LA_IFRAMES 0x34
// LAPolyFrame (0x18 bytes)
#define LF_SIZE 0x18
// WorldPoly
#define WP_LMANIMREFS 0x0c          // (light anim, its poly index) pairs of uint16
#define WP_NLMANIMREFS 0x10
#define WP_FLAGS 0x14
#define WP_PLANE 0x28
#define WP_LMORIGIN 0x38
#define WP_LMWIDTH 0x4c
#define WP_LMHEIGHT 0x4d

#define LM_PAGE_SIZE 2048

// a lightmap texel run: a dword colour, one with the top bit set followed by a byte count (d3d_DecompressLM32)
static int decompress_lm(uint32_t in, uint32_t len, uint32_t *out, uint32_t max)
{
    const uint8_t *p = (const uint8_t *)G2H(in), *e = p + len;
    uint32_t n = 0;
    while (p + 4 <= e)
    {
        uint32_t c, count = 1;
        memcpy(&c, p, 4);
        p += 4;
        if (c & 0x80000000u)
        {
            if (p >= e) break;
            count = *p++;
            c &= 0x7fffffffu;
        }
        if (n + count > max) return 0;
        while (count--) out[n++] = c;
    }
    return 1;
}

// places w x h texels (with a one texel border) in a lightmap page: returns the page, (x, y) of the inside
// places n blocks of w x h texels side by side (each with a one texel border) in a lightmap page: returns the
// page, (x, y) of the first block's inside (block k is (w + 2) * k texels to the right)
static int lm_place(const uint32_t *const *blocks, int n, int w, int h, int *ox, int *oy)
{
    int bw = (w + 2) * n, bh = h + 2;
    lm_page *pg = wm->pages.empty() ? NULL : &wm->pages.back();
    if (pg != NULL)
    {
        if (pg->x + bw > pg->size) { pg->x = 0; pg->y += pg->row_h; pg->row_h = 0; }
        if (pg->y + bh > pg->size) pg = NULL;
    }
    if (pg == NULL)
    {
        wm->pages.push_back(lm_page());
        pg = &wm->pages.back();
        pg->size = LM_PAGE_SIZE;
        pg->px.assign((size_t)LM_PAGE_SIZE * LM_PAGE_SIZE, 0);
        pg->x = pg->y = pg->row_h = 0;
    }
    for (int k = 0; k < n; k++)
    {
        const uint32_t *texels = blocks[k];
        for (int y = -1; y <= h; y++)
        {
            int sy = (y < 0) ? 0 : ((y >= h) ? h - 1 : y);
            uint32_t *row = &pg->px[(size_t)(pg->y + 1 + y) * pg->size + pg->x + (w + 2) * k + 1];
            for (int x = -1; x <= w; x++)
            {
                int sx = (x < 0) ? 0 : ((x >= w) ? w - 1 : x);
                row[x] = texels[sy * w + sx];
            }
        }
    }
    *ox = pg->x + 1;
    *oy = pg->y + 1;
    pg->x += bw;
    if (bh > pg->row_h) pg->row_h = bh;
    return (int)wm->pages.size() - 1;
}

static void upload_lm_pages(void)
{
    int index = 0;
    for (lm_page &pg : wm->pages)
    {
        if (getenv("AVP2_R3D_DUMPLM"))
        {
            // debugging: the page as a PNG (and its average)
            uint64_t sum[3] = { 0, 0, 0 };
            for (uint32_t c : pg.px) { sum[0] += (c >> 16) & 255; sum[1] += (c >> 8) & 255; sum[2] += c & 255; }
            eprintf("render3d: lightmap page %d: used to row %d, average (whole page) %.1f %.1f %.1f\n", index, pg.y + pg.row_h,
                    sum[0] / (double)pg.px.size(), sum[1] / (double)pg.px.size(), sum[2] / (double)pg.px.size());
        }
        PackedByteArray b;
        b.resize((int64_t)pg.size * pg.size * 4);
        uint8_t *d = b.ptrw();
        for (size_t i = 0; i < pg.px.size(); i++)
        {
            uint32_t c = pg.px[i];
            d[4 * i + 0] = (uint8_t)(c >> 16);
            d[4 * i + 1] = (uint8_t)(c >> 8);
            d[4 * i + 2] = (uint8_t)c;
            d[4 * i + 3] = 255;
        }
        Ref<Image> img = Image::create_from_data(pg.size, pg.size, false, Image::FORMAT_RGBA8, b);
        if (getenv("AVP2_R3D_DUMPLM"))
        {
            char name[256];
            snprintf(name, sizeof(name), "%s/lmpage%d.png", getenv("AVP2_R3D_DUMPLM"), index);
            img->save_png(name);
        }
        index++;
        pg.texture = ImageTexture::create_from_image(img);
        pg.px.clear();
        pg.px.shrink_to_fit();
    }
}

// lightmap axes (d3d.ren's six bases, picked by the poly's flags): right = normal.Cross(base), up =
// right.Cross(normal), with LithTech's Cross (a.Cross(b) is b x a in the usual notation)
static void cross(const float a[3], const float b[3], float r[3])
{
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

static void lm_axes(uint32_t poly, float right[3], float up[3])
{
    static const float bases[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 } };
    uint32_t plane = g32(poly + WP_PLANE);
    float n[3] = { gf(plane), gf(plane + 4), gf(plane + 8) };
    int i = (g16(poly + WP_FLAGS) >> 11) & 7;
    if (i > 5) i = 0;
    cross(bases[i], n, right);
    cross(n, right, up);
}

// the light animations that touch a poly (its refs), in their current frames (d3d.ren: UpdatePolyAnimatedLightmap,
// d3d_AddLightAnimVertexColors). Animations that are off (frame -1) or below 2 % are left out; shadow-map ones
// (lit from the light's position through a mask) aren't done yet.
#define LA_SHADOWMAP 0x20
#define LA_BLEND 0x40
#define LF_VERTR 0x08
#define LF_VERTG 0x0c
#define LF_VERTB 0x10
#define LF_NVERTS 0x14

static uint32_t anim_frame(uint32_t mw, uint32_t ref, uint32_t *anim_out)
{
    uint32_t anims = g32(mw + MW_LIGHTANIMS + 4), nanims = g32(mw + MW_LIGHTANIMS + 8);
    uint32_t ia = g16(ref), ip = g16(ref + 2);
    if (ia >= nanims) return 0;
    uint32_t anim = anims + ia * LA_SIZE;
    if (ip >= g16(anim + LA_NPOLIES)) return 0;
    uint32_t frame = g32(anim + LA_IFRAMES);
    if ((frame == 0xffffffffu) || (frame >= g8(anim + LA_NFRAMES)) || (gf(anim + LA_BLEND) < 0.02f)) return 0;
    uint32_t frames = g32(anim + LA_FRAMES);
    if (frames == 0) return 0;
    *anim_out = anim;
    return g32(frames + 4 * frame) + ip * LF_SIZE;
}

// Light animations that change while the level runs (every one but the base, LightAnim_BASE, with a single frame;
// m1s1's "Flicker1WaveLightFX" ones: their blend goes up and down) are kept apart: a poly gets up to MAX_LAYERS of
// them as layers that the shader adds at their current state (lt_anims); the others (and the layers past
// MAX_LAYERS) are baked in as they are now.
#define MAX_LAYERS 4
#define MAX_VLAYERS 2

struct poly_layers
{
    int n;                      // layers
    uint32_t anim[MAX_LAYERS];  // their light animations' indices
    int after_base;             // bit k: layer k comes after a base layer that's on (d3d.ren: then it's scaled by its blend)
    int has_base;
};

static int anim_is_dynamic(uint32_t anim)
{
    return (strncmp((const char *)G2H(anim), "LightAnim_BASE", 32) != 0) && !g32(anim + LA_SHADOWMAP) && (g8(anim + LA_NFRAMES) == 1);
}

// a light animation's frame for a poly whatever its state (its current frame, or the first one)
static uint32_t anim_any_frame(uint32_t mw, uint32_t ref, uint32_t *ianim)
{
    uint32_t anims = g32(mw + MW_LIGHTANIMS + 4), nanims = g32(mw + MW_LIGHTANIMS + 8);
    uint32_t ia = g16(ref), ip = g16(ref + 2);
    if (ia >= nanims) return 0;
    uint32_t anim = anims + ia * LA_SIZE;
    if (ip >= g16(anim + LA_NPOLIES)) return 0;
    uint32_t frame = g32(anim + LA_IFRAMES), frames = g32(anim + LA_FRAMES);
    if ((frame == 0xffffffffu) || (frame >= g8(anim + LA_NFRAMES))) frame = 0;
    if (frames == 0) return 0;
    *ianim = ia;
    return g32(frames + 4 * frame) + ip * LF_SIZE;
}

static void lm_add_scaled(uint32_t *texels, const uint32_t *layer, int n, uint32_t scale)
{
    for (int t = 0; t < n; t++)
    {
        uint32_t a = texels[t], b = layer[t], o = 0;
        for (int sh = 0; sh <= 16; sh += 8)
        {
            uint32_t c = ((a >> sh) & 255) + (((b >> sh) & 255) * scale) / 255;
            o |= ((c > 255) ? 255 : c) << sh;
        }
        texels[t] = o;
    }
}

// the poly's lightmap (texels 0x00RRGGBB): the base - its animations as they are now, the first one as is, the
// others added at their intensity (UpdatePolyAnimatedLightmap) - and the changing layers
static int poly_lightmap(uint32_t mw, uint32_t poly, uint32_t *texels, uint32_t (*layers)[0x400], poly_layers *pl, int *w, int *h)
{
    static uint32_t layer[0x400];
    *w = g8(poly + WP_LMWIDTH);
    *h = g8(poly + WP_LMHEIGHT);
    pl->n = 0;
    pl->after_base = 0;
    pl->has_base = 0;
    int n = *w * *h;
    if ((n == 0) || (n > 0x400)) return 0;
    uint32_t refs = g32(poly + WP_LMANIMREFS), nrefs = g32(poly + WP_NLMANIMREFS);
    if ((refs == 0) || (nrefs == 0) || (nrefs > 64)) return 0;
    memset(texels, 0, (size_t)n * 4);
    int on = 0;     // layers that are on, in order (the first is taken as is)
    for (uint32_t r = 0; r < nrefs; r++)
    {
        uint32_t ia, anim, lf = anim_any_frame(mw, refs + 4 * r, &ia);
        if (lf == 0) continue;
        anim = g32(mw + MW_LIGHTANIMS + 4) + ia * LA_SIZE;
        if (g32(anim + LA_SHADOWMAP)) continue;
        uint32_t data = g32(lf), len = g16(lf + 4);
        if ((data == 0) || (len == 0)) continue;
        if (anim_is_dynamic(anim) && (pl->n < MAX_LAYERS))
        {
            memset(layers[pl->n], 0, (size_t)n * 4);
            if (!decompress_lm(data, len, layers[pl->n], (uint32_t)n)) continue;
            if (pl->has_base) pl->after_base |= 1 << pl->n;
            pl->anim[pl->n++] = ia;
            continue;
        }
        uint32_t dummy;
        if (anim_frame(mw, refs + 4 * r, &dummy) == 0) continue;    // off now
        memset(layer, 0, (size_t)n * 4);
        if (!decompress_lm(data, len, layer, (uint32_t)n)) continue;
        if (on == 0) memcpy(texels, layer, (size_t)n * 4);
        else lm_add_scaled(texels, layer, n, (uint32_t)(gf(anim + LA_BLEND) * 255.0f));
        on++;
        if (!anim_is_dynamic(anim)) pl->has_base = 1;
    }
    return (on > 0) || (pl->n > 0);
}

// a Gouraud poly's vertex colours (r, g, b per vertex): the sum of its animations' frames that are on
// (d3d_AddLightAnimVertexColors, not scaled by the blend), and up to MAX_VLAYERS changing layers
static void poly_vertex_light(uint32_t mw, uint32_t poly, uint32_t n, uint8_t (*rgb)[3], uint8_t (*vlayers)[1024][3], poly_layers *pl)
{
    memset(rgb, 0, (size_t)n * 3);
    pl->n = 0;
    pl->after_base = 0;
    pl->has_base = 0;
    uint32_t refs = g32(poly + WP_LMANIMREFS), nrefs = g32(poly + WP_NLMANIMREFS);
    if ((refs == 0) || (nrefs > 64)) return;
    for (uint32_t r = 0; r < nrefs; r++)
    {
        uint32_t ia, anim, lf = anim_any_frame(mw, refs + 4 * r, &ia);
        if (lf == 0) continue;
        anim = g32(mw + MW_LIGHTANIMS + 4) + ia * LA_SIZE;
        uint32_t vr = g32(lf + LF_VERTR), vg = g32(lf + LF_VERTG), vb = g32(lf + LF_VERTB), nv = g8(lf + LF_NVERTS);
        if ((vr == 0) || (vg == 0) || (vb == 0)) continue;
        uint8_t (*dst)[3] = rgb;
        if (anim_is_dynamic(anim) && (pl->n < MAX_VLAYERS))
        {
            dst = vlayers[pl->n];
            memset(dst, 0, (size_t)n * 3);
            pl->anim[pl->n++] = ia;
        }
        else
        {
            uint32_t dummy;
            if (anim_frame(mw, refs + 4 * r, &dummy) == 0) continue;     // off now
        }
        for (uint32_t i = 0; (i < n) && (i < nv); i++)
        {
            uint32_t c;
            c = dst[i][0] + g8(vr + i); dst[i][0] = (uint8_t)((c > 255) ? 255 : c);
            c = dst[i][1] + g8(vg + i); dst[i][1] = (uint8_t)((c > 255) ? 255 : c);
            c = dst[i][2] + g8(vb + i); dst[i][2] = (uint8_t)((c > 255) ? 255 : c);
        }
    }
}

// the light animations' current state for the shader: per animation (blend, on)
static std::vector<float> anim_state;

static void update_anims(uint32_t mw)
{
    if (anim_image.is_null()) return;
    uint32_t anims = g32(mw + MW_LIGHTANIMS + 4), n = g32(mw + MW_LIGHTANIMS + 8);
    if (n > ANIM_TEX_W * ANIM_TEX_H) n = ANIM_TEX_W * ANIM_TEX_H;
    int changed = 0;
    if (anim_state.size() != (size_t)n * 2) { anim_state.assign((size_t)n * 2, -1.0f); changed = 1; }
    for (uint32_t a = 0; a < n; a++)
    {
        uint32_t la = anims + a * LA_SIZE;
        float blend = gf(la + LA_BLEND);
        float on = ((g32(la + LA_IFRAMES) != 0xffffffffu) && (blend >= 0.02f)) ? 1.0f : 0.0f;
        if ((anim_state[a * 2] != blend) || (anim_state[a * 2 + 1] != on))
        {
            anim_state[a * 2] = blend;
            anim_state[a * 2 + 1] = on;
            anim_image->set_pixel(a % ANIM_TEX_W, a / ANIM_TEX_W, Color(blend, on, 0, 0));
            changed = 1;
        }
    }
    if (changed) anim_texture->update(anim_image);
}

/* ------------------------------------------------------------------ */
/* objects                                                             */

// The client's objects: the world the renderer is bound to is CClientMgr::m_World, followed by its ObjectMgr
// (0x1f0), whose m_ObjectLists (LTList per OT_ type: count, then the head LTLink {prev, next, data}) are at +0x220.
#define MW_OBJECTLISTS (0x1f0 + 0x220)
#define NUM_OBJECTTYPES 11
// LTObject
#define OB_LINK 0x7c
#define OB_FLAGS 0x88
#define OB_FLAGS2 0x8c
#define OB_COLOR 0x94
#define OB_ROTATION 0x9c
#define OB_SCALE 0xac
#define OB_TYPE 0xbc
#define OB_SD 0x190
#define OB_POS 0x194
#define OB_DIMS 0x1a0
// WorldModelInstance
#define WM_ORIGINALBSP 0x1b0
#define WM_WORLDBSP 0x1b4
#define WM_VALIDBSP 0x1b8
#define WM_TRANSFORM 0x1f8
#define WB_NAME 0x04
#define WB_WORLDTRANSLATION 0xe8

enum { OT_NORMAL = 0, OT_MODEL, OT_WORLDMODEL, OT_SPRITE, OT_LIGHT, OT_CAMERA, OT_PARTICLESYSTEM, OT_POLYGRID, OT_LINESYSTEM, OT_CONTAINER, OT_CANVAS };
#define FLAG_VISIBLE 1
#define FLAG2_SKYOBJECT (1 << 8)

// calls fn(object) for each object of a type
template <typename F> static void for_objects(uint32_t mw, int type, F fn)
{
    uint32_t head = mw + MW_OBJECTLISTS + type * 0x10 + 4;
    uint32_t n = 0;
    for (uint32_t l = g32(head + 4); (l != head) && (l != 0) && (n < 100000); l = g32(l + 4), n++)
    {
        uint32_t obj = g32(l + 8);
        if (obj != 0) fn(obj);
    }
}

static void dump_objects(uint32_t mw)
{
    static const char *names[NUM_OBJECTTYPES] = { "normal", "model", "worldmodel", "sprite", "light", "camera", "particles", "polygrid", "lines", "container", "canvas" };
    for (int t = 0; t < NUM_OBJECTTYPES; t++)
    {
        eprintf("render3d: objects %s: %u\n", names[t], g32(mw + MW_OBJECTLISTS + t * 0x10));
        for_objects(mw, t, [&](uint32_t o) {
            eprintf("render3d:   %08x type %u flags %08x/%08x color %02x%02x%02x%02x sd %08x pos %.1f %.1f %.1f rot %.3f %.3f %.3f %.3f scale %.2f %.2f %.2f dims %.1f %.1f %.1f\n",
                    o, g8(o + OB_TYPE), g32(o + OB_FLAGS), g32(o + OB_FLAGS2), g8(o + OB_COLOR), g8(o + OB_COLOR + 1), g8(o + OB_COLOR + 2), g8(o + OB_COLOR + 3), g32(o + OB_SD),
                    gf(o + OB_POS), gf(o + OB_POS + 4), gf(o + OB_POS + 8), gf(o + OB_ROTATION), gf(o + OB_ROTATION + 4), gf(o + OB_ROTATION + 8), gf(o + OB_ROTATION + 12),
                    gf(o + OB_SCALE), gf(o + OB_SCALE + 4), gf(o + OB_SCALE + 8), gf(o + OB_DIMS), gf(o + OB_DIMS + 4), gf(o + OB_DIMS + 8));
            if (t == OT_MODEL)
            {
                // Primal Hunt: 4 bytes more before the tracker, 0x14 before the hidden pieces (MI_MODEL below)
                uint32_t d1 = game.primal_hunt ? 4 : 0, d2 = game.primal_hunt ? 0x14 : 0;
                uint32_t model = g32(o + 0x1f8 + d1), vt = g32(o);
                eprintf("render3d:     vtable %08x [18] %08x model %08x skins %08x %08x %08x %08x hidden %08x transforms %08x/%u light %.2f %.2f %.2f\n", vt, vt ? g32(vt + 0x48) : 0, model,
                        g32(o + 0x1c0), g32(o + 0x1c4), g32(o + 0x1c8), g32(o + 0x1cc), g32(o + 0x24c + d2), g32(o + 0x238 + d2), g32(o + 0x23c + d2), gf(o + 0x2c0 + d2), gf(o + 0x2c4 + d2), gf(o + 0x2c8 + d2));
                if (model == 0) return;
                uint32_t fn = g32(model + 4);
                eprintf("render3d:     \"%s\" pieces %u nodes %u (flat %u) weights %u verts %u tris %u\n", fn ? (const char *)G2H(fn) : "?", g32(model + 0x3c), g32(model + 0x78), g32(model + 0x2c),
                        g32(model + 0x5c), g32(model + 0x64), g32(model + 0x68));
                for (uint32_t i = 0; (i < g32(model + 0x3c)) && (i < 8); i++)
                {
                    uint32_t pc = g32(g32(model + 0x38) + 4 * i);
                    eprintf("render3d:       piece \"%.32s\" texture %u verts %u tris %u lods %u\n", (const char *)G2H(pc + 0x48), g32(pc + 0x24), g32(pc + 8), g32(pc + 0x18), g32(pc + 0x3c));
                }
            }
            if ((t == OT_WORLDMODEL) || (t == OT_CONTAINER))
            {
                uint32_t bsp = g32(o + WM_ORIGINALBSP);
                if (bsp == 0) return;
                eprintf("render3d:     bsp %08x/%08x/%08x \"%.64s\" wif %08x polies %u translation %.1f %.1f %.1f\n", bsp, g32(o + WM_WORLDBSP), g32(o + WM_VALIDBSP),
                        (const char *)G2H(bsp + WB_NAME), g32(bsp + WB_WORLDINFOFLAGS), g32(bsp + WB_NPOLIES),
                        gf(bsp + WB_WORLDTRANSLATION), gf(bsp + WB_WORLDTRANSLATION + 4), gf(bsp + WB_WORLDTRANSLATION + 8));
                uint32_t m = o + WM_TRANSFORM;
                eprintf("render3d:     transform %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f\n", gf(m), gf(m + 4), gf(m + 8), gf(m + 12),
                        gf(m + 16), gf(m + 20), gf(m + 24), gf(m + 28), gf(m + 32), gf(m + 36), gf(m + 40), gf(m + 44));
            }
        });
    }
}

/* ------------------------------------------------------------------ */
/* the world                                                           */

struct batch
{
    PackedVector3Array v;
    PackedVector2Array uv, uv2;
    PackedColorArray col;
    // the changing light layers: animation indices + 1 (0: none); lightmap layer stride (u), after_base bits,
    // has_base; vertex layers' colours
    PackedFloat32Array c0, c1, c2, c3;
};
// polygons grouped by (texture, material kind, lightmap page)
typedef std::map<uint64_t, batch> batch_map;

#define FLAG2_CHROMAKEY (1 << 1)

// AVP2_R3D_WORLDALPHA=1: the world's (and world models') textures with alpha always alpha tested
static int world_alpha_always(void)
{
    static int v = -1;
    if (v < 0) v = (getenv("AVP2_R3D_WORLDALPHA") != NULL) && (atoi(getenv("AVP2_R3D_WORLDALPHA")) != 0);
    return v;
}

struct build_stats
{
    uint32_t polies, tris, lightmapped;
    uint32_t sprite_polies = 0;     // SURF_SPRITEANIMATE
    uint32_t alpha_main = 0, alpha_wm = 0;      // alpha tested (the texture has alpha): the main world's, world models'
    int main_world = 0;             // add_bsp: the main world's (or terrain's) BSP
};

static void clear_world(void)
{
    for (MeshInstance3D *mi : gs->world_meshes) mi->queue_free();
    gs->world_meshes.clear();
    for (auto &kv : gs->wm_instances) kv.second.mi->queue_free();
    gs->wm_instances.clear();
    for (auto &kv : gs->models) kv.second.mi->queue_free();
    gs->models.clear();
    for (auto &kv : gs->sprites) kv.second.mi->queue_free();
    gs->sprites.clear();
    for (auto &kv : gs->particles) kv.second.mi->queue_free();
    gs->particles.clear();
    for (auto &kv : gs->lines) kv.second.mi->queue_free();
    gs->lines.clear();
    for (auto &kv : gs->polygrids) kv.second.mi->queue_free();
    gs->polygrids.clear();
    for (auto &kv : gs->canvases) kv.second.mi->queue_free();
    gs->canvases.clear();
    gs->sprite_materials.clear();
    for (auto &kv : gs->textures) kv.second.materials.clear();
    gs->bsp_meshes.clear();
    gs->bsp_keys.clear();
    for (auto &kv : gs->sky_instances) kv.second.mi->queue_free();
    gs->sky_instances.clear();
    delete wm;
    wm = new world_materials();
}

// a BSP's polygons into batches, in the BSP's own coordinates (where it was placed in the level)
static void add_bsp(uint32_t mw, uint32_t bsp, float grid, batch_map &batches, build_stats &st)
{
    static uint32_t texels[0x400], layers[MAX_LAYERS][0x400];
    static uint8_t vlight[1024][3], vlayers[MAX_VLAYERS][1024][3];
    uint32_t polies = g32(bsp + WB_POLIES), npolies = g32(bsp + WB_NPOLIES);
    if ((polies == 0) || (npolies > 1000000)) return;
    for (uint32_t p = 0; p < npolies; p++)
    {
        uint32_t poly = g32(polies + 4 * p);
        if (poly == 0) continue;
        uint32_t surface = g32(poly + WP_SURFACE);
        if (surface == 0) continue;
        uint32_t sflags = g32(surface + SF_FLAGS);
        // debugging: AVP2_R3D_DUMPBSP=<name part> lists that BSP's polies
        static const char *dump_bsp = getenv("AVP2_R3D_DUMPBSP");
        if ((dump_bsp != NULL) && strstr((const char *)G2H(bsp + WB_NAME), dump_bsp))
        {
            uint32_t pv = g32(poly + WP_PVERTICES), pn = (pv == poly + WP_VERTICES) ? g16(poly + WP_NVERTICES) : g16(poly + WP_NEXTRAVERTICES);
            eprintf("render3d: bsp \"%.40s\" poly %u: surface flags %08x texture %08x verts %u (%08x, first point %08x)\n", (const char *)G2H(bsp + WB_NAME), p, sflags,
                    g32(surface + SF_TEXTURE), pn, pv, pv ? g32(pv + PV_VEC) : 0);
        }
        if (sflags & (SURF_INVISIBLE | SURF_NONEXISTANT | SURF_SKY)) continue;
        if (sflags & SURF_SPRITEANIMATE) st.sprite_polies++;
        uint32_t tex = g32(surface + SF_TEXTURE);
        uint32_t verts = g32(poly + WP_PVERTICES);
        // the poly's own vertices, or a reallocated list of m_nExtraVertices (the poly grew)
        uint32_t n = (verts == poly + WP_VERTICES) ? g16(poly + WP_NVERTICES) : g16(poly + WP_NEXTRAVERTICES);
        if ((verts == 0) || (n < 3)) continue;
        uint32_t k;
        for (k = 0; k < n; k++)
        {
            uint32_t pt = g32(verts + k * PV_SIZE + PV_VEC);
            if ((pt < 0x10000) || (pt >= 0x80000000u)) break;
        }
        if (k < n) continue;

        int kind = MAT_OPAQUE;
        if (sflags & SURF_ADDITIVE) kind = MAT_ADD;
        else if (sflags & SURF_TRANSPARENT) kind = MAT_BLEND;
        else if (get_texture(tex).has_alpha) kind = MAT_ALPHA_TEST;
        if (kind == MAT_ALPHA_TEST) { if (st.main_world) st.alpha_main++; else st.alpha_wm++; }
        if ((kind == MAT_ALPHA_TEST) && st.main_world && getenv("AVP2_R3D_DUMPALPHA"))
        {
            // debugging: the main world's alpha tested textures (their command strings), once each
            static std::map<uint32_t, int> seen;
            if (seen[tex]++ == 0)
            {
                uint32_t cl = g32(tex + 0x38);
                eprintf("render3d: main world alpha texture %08x cmd \"%.80s\" surface flags %08x\n", tex,
                        ((cl > 0x10000) && (cl < 0x80000000u)) ? (const char *)G2H(cl) : "", sflags);
            }
        }
        // d3d.ren draws the main world without an alpha test (only the chroma key world models and models are drawn
        // in its alpha tested pass): opaque, also where the texture is a cut-out (ColorKey/AlphaRef). AVP2_R3D_WORLDALPHA
        // = 1: alpha tested anyway (the earlier behaviour), 0: as d3d.ren (default)
        if ((kind == MAT_ALPHA_TEST) && st.main_world && !world_alpha_always()) kind = MAT_OPAQUE;
        texture_entry &te = get_texture(tex);

        // the lightmap
        int lw = 0, lh = 0, page = -1, lx = 0, ly = 0;
        float right[3] = { 0, 0, 0 }, up[3] = { 0, 0, 0 }, org[3] = { 0, 0, 0 };
        poly_layers pl;
        pl.n = 0; pl.after_base = 0; pl.has_base = 0;
        if (!(sflags & SURF_BRIGHT) && poly_lightmap(mw, poly, texels, layers, &pl, &lw, &lh))
        {
            const uint32_t *blocks[1 + MAX_LAYERS] = { texels, layers[0], layers[1], layers[2], layers[3] };
            page = lm_place(blocks, 1 + pl.n, lw, lh, &lx, &ly);
            lm_axes(poly, right, up);
            org[0] = gf(poly + WP_LMORIGIN); org[1] = gf(poly + WP_LMORIGIN + 4); org[2] = gf(poly + WP_LMORIGIN + 8);
            st.lightmapped++;
        }
        batch &b = batches[(sflags & SURF_SPRITEANIMATE) ? (((uint64_t)surface << 16) | ((uint64_t)(page + 1) << 4) | (uint64_t)kind | SPRITE_KEY)
                                                       : (((uint64_t)tex << 16) | ((uint64_t)(page + 1) << 4) | (uint64_t)kind)];
        if ((page < 0) && (n <= 1024))
        {
            if (sflags & (SURF_BRIGHT | SURF_LIGHTMAP)) pl.n = 0;
            else poly_vertex_light(mw, poly, n, vlight, vlayers, &pl);
        }
        float lc0[4] = { 0, 0, 0, 0 }, lc1[4] = { (float)(lw + 2) / LM_PAGE_SIZE, (float)pl.after_base, (float)pl.has_base, 0 };
        for (int k = 0; k < pl.n; k++) lc0[k] = (float)(pl.anim[k] + 1);
        for (k = 1; k + 1 < n; k++)
        {
            uint32_t idx[3] = { 0, k + 1, k };     // the z flip mirrors the winding
            for (int j = 0; j < 3; j++)
            {
                uint32_t pv = verts + idx[j] * PV_SIZE, pt = g32(pv + PV_VEC);
                b.v.push_back(lt_pos(pt));
                for (int q = 0; q < 4; q++) { b.c0.push_back(lc0[q]); b.c1.push_back(lc1[q]); }
                {
                    float v0[4] = { 0, 0, 0, 0 }, v1[4] = { 0, 0, 0, 0 };
                    if ((page < 0) && (idx[j] < 1024))
                    {
                        if (pl.n > 0) for (int q = 0; q < 3; q++) v0[q] = vlayers[0][idx[j]][q] / 255.0f;
                        if (pl.n > 1) for (int q = 0; q < 3; q++) v1[q] = vlayers[1][idx[j]][q] / 255.0f;
                    }
                    for (int q = 0; q < 4; q++) { b.c2.push_back(v0[q]); b.c3.push_back(v1[q]); }
                }
                b.uv.push_back(Vector2(gf(pv + PV_U) * te.uv_w, gf(pv + PV_V) * te.uv_h));
                if (page >= 0)
                {
                    float d[3] = { gf(pt) - org[0], gf(pt + 4) - org[1], gf(pt + 8) - org[2] };
                    float u = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / grid + 0.5f;
                    float v = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / grid + 0.5f;
                    b.uv2.push_back(Vector2((lx + u) / LM_PAGE_SIZE, (ly + v) / LM_PAGE_SIZE));
                    b.col.push_back(Color(1, 1, 1, 1));
                }
                else
                {
                    b.uv2.push_back(Vector2(0, 0));
                    // full bright, or the vertex colour; lightmapped surfaces without one: full bright
                    if (sflags & (SURF_BRIGHT | SURF_LIGHTMAP)) b.col.push_back(Color(1, 1, 1, 1));
                    else if (idx[j] < 1024)
                    {
                        const uint8_t *c = vlight[idx[j]];
                        b.col.push_back(Color(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f));
                    }
                    else b.col.push_back(Color(1, 1, 1, 1));
                }
            }
            st.tris++;
        }
        st.polies++;
    }
}

// batches -> meshes, a surface per batch (Godot allows 256 surfaces per mesh)
static std::vector<Ref<ArrayMesh>> make_meshes(batch_map &batches)
{
    std::vector<Ref<ArrayMesh>> out;
    Ref<ArrayMesh> mesh;
    int surfaces = 0;
    for (auto &kv : batches)
    {
        if (mesh.is_null() || (surfaces == 250))
        {
            mesh.instantiate();
            out.push_back(mesh);
            surfaces = 0;
        }
        Array arrays;
        arrays.resize(Mesh::ARRAY_MAX);
        arrays[Mesh::ARRAY_VERTEX] = kv.second.v;
        arrays[Mesh::ARRAY_TEX_UV] = kv.second.uv;
        arrays[Mesh::ARRAY_TEX_UV2] = kv.second.uv2;
        arrays[Mesh::ARRAY_COLOR] = kv.second.col;
        arrays[Mesh::ARRAY_CUSTOM0] = kv.second.c0;
        arrays[Mesh::ARRAY_CUSTOM1] = kv.second.c1;
        arrays[Mesh::ARRAY_CUSTOM2] = kv.second.c2;
        arrays[Mesh::ARRAY_CUSTOM3] = kv.second.c3;
        int64_t fmt = ((int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT) | ((int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT) |
                      ((int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM2_SHIFT) | ((int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM3_SHIFT);
        mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), (BitField<Mesh::ArrayFormat>)fmt);
        mesh->surface_set_material(surfaces, batch_material(kv.first));
        surfaces++;
    }
    return out;
}

// The world: the main world's BSP and the terrain are one static mesh. Every other BSP (doors, lifts, glass,
// the sky's world models, ...) is drawn through its world model object, which may move or be hidden: those
// get their own meshes, placed each frame (update_world_models).
static void build_world(world_context *ctx)
{
    clear_world();
    uint32_t mw = ctx->world;
    uint32_t models = g32(mw + MW_WORLDMODELS + 4), nmodels = g32(mw + MW_WORLDMODELS + 8);
    if (winapi_debug) eprintf("render3d: world %08x, %u world models\n", mw, nmodels);
    if ((models == 0) || (nmodels > 100000)) return;

    float grid = gf(mw + MW_LMGRIDSIZE);
    if (!(grid > 0.0f)) grid = 32.0f;
    if (winapi_debug)
    {
        uint32_t multi = 0, shadow = 0, base = g32(mw + MW_LIGHTANIMS + 4);
        for (uint32_t a = 0; a < g32(mw + MW_LIGHTANIMS + 8); a++)
        {
            if (g8(base + a * LA_SIZE + LA_NFRAMES) > 1) multi++;
            if (g32(base + a * LA_SIZE + LA_SHADOWMAP)) shadow++;
        }
        eprintf("render3d: %u light animations (%u multi-frame, %u shadow-map), lightmap grid %.1f\n", g32(mw + MW_LIGHTANIMS + 8), multi, shadow, grid);
    }
    if (winapi_debug >= 2)
        for (uint32_t a = 0; a < g32(mw + MW_LIGHTANIMS + 8); a++)
        {
            uint32_t la = g32(mw + MW_LIGHTANIMS + 4) + a * LA_SIZE;
            eprintf("render3d: light anim %u \"%.32s\" shadowmap %u frames %u polies %u iframes %d %d between %u blend %.2f pos %.0f %.0f %.0f colour %.0f %.0f %.0f radius %.0f\n", a,
                    (const char *)G2H(la), g32(la + 0x20), g8(la + 0x28), g16(la + 0x30), (int)g32(la + 0x34), (int)g32(la + 0x38), g32(la + 0x3c), gf(la + 0x40),
                    gf(la + 0x44), gf(la + 0x48), gf(la + 0x4c), gf(la + 0x50), gf(la + 0x54), gf(la + 0x58), gf(la + 0x5c));
        }

    batch_map batches;
    std::map<uint32_t, batch_map> bsp_batches;
    build_stats st = { 0, 0, 0 };
    for (uint32_t i = 0; i < nmodels; i++)
    {
        uint32_t wd = g32(models + 4 * i);
        if (wd == 0) continue;
        uint32_t bsp = g32(wd + WD_ORIGINALBSP);
        if (bsp == 0) continue;
        uint32_t wif = g32(bsp + WB_WORLDINFOFLAGS);
        if (winapi_debug >= 2)
            eprintf("render3d: bsp %u %08x \"%.64s\" wif %08x polies %u translation %.1f %.1f %.1f\n", i, bsp, (const char *)G2H(bsp + 4), wif, g32(bsp + WB_NPOLIES),
                    gf(bsp + 0xe8), gf(bsp + 0xec), gf(bsp + 0xf0));
        if (wif & WIF_VISBSP) continue;
        st.main_world = (wif & (WIF_MAINWORLD | WIF_TERRAIN)) != 0;
        if (st.main_world) add_bsp(mw, bsp, grid, batches, st);
        else add_bsp(mw, bsp, grid, bsp_batches[bsp], st);
    }
    upload_lm_pages();

    for (Ref<ArrayMesh> &mesh : make_meshes(batches))
    {
        MeshInstance3D *mi = memnew(MeshInstance3D);
        mi->set_mesh(mesh);
        gs->root->add_child(mi);
        gs->world_meshes.push_back(mi);
    }
    for (auto &kv : bsp_batches)
    {
        std::vector<Ref<ArrayMesh>> m = make_meshes(kv.second);
        if (!m.empty()) gs->bsp_meshes[kv.first] = m[0];
        std::vector<uint64_t> &keys = gs->bsp_keys[kv.first];
        for (auto &b : kv.second) keys.push_back(b.first);    // the surfaces' materials, in their order
    }
    ctx->built = 1;
    if (winapi_debug) eprintf("render3d: built %u polies (%u lightmapped, %zu lightmap pages, %u sprite-animated, alpha tested: %u main world, %u world models), %u triangles, %zu static batches, %zu world model meshes, %zu textures\n",
                              st.polies, st.lightmapped, wm->pages.size(), st.sprite_polies, st.alpha_main, st.alpha_wm, st.tris, batches.size(), gs->bsp_meshes.size(), gs->textures.size());
}

// LithTech's LTMatrix (rows, translation in the 4th column) -> Godot: S M S with S = diag(1, 1, -1)
static Transform3D lt_matrix(uint32_t m)
{
    Basis b(gf(m), gf(m + 4), -gf(m + 8),
            gf(m + 16), gf(m + 20), -gf(m + 24),
            -gf(m + 32), -gf(m + 36), gf(m + 40));
    return Transform3D(b, Vector3(gf(m + 12), gf(m + 28), -gf(m + 44)));
}

// World model objects (OT_WORLDMODEL, OT_CONTAINER) that are visible: their BSP's mesh where the object is now
// (m_Transform: from where the BSP was placed to the object's position and rotation)
static void update_world_models(uint32_t mw)
{
    gs->frame++;
    static const int types[2] = { OT_WORLDMODEL, OT_CONTAINER };
    for (int t : types)
    {
        for_objects(mw, t, [&](uint32_t o) {
            uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
            if (!(flags & FLAG_VISIBLE) || (flags2 & FLAG2_SKYOBJECT)) return;
            uint32_t bsp = g32(o + WM_ORIGINALBSP);
            auto mit = gs->bsp_meshes.find(bsp);
            if (mit == gs->bsp_meshes.end()) return;
            wm_instance &wi = gs->wm_instances[o];
            if (wi.mi == NULL)
            {
                wi.mi = memnew(MeshInstance3D);
                gs->root->add_child(wi.mi);
                wi.bsp = 0;
            }
            if (wi.bsp != bsp)
            {
                wi.mi->set_mesh(mit->second);
                wi.bsp = bsp;
                wi.chromakey = -1;
            }
            // d3d.ren alpha tests a world model only with FLAG2_CHROMAKEY (d3d_DrawChromaKeyWorldModels); the others
            // are drawn opaque: their alpha tested surfaces (the BSP's batches: the texture has alpha) get the opaque
            // material instead
            int chroma = (flags2 & FLAG2_CHROMAKEY) ? 1 : 0;
            if ((chroma != wi.chromakey) && !world_alpha_always())
            {
                std::vector<uint64_t> &keys = gs->bsp_keys[bsp];
                for (size_t k = 0; k < keys.size(); k++)
                {
                    uint64_t key = keys[k];
                    int kind = (int)(key & 0xf);
                    if (((kind & 3) != MAT_ALPHA_TEST) || (key & SPRITE_KEY)) continue;
                    Ref<Material> m;
                    if (!chroma) m = get_material(key_texture(key), (kind & ~3) | MAT_OPAQUE, (int)((key >> 4) & 0xfff) - 1);
                    wi.mi->set_surface_override_material((int)k, m);
                }
                wi.chromakey = chroma;
            }
            wi.mi->set_transform(lt_matrix(o + WM_TRANSFORM));
            // the object's alpha fades the whole model (translucent world models)
            uint8_t a = g8(o + OB_COLOR + 3);
            if (a != wi.alpha)
            {
                wi.mi->set_transparency(1.0f - a / 255.0f);
                wi.alpha = a;
            }
            if (!wi.shown) { wi.mi->set_visible(true); wi.shown = 1; }
            wi.frame = gs->frame;
        });
    }
    for (auto &kv : gs->wm_instances)
    {
        wm_instance &wi = kv.second;
        if ((wi.frame != gs->frame) && wi.shown) { wi.mi->set_visible(false); wi.shown = 0; }
    }
}

/* ------------------------------------------------------------------ */
/* models                                                              */

// ModelInstance
#define MI_SKINS 0x1c0              // SharedTexture*[4]
// Primal Hunt's ModelInstance has an environment map after the skins (+0x1d0): the tracker is 4 bytes later, the
// hidden pieces 0x14 (avp2src: server/commonmodel.h, d3d/ph/drawmodel_env.cpp, d3d/ph/d3d_modelvb.cpp)
#define MI_MODEL (game.primal_hunt ? 0x1fc : 0x1f8)                 // m_AnimTracker.m_TimeRef.m_pModel
#define MI_HIDDENPIECES (game.primal_hunt ? 0x260 : 0x24c)
#define MI_VT_GETTRANSFORMS 0x48    // virtual LTMatrix *GetTransforms() (thiscall): the node transforms in model space
// Model
#define MD_PIECES 0x34              // CMoArray<ModelPiece*>
#define MD_TRANSFORMS 0x70          // CMoArray<LTMatrix>: its size is the node count
#define MD_GLOBALRADIUS 0xa4
// ModelPiece (LOD 0 is the piece's own vertices and triangles)
#define PC_VERTS 0x00               // CMoArray<ModelVert> (0x20 bytes: weights*, u16 count, u16, rest position, rest normal)
#define PC_TRIS 0x10                // CMoArray<ModelTri> (0x20 bytes: u16 indices[3], pad, UVs[3])
#define PC_TEXTURE 0x24
// DynamicLight
#define DL_RADIUS 0x1b0
// RenderStruct
#define RS_LIGHTDIR 0x138
#define RS_LIGHTCOLOR 0x144
// SceneDesc
#define SD_MODELLIGHTADD 0x1c
#define SD_VERTEXTINT 0x5c

#define FLAG_ONLYLIGHTWORLD (1 << 5)
#define FLAG_REALLYCLOSE (1 << 6)
#define FLAG_NOLIGHT (1 << 9)
#define FLAG2_ADDITIVE (1 << 2)
#define FLAG2_DYNAMICDIRLIGHT (1 << 7)

// the world's light grid at a point (w_GetLightVal: trilinear, LTRGB = b, g, r, a)
static void light_table(uint32_t mw, const float p[3], float out[3])
{
    uint32_t t = mw + 0xfc;
    uint32_t data = g32(t);
    out[0] = out[1] = out[2] = 0.0f;
    if ((data == 0) || (g32(t + 4) == 0)) return;
    uint32_t dims[3] = { g32(t + 8), g32(t + 12), g32(t + 16) }, dm1[3] = { g32(t + 0x14), g32(t + 0x18), g32(t + 0x1c) };
    uint32_t xy = g32(t + 0x20);
    float f[3], inv[3];
    int g[3];
    for (int i = 0; i < 3; i++)
    {
        f[i] = (p[i] - gf(t + 0x3c + 4 * i)) * gf(t + 0x30 + 4 * i);
        g[i] = (int)f[i];
        if (g[i] < 0) g[i] = 0;
        if (g[i] > (int)dm1[i]) g[i] = (int)dm1[i];
        if (g[i] == (int)dm1[i]) { f[i] = 0.0f; inv[i] = 1.0f; g[i]--; if (g[i] < 0) g[i] = 0; }
        else { f[i] -= floorf(f[i]); inv[i] = 1.0f - f[i]; }
    }
    uint32_t base = data + 4 * (g[0] + g[1] * dims[0] + g[2] * xy);
    if (base + 4 * (xy + dims[0] + 1) >= data + 4 * g32(t + 4) + 4) return;
    for (int c = 0; c < 3; c++)
    {
        int ch = 2 - c;     // r, g, b from b, g, r
        auto s = [&](uint32_t a) { return (float)g8(a + ch); };
        float y0 = s(base) * inv[1] + s(base + 4 * dims[0]) * f[1];
        float y1 = s(base + 4) * inv[1] + s(base + 4 * (dims[0] + 1)) * f[1];
        float z0 = y0 * inv[0] + y1 * f[0];
        uint32_t b2 = base + 4 * xy;
        y0 = s(b2) * inv[1] + s(b2 + 4 * dims[0]) * f[1];
        y1 = s(b2 + 4) * inv[1] + s(b2 + 4 * (dims[0] + 1)) * f[1];
        float z1 = y0 * inv[0] + y1 * f[0];
        out[c] = (float)(int)(z0 * inv[2] + z1 * f[2]);
    }
}

// LithTech position, rotation, scale -> Godot
static Transform3D lt_object_transform(uint32_t o)
{
    Quaternion q(-gf(o + OB_ROTATION), -gf(o + OB_ROTATION + 4), gf(o + OB_ROTATION + 8), gf(o + OB_ROTATION + 12));
    if (q.length_squared() < 1e-6f) q = Quaternion();
    Basis b = Basis(q.normalized()) * Basis::from_scale(Vector3(gf(o + OB_SCALE), gf(o + OB_SCALE + 4), gf(o + OB_SCALE + 8)));
    return Transform3D(b, lt_pos(o + OB_POS));
}

struct model_light { float pos[3], dir[3], radius2, color[3]; };

// the engine's i_IntersectSegment (cdecl: IntersectQuery*, IntersectInfo*, WorldTree*, bServer), recompiled
#define MW_WORLDTREE 0x6c
#define WT_ROOT 0x0c
#define WTN_RADIUS 0x40

// CastRayAtSky: does the segment from p along d end on a sky poly? (the query and its result in 32-bit memory)
static int ray_at_sky(uint32_t mw, const float p[3], const float d[3])
{
    static uint32_t query_init[16], info_init[16];  // the engine's: below 2 GB (LOW_OBJ)
    LOW_SLOT(query) LOW_SLOT(info)
    uint32_t *query = LOW_OBJ(query), *info = LOW_OBJ(info);
    float *q = (float *)query;
    memset(query, 0, sizeof(query_init));
    memset(info, 0, sizeof(info_init));
    for (int i = 0; i < 3; i++) { q[i] = p[i]; q[3 + i] = p[i] + d[i]; }
    query[9] = 4;       // INTERSECT_HPOLY
    uint32_t args[4] = { H2G(query), H2G(info), mw + MW_WORLDTREE, 0 };
    if (!call_game(H2G(lithtech_i_IntersectSegment), 4, args)) return 0;
    uint32_t hpoly = info[8];
    uint32_t models = g32(mw + MW_WORLDMODELS + 4), nmodels = g32(mw + MW_WORLDMODELS + 8);
    if ((hpoly >> 16) >= nmodels) return 0;
    uint32_t wd = g32(models + 4 * (hpoly >> 16));
    uint32_t bsp = wd ? g32(wd + WD_ORIGINALBSP) : 0;
    if ((bsp == 0) || ((hpoly & 0xffff) >= g32(bsp + WB_NPOLIES))) return 0;
    uint32_t poly = g32(g32(bsp + WB_POLIES) + 4 * (hpoly & 0xffff));
    uint32_t surface = poly ? g32(poly + WP_SURFACE) : 0;
    return surface && (g32(surface + SF_FLAGS) & SURF_SKY);
}

// ModelDraw::GetDirLightAmount: rays toward the sun from the model's centre, and where the top and the bottom
// of the model (along the sun's "up") disagree, a binary search (ModelSunVariance 0.05)
static float sun_amount(uint32_t mw, const float p[3], const float right[3], float radius)
{
    const float *ld = (const float *)(render_struct + RS_LIGHTDIR);
    float len = gf(mw + MW_WORLDTREE + WT_ROOT + WTN_RADIUS) * -2.0f;
    float d[3] = { ld[0] * len, ld[1] * len, ld[2] * len };
    // vLightUp = LightDir.Cross(vRight) (LithTech's Cross: b x a) * radius
    float up[3];
    cross(right, ld, up);
    for (int i = 0; i < 3; i++) up[i] *= radius;
    auto at = [&](float f) { float q[3] = { p[0] + up[0] * f, p[1] + up[1] * f, p[2] + up[2] * f }; return ray_at_sky(mw, q, d); };
    int top = at(1.0f), bottom = at(-1.0f);
    if (top == bottom) return top ? 1.0f : 0.0f;
    float variance = 1.0f, ftop = 1.0f, fbottom = -1.0f, light = 1.0f;
    while ((variance > 0.05f) && (bottom != top))
    {
        float mid = (ftop + fbottom) * 0.5f;
        int m = at(mid);
        variance *= 0.5f;
        if (!m) light -= variance;
        if (m == top) { ftop = mid; top = m; }
        else { fbottom = mid; bottom = m; }
    }
    return light;
}

static uint64_t hash_bytes(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static int dyn_mask(const dyn_arrays &a, int lines)
{
    return (a.uv.is_empty() ? 0 : DA_UV) | (a.uv2.is_empty() ? 0 : DA_UV2) | (a.col.is_empty() ? 0 : DA_COLOR) |
           (a.ind.is_empty() ? 0 : DA_INDEX) | (lines ? DA_LINES : 0);
}

static inline uint8_t unorm8(float f)
{
    double d = (double)f * 255.0;
    return (uint8_t)((d < 0.0) ? 0.0 : ((d > 255.0) ? 255.0 : d));
}

// Godot's vertex and attribute buffers for these arrays
static void dyn_encode(int64_t format, const dyn_arrays &a, PackedByteArray &vb, PackedByteArray &ab)
{
    RenderingServer *rs = RenderingServer::get_singleton();
    BitField<RenderingServer::ArrayFormat> f = (BitField<RenderingServer::ArrayFormat>)format;
    int32_t n = (int32_t)a.v.size();
    uint32_t vs = rs->mesh_surface_get_format_vertex_stride(f, n), as = rs->mesh_surface_get_format_attribute_stride(f, n);
    uint32_t ov = rs->mesh_surface_get_format_offset(f, n, Mesh::ARRAY_VERTEX);
    uint32_t ocol = rs->mesh_surface_get_format_offset(f, n, Mesh::ARRAY_COLOR);
    uint32_t ouv = rs->mesh_surface_get_format_offset(f, n, Mesh::ARRAY_TEX_UV), ouv2 = rs->mesh_surface_get_format_offset(f, n, Mesh::ARRAY_TEX_UV2);
    vb.resize((int64_t)n * vs);
    ab.resize((int64_t)n * as);
    uint8_t *pv = vb.ptrw(), *pa = ab.ptrw();
    const Vector3 *V = a.v.ptr();
    const Vector2 *U = a.uv.is_empty() ? NULL : a.uv.ptr(), *U2 = a.uv2.is_empty() ? NULL : a.uv2.ptr();
    const Color *C = a.col.is_empty() ? NULL : a.col.ptr();
    for (int32_t i = 0; i < n; i++)
    {
        float p[3] = { (float)V[i].x, (float)V[i].y, (float)V[i].z };
        memcpy(pv + (size_t)i * vs + ov, p, sizeof(p));
        uint8_t *at = pa + (size_t)i * as;
        if (C != NULL)
        {
            uint8_t c[4] = { unorm8(C[i].r), unorm8(C[i].g), unorm8(C[i].b), unorm8(C[i].a) };
            memcpy(at + ocol, c, sizeof(c));
        }
        if (U != NULL) { float t[2] = { (float)U[i].x, (float)U[i].y }; memcpy(at + ouv, t, sizeof(t)); }
        if (U2 != NULL) { float t[2] = { (float)U2[i].x, (float)U2[i].y }; memcpy(at + ouv2, t, sizeof(t)); }
    }
}

// can the surfaces with these arrays be updated in place? (once per combination: our encoding of the surface Godot
// just made against Godot's)
static int dyn_layout_ok(const Ref<ArrayMesh> &mesh, int surface, int mask, const dyn_arrays &a)
{
    static std::map<int, int> checked;
    auto it = checked.find(mask);
    if (it != checked.end()) return it->second;
    static const char *off = getenv("AVP2_R3D_NOINPLACE");
    int ok = 0;
    if (off == NULL)
    {
        Dictionary d = RenderingServer::get_singleton()->mesh_get_surface(mesh->get_rid(), surface);
        PackedByteArray vb, ab, gvb = d["vertex_data"], gab = d["attribute_data"];
        dyn_encode((int64_t)mesh->surface_get_format(surface), a, vb, ab);
        ok = (vb == gvb) && (ab == gab);
        if (winapi_debug || !ok)
            eprintf("render3d: dynamic meshes with arrays %02x: %s (vertex data %lld/%lld bytes, attributes %lld/%lld)\n", mask,
                    ok ? "updated in place" : "rebuilt (the data layout isn't Godot's)", (long long)vb.size(), (long long)gvb.size(),
                    (long long)ab.size(), (long long)gab.size());
    }
    checked[mask] = ok;
    return ok;
}

static uint64_t dyn_index_hash(const dyn_arrays &a)
{
    return a.ind.is_empty() ? 0 : hash_bytes(0xcbf29ce484222325ull, a.ind.ptr(), (size_t)a.ind.size() * sizeof(int32_t));
}

static AABB dyn_bounds(const std::vector<dyn_part> &parts)
{
    AABB box;
    int first = 1;
    for (const dyn_part &p : parts)
    {
        const Vector3 *V = p.a->v.ptr();
        for (int64_t i = 0, n = p.a->v.size(); i < n; i++)
        {
            if (first) { box = AABB(V[i], Vector3()); first = 0; }
            else box.expand_to(V[i]);
        }
    }
    return box;
}

enum { DYN_MODEL, DYN_PARTICLES, DYN_LINES, DYN_POLYGRID, DYN_CANVAS, DYN_KINDS };
static struct { uint64_t in_place[DYN_KINDS], rebuilt[DYN_KINDS]; } dyn_stats;

// the mesh's surfaces: these parts (empty ones left out by the caller); returns the vertices' bounding box
static AABB dyn_update(int kind, const Ref<ArrayMesh> &mesh, std::vector<dyn_surface> &state, const std::vector<dyn_part> &parts, int lines = 0)
{
    RenderingServer *rs = RenderingServer::get_singleton();
    int same = (state.size() == parts.size()) && (mesh->get_surface_count() == (int32_t)parts.size());
    for (size_t i = 0; same && (i < parts.size()); i++)
    {
        const dyn_arrays &a = *parts[i].a;
        const dyn_surface &s = state[i];
        RID m = parts[i].material.is_valid() ? parts[i].material->get_rid() : RID();
        same = (s.mask == dyn_mask(a, lines)) && (s.count == a.v.size()) && (s.index_count == a.ind.size()) && (s.material == m) &&
               (s.index_hash == dyn_index_hash(a)) && dyn_layout_ok(mesh, (int)i, s.mask, a);
    }
    if (same)
    {
        static PackedByteArray vb, ab;
        for (size_t i = 0; i < parts.size(); i++)
        {
            dyn_encode(state[i].format, *parts[i].a, vb, ab);
            rs->mesh_surface_update_vertex_region(mesh->get_rid(), (int32_t)i, 0, vb);
            if (!ab.is_empty()) rs->mesh_surface_update_attribute_region(mesh->get_rid(), (int32_t)i, 0, ab);
        }
        dyn_stats.in_place[kind]++;
        return dyn_bounds(parts);
    }

    mesh->clear_surfaces();
    state.clear();
    for (size_t i = 0; i < parts.size(); i++)
    {
        const dyn_arrays &a = *parts[i].a;
        Array arrays;
        arrays.resize(Mesh::ARRAY_MAX);
        arrays[Mesh::ARRAY_VERTEX] = a.v;
        if (!a.uv.is_empty()) arrays[Mesh::ARRAY_TEX_UV] = a.uv;
        if (!a.uv2.is_empty()) arrays[Mesh::ARRAY_TEX_UV2] = a.uv2;
        if (!a.col.is_empty()) arrays[Mesh::ARRAY_COLOR] = a.col;
        if (!a.ind.is_empty()) arrays[Mesh::ARRAY_INDEX] = a.ind;
        mesh->add_surface_from_arrays(lines ? Mesh::PRIMITIVE_LINES : Mesh::PRIMITIVE_TRIANGLES, arrays);
        mesh->surface_set_material((int32_t)i, parts[i].material);
        dyn_surface s;
        s.mask = dyn_mask(a, lines);
        s.count = a.v.size();
        s.index_count = a.ind.size();
        s.index_hash = dyn_index_hash(a);
        s.material = parts[i].material.is_valid() ? parts[i].material->get_rid() : RID();
        s.format = (int64_t)mesh->surface_get_format((int32_t)i);
        state.push_back(s);
        dyn_layout_ok(mesh, (int)i, s.mask, a);
    }
    dyn_stats.rebuilt[kind]++;
    return dyn_bounds(parts);
}

static void draw_model(uint32_t mw, uint32_t sd, uint32_t o, const Transform3D &cam, const std::vector<uint32_t> &lights)
{
    uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
    uint32_t model = g32(o + MI_MODEL), vt = g32(o);
    if ((model == 0) || (vt == 0)) return;
    uint32_t nnodes = g32(model + MD_TRANSFORMS + 8);
    uint32_t pieces = g32(model + MD_PIECES + 4), npieces = g32(model + MD_PIECES + 8);
    if ((pieces == 0) || (npieces == 0) || (npieces > 64) || (nnodes == 0) || (nnodes > 512)) return;
    int close = (flags & FLAG_REALLYCLOSE) != 0;

    // where it is (really close objects: in the camera's space)
    Transform3D xf = lt_object_transform(o);
    Transform3D world = close ? cam * xf : xf;
    if (!close)
    {
        // behind the camera or too far: not drawn
        Vector3 d = world.origin - cam.origin;
        float r = 256.0f * fmaxf(1.0f, fabsf(gf(o + OB_SCALE)));
        if ((d.dot(-cam.basis.get_column(2)) < -r) || (d.length() > 12000.0f)) return;
    }

    uint32_t args[1] = { o };
    uint32_t tr = call_game_this(g32(vt + MI_VT_GETTRANSFORMS), 1, args);
    if (tr == 0) return;

    // light (ModelDraw::SetupModelLight), in model space
    float objc[3] = { g8(o + OB_COLOR) / 255.0f, g8(o + OB_COLOR + 1) / 255.0f, g8(o + OB_COLOR + 2) / 255.0f };
    uint8_t alpha = g8(o + OB_COLOR + 3);
    float add[3] = { gf(sd + SD_MODELLIGHTADD), gf(sd + SD_MODELLIGHTADD + 4), gf(sd + SD_MODELLIGHTADD + 8) };
    float tint[3] = { gf(sd + SD_VERTEXTINT), gf(sd + SD_VERTEXTINT + 4), gf(sd + SD_VERTEXTINT + 8) };
    float base[3], lit[3], dir[3] = { 0, 0, 0 };
    std::vector<model_light> ml;
    Transform3D inv_world = world.affine_inverse();     // Godot world -> model (Godot axes)
    if (flags & FLAG_NOLIGHT)
    {
        for (int c = 0; c < 3; c++) base[c] = lit[c] = 255.0f * objc[c] + add[c];
    }
    else
    {
        float amb[3], p[3] = { world.origin.x, world.origin.y, -world.origin.z };
        light_table(mw, p, amb);
        for (int c = 0; c < 3; c++) base[c] = amb[c] * tint[c] * objc[c] + add[c];
        // the sun (the world's directional light), as much of it as reaches the model; computed again when it moved
        // (FLAG2_DYNAMICDIRLIGHT) - the original keeps it in the instance the same way
        const float *sc = render_struct ? (const float *)(render_struct + RS_LIGHTCOLOR) : NULL;
        for (int c = 0; c < 3; c++) lit[c] = base[c];
        if ((sc != NULL) && ((sc[0] != 0.0f) || (sc[1] != 0.0f) || (sc[2] != 0.0f)) && ((tint[0] != 0.0f) || (tint[1] != 0.0f) || (tint[2] != 0.0f)))
        {
            model_instance &cache = gs->models[o];
            if ((cache.sun < 0.0f) || ((flags2 & FLAG2_DYNAMICDIRLIGHT) &&
                ((fabsf(cache.sun_pos[0] - p[0]) > 0.051f) || (fabsf(cache.sun_pos[1] - p[1]) > 0.051f) || (fabsf(cache.sun_pos[2] - p[2]) > 0.051f))))
            {
                Vector3 r = world.basis.get_column(0);
                float right[3] = { r.x, r.y, -r.z };
                float rlen = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
                if (rlen > 0.0f) for (int i = 0; i < 3; i++) right[i] /= rlen;
                cache.sun = sun_amount(mw, p, right, gf(model + MD_GLOBALRADIUS) * fabsf(gf(o + OB_SCALE)));
                memcpy(cache.sun_pos, p, sizeof(p));
            }
            if (cache.sun > 0.0f)
            {
                // the light's direction in the model's space, towards the light
                const float *ld = (const float *)(render_struct + RS_LIGHTDIR);
                Vector3 m = inv_world.basis.xform(Vector3(ld[0], ld[1], -ld[2]));
                dir[0] = -m.x * cache.sun; dir[1] = -m.y * cache.sun; dir[2] = m.z * cache.sun;
                for (int c = 0; c < 3; c++) lit[c] = sc[c] * tint[c] * objc[c] + add[c];
            }
        }
        // dynamic lights that light objects
        for (uint32_t l : lights)
        {
            if (ml.size() >= 16) break;
            Vector3 lp = lt_pos(l + OB_POS);
            float radius = gf(l + DL_RADIUS);
            if (!(radius > 0.0f) || ((lp - world.origin).length() - 128.0f > radius)) continue;
            Vector3 m = inv_world.xform(lp);    // in the model's space (Godot axes) -> LithTech's
            model_light L;
            L.pos[0] = m.x; L.pos[1] = m.y; L.pos[2] = -m.z;
            L.radius2 = radius * radius;
            float len = sqrtf(L.pos[0] * L.pos[0] + L.pos[1] * L.pos[1] + L.pos[2] * L.pos[2]);
            for (int c = 0; c < 3; c++) L.dir[c] = ((len > 0.0f) ? L.pos[c] / len : 0.0f) / L.radius2;
            L.color[0] = g8(l + OB_COLOR) * tint[0] * objc[0];
            L.color[1] = g8(l + OB_COLOR + 1) * tint[1] * objc[1];
            L.color[2] = g8(l + OB_COLOR + 2) * tint[2] * objc[2];
            ml.push_back(L);
        }
    }

    model_instance &mi = gs->models[o];
    if (mi.mi == NULL)
    {
        mi.mi = memnew(MeshInstance3D);
        mi.mesh.instantiate();
        mi.mi->set_mesh(mi.mesh);
        (close ? (Node *)gs->camera : (Node *)gs->root)->add_child(mi.mi);
        mi.close = close;
    }
    else if (mi.close != close)
    {
        mi.mi->get_parent()->remove_child(mi.mi);
        (close ? (Node *)gs->camera : (Node *)gs->root)->add_child(mi.mi);
        mi.close = close;
    }
    // a really close model with a FOV offset (the player-view weapon: Model m_bFovOffset +0x140, offsets +0x144/+0x148)
    // gets a projection of its own in d3d.ren (SetupModel): its FOV is ExtraFOVXOffset/YOffset degrees + the model's
    // offsets + the view's FovX/FovY (the game sets the variables to ViewModelBaseFOVX/Y - the view's: 95 x 115 degrees)
    // and its clip transform is diag(tan(its FOV / 2)) x the shear - in place of the view's mFOVScale, diag(1 / tan(the
    // view's FOV / 2)) (the game's d3d.ren 0x1001af52: mProj x m_mShear, where other really close models get
    // m_mReallyCloseClipTransform). So it's scaled by tan(weapon / 2) x tan(view / 2) against the view's projection -
    // in m1s1 x 1.09 wider and x 1.27 taller (the knife's blade angle then matches the Wine reference: 27 degrees).
    // AVP2_R3D_WEAPONFOV=0: off.
    Transform3D placed = xf;
    static const char *weapon_fov = getenv("AVP2_R3D_WEAPONFOV");
    if (close && g32(model + 0x140) && !((weapon_fov != NULL) && (strcmp(weapon_fov, "0") == 0)))
    {
        float vfx = gf(sd + SD_XFOV), vfy = gf(sd + SD_YFOV);
        // (with camera.c's Hor+ the game's own FovX: the weapon keeps its 4:3 size on a wide screen)
        float hp = camera_hor_plus();
        if ((hp != 1.0f) && (vfx > 0.01f) && (vfx < 3.1f)) vfx = 2.0f * atanf(tanf(vfx * 0.5f) / hp);
        if ((vfx > 0.01f) && (vfx < 3.1f) && (vfy > 0.01f) && (vfy < 3.1f))
        {
            float wfx = cvar("ExtraFOVXOffset", 0) * 0.017453292f + gf(model + 0x144) + vfx;
            float wfy = cvar("ExtraFOVYOffset", 0) * 0.017453292f + gf(model + 0x148) + vfy;
            wfx = fminf(fmaxf(wfx, 0.17453292f), 2.9670596f);
            wfy = fminf(fmaxf(wfy, 0.17453292f), 2.9670596f);
            float kx = tanf(wfx * 0.5f) * tanf(vfx * 0.5f), ky = tanf(wfy * 0.5f) * tanf(vfy * 0.5f);
            placed = Transform3D(Basis().scaled(Vector3(kx, ky, 1.0f)), Vector3()) * xf;
        }
    }
    mi.mi->set_transform(placed);
    mi.frame = gs->frame;
    if (!mi.shown) { mi.mi->set_visible(true); mi.shown = 1; }

    // rebuilt only when the pose, the light, the skins or the hidden pieces changed
    uint64_t key = 0xcbf29ce484222325ull;
    key = hash_bytes(key, (const void *)G2H(tr), (size_t)nnodes * 0x40);
    key = hash_bytes(key, base, sizeof(base));
    key = hash_bytes(key, (const void *)G2H(o + MI_SKINS), 16);
    key = hash_bytes(key, (const void *)G2H(o + MI_HIDDENPIECES), 4);
    key = hash_bytes(key, &alpha, 1);
    key = hash_bytes(key, &flags2, 4);
    key = hash_bytes(key, &model, 4);
    for (int t = 0; t < 4; t++)
    {
        // the skins' state changes (vision modes switch their blending)
        uint32_t sk = g32(o + MI_SKINS + 4 * t);
        int b = sk ? get_texture(sk).state.blend : 0;
        key = hash_bytes(key, &b, sizeof(b));
    }
    for (const model_light &L : ml) key = hash_bytes(key, &L, sizeof(L));
    if ((key == mi.key) && (mi.mesh->get_surface_count() > 0)) return;
    mi.key = key;

    // a surface per skin
    // (kept between calls: the arrays keep their capacity)
    static dyn_arrays s[4];
    for (int t = 0; t < 4; t++) { s[t].v.clear(); s[t].uv.clear(); s[t].col.clear(); }
    uint32_t hidden = g32(o + MI_HIDDENPIECES);
    static std::vector<float> pos, col;
    for (uint32_t pi = 0; pi < npieces; pi++)
    {
        if ((pi < 32) && (hidden & (1u << pi))) continue;
        uint32_t pc = g32(pieces + 4 * pi);
        if (pc == 0) continue;
        uint32_t verts = g32(pc + PC_VERTS + 4), nverts = g32(pc + PC_VERTS + 8);
        uint32_t tris = g32(pc + PC_TRIS + 4), ntris = g32(pc + PC_TRIS + 8);
        uint32_t tex = g32(pc + PC_TEXTURE);
        if (tex > 3) tex = 3;
        if ((verts == 0) || (tris == 0) || (nverts > 65536)) continue;
        pos.resize((size_t)nverts * 3);
        col.resize((size_t)nverts * 3);
        for (uint32_t i = 0; i < nverts; i++)
        {
            uint32_t v = verts + i * 0x20;
            // skinned: the sum of the weighted node-space offsets through their nodes, over the weight sum
            float acc[4] = { 0, 0, 0, 0 };
            uint32_t w = g32(v), nw = g16(v + 4);
            for (uint32_t k = 0; k < nw; k++, w += 0x14)
            {
                uint32_t node = g32(w + 0x10);
                if (node >= nnodes) continue;
                uint32_t m = tr + node * 0x40;
                float x = gf(w), y = gf(w + 4), z = gf(w + 8), ww = gf(w + 12);
                acc[0] += gf(m) * x + gf(m + 4) * y + gf(m + 8) * z + gf(m + 12) * ww;
                acc[1] += gf(m + 16) * x + gf(m + 20) * y + gf(m + 24) * z + gf(m + 28) * ww;
                acc[2] += gf(m + 32) * x + gf(m + 36) * y + gf(m + 40) * z + gf(m + 44) * ww;
                acc[3] += gf(m + 48) * x + gf(m + 52) * y + gf(m + 56) * z + gf(m + 60) * ww;
            }
            float rw = (acc[3] != 0.0f) ? 1.0f / acc[3] : 1.0f;
            pos[i * 3] = acc[0] * rw; pos[i * 3 + 1] = acc[1] * rw; pos[i * 3 + 2] = acc[2] * rw;
            // lit in the rest pose (rest position and normal)
            float n[3] = { gf(v + 0x14), gf(v + 0x18), gf(v + 0x1c) }, rp[3] = { gf(v + 8), gf(v + 12), gf(v + 16) };
            float c[3] = { base[0], base[1], base[2] };
            float d = dir[0] * n[0] + dir[1] * n[1] + dir[2] * n[2];
            if (d > 0.0f) for (int j = 0; j < 3; j++) c[j] = base[j] + (lit[j] - base[j]) * d;
            for (const model_light &L : ml)
            {
                float ld = L.dir[0] * n[0] + L.dir[1] * n[1] + L.dir[2] * n[2];
                if (ld <= 0.0f) continue;
                float dx = rp[0] - L.pos[0], dy = rp[1] - L.pos[1], dz = rp[2] - L.pos[2];
                float dist2 = dx * dx + dy * dy + dz * dz;
                if (dist2 < L.radius2) for (int j = 0; j < 3; j++) c[j] += L.color[j] * (L.radius2 - dist2) * ld;
            }
            for (int j = 0; j < 3; j++) col[i * 3 + j] = (c[j] > 255.0f) ? 1.0f : ((c[j] < 0.0f) ? 0.0f : c[j] / 255.0f);
        }
        dyn_arrays &S = s[tex];
        for (uint32_t t = 0; t < ntris; t++)
        {
            uint32_t tri = tris + t * 0x20;
            static const int order[3] = { 0, 2, 1 };    // the z flip mirrors the winding
            uint32_t idx[3] = { g16(tri), g16(tri + 2), g16(tri + 4) };
            if ((idx[0] >= nverts) || (idx[1] >= nverts) || (idx[2] >= nverts)) continue;
            for (int j : order)
            {
                uint32_t k = idx[j];
                S.v.push_back(Vector3(pos[k * 3], pos[k * 3 + 1], -pos[k * 3 + 2]));
                S.uv.push_back(Vector2(gf(tri + 8 + j * 8), gf(tri + 12 + j * 8)));
                S.col.push_back(Color(col[k * 3], col[k * 3 + 1], col[k * 3 + 2], alpha / 255.0f));
            }
        }
    }
    if (winapi_debug && close)
    {
        static uint32_t reported;
        if (reported != model)
        {
            reported = model;
            AABB box;
            int first = 1;
            for (int t = 0; t < 4; t++) for (int64_t i = 0; i < s[t].v.size(); i++) { if (first) box = AABB(s[t].v[i], Vector3()); else box.expand_to(s[t].v[i]); first = 0; }
            eprintf("render3d: light base %.1f %.1f %.1f add %.1f %.1f %.1f tint %.2f %.2f %.2f, %zu lights, sun %.2f %.2f %.2f dir %.2f %.2f %.2f, scene light add %.2f %.2f %.2f, scale %.2f %.2f %.2f, global light %u\n",
                    base[0], base[1], base[2], add[0], add[1], add[2], tint[0], tint[1], tint[2], ml.size(),
                    render_struct ? gf(H2G(render_struct) + RS_LIGHTCOLOR) : 0.0f, render_struct ? gf(H2G(render_struct) + RS_LIGHTCOLOR + 4) : 0.0f,
                    render_struct ? gf(H2G(render_struct) + RS_LIGHTCOLOR + 8) : 0.0f, render_struct ? gf(H2G(render_struct) + RS_LIGHTDIR) : 0.0f,
                    render_struct ? gf(H2G(render_struct) + RS_LIGHTDIR + 4) : 0.0f, render_struct ? gf(H2G(render_struct) + RS_LIGHTDIR + 8) : 0.0f,
                    gf(sd + 0x74), gf(sd + 0x78), gf(sd + 0x7c), gf(sd + 0x50), gf(sd + 0x54), gf(sd + 0x58), render_struct ? g32(H2G(render_struct) + 0x150) : 0);
            eprintf("render3d: ExtraFOVXOffset %.3f ExtraFOVYOffset %.3f ModelFovTest %.0f, view fov %.3f %.3f\n", cvar("ExtraFOVXOffset", -999), cvar("ExtraFOVYOffset", -999),
                    cvar("ModelFovTest", -1), gf(sd + SD_XFOV), gf(sd + SD_YFOV));
            eprintf("render3d: really close model \"%s\" fov offset %u %.3f %.3f, box %.1f %.1f %.1f - %.1f %.1f %.1f, at %.2f %.2f %.2f\n", g32(model + 4) ? (const char *)G2H(g32(model + 4)) : "?",
                    g32(model + 0x140), gf(model + 0x144), gf(model + 0x148), box.position.x, box.position.y, box.position.z, box.get_end().x, box.get_end().y, box.get_end().z,
                    xf.origin.x, xf.origin.y, xf.origin.z);
        }
    }
    // debugging: AVP2_R3D_DUMPMODEL=<file name part> logs those models when they are rebuilt (box in model space)
    static const char *dump_model = getenv("AVP2_R3D_DUMPMODEL");
    if ((dump_model != NULL) && g32(model + 4) && strstr((const char *)G2H(g32(model + 4)), dump_model))
    {
        AABB box;
        int first = 1, nv = 0;
        for (int t = 0; t < 4; t++) for (int64_t i = 0; i < s[t].v.size(); i++) { if (first) box = AABB(s[t].v[i], Vector3()); else box.expand_to(s[t].v[i]); first = 0; nv++; }
        eprintf("render3d: model %08x \"%s\" %d vertices, box %.1f %.1f %.1f - %.1f %.1f %.1f, at %.1f %.1f %.1f, camera %.1f %.1f %.1f, alpha %u flags %08x/%08x skins %08x %08x\n", o,
                (const char *)G2H(g32(model + 4)), nv, box.position.x, box.position.y, box.position.z, box.get_end().x, box.get_end().y, box.get_end().z,
                xf.origin.x, xf.origin.y, xf.origin.z, cam.origin.x, cam.origin.y, cam.origin.z, alpha, flags, flags2, g32(o + MI_SKINS), g32(o + MI_SKINS + 4));
        for (int t = 0; t < 4; t++)
            if (!s[t].v.is_empty())
            {
                texture_entry &te = get_texture(g32(o + MI_SKINS + 4 * t));
                eprintf("render3d:   skin %d: %08x %s has alpha %d, %lld vertices\n", t, g32(o + MI_SKINS + 4 * t), te.texture.is_null() ? "(no texture)" : "", te.has_alpha, (long long)s[t].v.size());
            }
    }
    std::vector<dyn_part> parts;
    for (int t = 0; t < 4; t++)
    {
        if (s[t].v.is_empty()) continue;
        uint32_t skin = g32(o + MI_SKINS + 4 * t);
        // d3d.ren alpha tests a model only with FLAG2_CHROMAKEY (d3d_DrawChromaKeyModels: ALPHAFUNC GREATER); a
        // solid model ignores its texture's alpha (a1s2's bursting chest skins have alpha and must be opaque)
        int kind = MAT_OPAQUE;
        const texture_entry &ste = get_texture(skin);
        if (flags2 & FLAG2_ADDITIVE) kind = MAT_ADD;
        else if (alpha < 255) kind = MAT_BLEND;
        else if ((flags2 & FLAG2_CHROMAKEY) && ste.has_alpha) kind = MAT_ALPHA_TEST;
        // the skin's state change (vision modes) may blend it: Hunting's NavAliens (INVSRCCOLOR), ONE ONE, alpha
        if (ste.state.blend == 2) kind = MAT_ADD;
        else if ((ste.state.blend == 1) || (ste.state.blend == 3)) kind = MAT_BLEND;
        if (close) kind |= MAT_REALLYCLOSE;
        parts.push_back({ &s[t], get_material(skin, kind, -1, 0) });
    }
    // the mesh's AABB isn't updated in place: the instance's
    AABB box = dyn_update(DYN_MODEL, mi.mesh, mi.dyn, parts);
    if (!parts.empty()) mi.mi->set_custom_aabb(box.grow(1.0f));
}

// Objects attached to others (a marine's rifle and shoulder lamp) are placed by the engine when the renderer asks:
// RenderStruct::ProcessAttachment(parent, attachment) moves the child to its socket and returns it
// (d3d_ProcessAttachments, recursively, for the objects that are drawn)
#define OB_ATTACHMENTS 0x98
#define AT_NEXT 0x24
#define RS_PROCESSATTACHMENT 0x00

static void process_attachments(uint32_t o, int depth)
{
    if ((depth > 32) || (render_struct == NULL)) return;
    for (uint32_t a = g32(o + OB_ATTACHMENTS), n = 0; (a != 0) && (n < 64); a = g32(a + AT_NEXT), n++)
    {
        uint32_t args[2] = { o, a };
        uint32_t child = call_game(*(uint32_t *)(render_struct + RS_PROCESSATTACHMENT), 2, args);
        if ((child != 0) && (child != o)) process_attachments(child, depth + 1);
    }
}

static void update_attachments(uint32_t mw)
{
    for (int t = 0; t < NUM_OBJECTTYPES; t++)
    {
        for_objects(mw, t, [&](uint32_t o) {
            uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
            if (!(flags & FLAG_VISIBLE) || (flags2 & FLAG2_SKYOBJECT) || (g32(o + OB_SD) != 0)) return;
            if (g32(o + OB_ATTACHMENTS)) process_attachments(o, 0);
        });
    }
}

// the dynamic lights for the world's shader (the nearest DL_MAX that light the world)
#define FLAG_ONLYLIGHTOBJECTS (1 << 7)

static void update_world_lights(uint32_t mw)
{
    if (dl_image.is_null()) return;
    Vector3 cam = gs->camera->get_transform().origin;
    std::vector<std::pair<float, uint32_t>> lights;
    for_objects(mw, OT_LIGHT, [&](uint32_t l) {
        uint32_t flags = g32(l + OB_FLAGS);
        float r = gf(l + DL_RADIUS);
        if (!(flags & FLAG_VISIBLE) || (flags & FLAG_ONLYLIGHTOBJECTS) || !(r > 0.0f)) return;
        if ((g8(l + OB_COLOR) == 0) && (g8(l + OB_COLOR + 1) == 0) && (g8(l + OB_COLOR + 2) == 0)) return;
        float d = (lt_pos(l + OB_POS) - cam).length() - r * 1.25f;
        if (d < 8000.0f) lights.push_back(std::make_pair(d, l));
    });
    std::sort(lights.begin(), lights.end());
    int n = (lights.size() > DL_MAX) ? DL_MAX : (int)lights.size();
    for (int i = 0; i < n; i++)
    {
        uint32_t l = lights[i].second;
        Vector3 p = lt_pos(l + OB_POS);
        dl_image->set_pixel(i, 0, Color(p.x, p.y, p.z, gf(l + DL_RADIUS)));
        dl_image->set_pixel(i, 1, Color(g8(l + OB_COLOR), g8(l + OB_COLOR + 1), g8(l + OB_COLOR + 2), 0));
    }
    if (n > 0) dl_texture->update(dl_image);
    if ((winapi_debug >= 2) && ((gs->frame % 600) == 300))
        for (int i = 0; i < n; i++)
        {
            uint32_t l = lights[i].second;
            eprintf("render3d: world light %d: %08x at %.0f %.0f %.0f radius %.0f colour %u %u %u flags %08x\n", i, l, gf(l + OB_POS), gf(l + OB_POS + 4), gf(l + OB_POS + 8),
                    gf(l + DL_RADIUS), g8(l + OB_COLOR), g8(l + OB_COLOR + 1), g8(l + OB_COLOR + 2), g32(l + OB_FLAGS));
        }
    static const StringName p_light_count("lt_light_count");
    RenderingServer::get_singleton()->global_shader_parameter_set(p_light_count, Vector4((float)n, 0, 0, 0));
}

static void update_models(uint32_t mw, uint32_t sd)
{
    Transform3D cam = gs->camera->get_transform();
    // the dynamic lights that light objects
    std::vector<uint32_t> lights;
    for_objects(mw, OT_LIGHT, [&](uint32_t l) {
        if ((g32(l + OB_FLAGS) & FLAG_VISIBLE) && !(g32(l + OB_FLAGS) & FLAG_ONLYLIGHTWORLD)) lights.push_back(l);
    });
    for_objects(mw, OT_MODEL, [&](uint32_t o) {
        uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
        if (!(flags & FLAG_VISIBLE) || (flags2 & FLAG2_SKYOBJECT)) return;
        draw_model(mw, sd, o, cam, lights);
    });
    for (auto &kv : gs->models)
    {
        model_instance &mi = kv.second;
        if ((mi.frame != gs->frame) && mi.shown) { mi.mi->set_visible(false); mi.shown = 0; }
    }
}

/* ------------------------------------------------------------------ */
/* sprites                                                             */

// SpriteInstance: its SpriteTracker (+0x1b0: sprite, current animation, current frame (SpriteEntry*: the texture))
#define SP_CURFRAME 0x1b8

#define FLAG_SPRITEBIAS (1 << 2)
#define FLAG_ROTATEABLESPRITE (1 << 3)
#define FLAG_SPRITE_NOZ (1 << 4)
#define FLAG_GLOWSPRITE (1 << 5)
#define FLAG_FOGDISABLE (1 << 7)
#define FLAG2_MULTIPLY (1 << 3)

// shader variants of the sprites (and particles)
enum
{
    SPR_ADD = 1, SPR_MUL = 2,       // else alpha blended
    SPR_NOZ = 4,                    // no depth test, over everything
    SPR_ROTATABLE = 8,              // placed by the object's rotation, else facing the camera
    SPR_GLOW = 16,                  // FLAG_GLOWSPRITE: smaller when near (10 .. 500 units: x0.1 .. x2)
    SPR_BIAS = 32,                  // FLAG_SPRITEBIAS: depth 20 units nearer
    SPR_CLOSE = 64,                 // FLAG_REALLYCLOSE
    SPR_PARTICLES = 128,            // a particle system: each quad's centre in its vertices, the corner offset in UV2
    SPR_NOFOG = 256,                // FLAG_FOGDISABLE
    SPR_MESH = 512,                 // a mesh as it is (poly grids)
};

// the sprite shaders: one per blending (alpha, add, multiply) and depth test (render modes); the rest of a variant
// (SPR_*: how the quad is placed, glow, bias, really close, fog) is the material's flags uniform, so that there are
// only 6 shaders to compile (preload_shaders) - not one per combination
#define SPR_SHADERS 6
static int sprite_shader_index(int v)
{
    return ((v & SPR_ADD) ? 1 : (v & SPR_MUL) ? 2 : 0) + ((v & SPR_NOZ) ? 3 : 0);
}

static Ref<Shader> sprite_shader(int v)
{
    static Ref<Shader> *shaders = new Ref<Shader>[SPR_SHADERS];
    int index = sprite_shader_index(v);
    if (shaders[index].is_valid()) return shaders[index];
    auto flag = [](int f) { return "((flags & " + std::to_string(f) + ") != 0)"; };
    std::string modes = "unshaded, cull_disabled, depth_draw_never";
    if (v & SPR_ADD) modes += ", blend_add";
    else if (v & SPR_MUL) modes += ", blend_mul";
    if (v & SPR_NOZ) modes += ", depth_test_disabled";
    std::string c = "shader_type spatial;\nrender_mode " + modes + ";\n"
        "uniform sampler2D albedo : filter_linear_mipmap, repeat_disable;\n"
        "uniform int flags = 0;\n"
        "instance uniform vec4 color = vec4(1.0);\n"
        "instance uniform vec2 half_size = vec2(1.0);\n" FOG_UNIFORMS STATE_UNIFORMS
        "vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n"
        "void vertex() {\n"
        "  vec3 ctr;\n"
        "  vec3 p;\n";
    // a mesh as it is; a particle system (each quad's centre in its vertices, the corner offset in UV2); a quad
    // placed by the object's rotation; a quad facing the camera (a glow sprite smaller when near)
    c += "  if (" + flag(SPR_MESH) + ") {\n"
         "    ctr = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;\n"
         "    p = ctr;\n"
         "  } else if (" + flag(SPR_PARTICLES) + ") {\n"
         "    ctr = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;\n"
         "    p = ctr + vec3(UV2, 0.0);\n"
         "  } else if (" + flag(SPR_ROTATABLE) + ") {\n"
         "    ctr = (MODELVIEW_MATRIX * vec4(0.0, 0.0, 0.0, 1.0)).xyz;\n"
         "    p = (MODELVIEW_MATRIX * vec4(VERTEX.xy * half_size, 0.0, 1.0)).xyz;\n"
         "  } else {\n"
         "    ctr = (MODELVIEW_MATRIX * vec4(0.0, 0.0, 0.0, 1.0)).xyz;\n"
         "    vec2 h = half_size;\n"
         "    if (" + flag(SPR_GLOW) + ") h *= 0.1 + 1.9 * clamp((-ctr.z - 10.0) / 490.0, 0.0, 1.0);\n"
         "    p = ctr + vec3(VERTEX.xy * h, 0.0);\n"
         "  }\n"
         "  POSITION = PROJECTION_MATRIX * vec4(p, 1.0);\n"
         "  if (" + flag(SPR_BIAS) + ") {\n"
         "    vec3 pb = p;\n    pb.z = -max(-ctr.z - 20.0, 1.0) + (p.z - ctr.z);\n"
         "    vec4 q = PROJECTION_MATRIX * vec4(pb, 1.0);\n    POSITION.z = q.z / q.w * POSITION.w;\n"
         "  }\n"
         "  if (" + flag(SPR_CLOSE) + ") POSITION.z = POSITION.w * (0.9 + 0.1 * clamp(POSITION.z / POSITION.w, 0.0, 1.0));\n"
         "}\n"
         "void fragment() {\n"
         "  vec4 t = texture(albedo, UV) * color * COLOR;\n"
         "  vec3 col = clamp(t.rgb, 0.0, 1.0);\n";
    // fog: additive fades to black, multiplied to white (d3d_DrawSprite's fog colours)
    c += "  if (!" + flag(SPR_NOFOG) + ") {\n"
         "    float f = lt_fog.x * clamp((length(VERTEX) - lt_fog.y) / max(lt_fog.z - lt_fog.y, 0.001), 0.0, 1.0);\n"
         "    if (lt_vfog.x > 0.5) f = lt_fog.x * lt_vfog_amount(CAMERA_POSITION_WORLD, (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz);\n"
         "    vec3 fc = lt_fog_color.rgb;\n"
         "    if (st_fog.x > 2.5) { f = clamp((-VERTEX.z - st_fog.y) / max(st_fog.z - st_fog.y, 0.001), 0.0, 1.0); fc = st_fog_color; }\n"
         "    else if (st_fog.x > 1.5) { f = 1.0; fc = st_fog_color; }\n"
         "    else if (st_fog.x > 0.5) f = 0.0;\n";
    c += (v & SPR_ADD) ? "    col *= 1.0 - f;\n" : (v & SPR_MUL) ? "    col = mix(col, vec3(1.0), f);\n" : "    col = mix(col, fc, f);\n";
    // additive: d3d.ren blends ONE, ONE (d3d_DrawSprite, the translucent object states) - the alpha (the texture's, the
    // object's: a marine's shoulder lamp glow is 9/255) isn't used, while Godot's blend_add multiplies by ALPHA
    c += "  }\n"
         "  ALBEDO = to_linear(col);\n";
    c += (v & SPR_ADD) ? "  ALPHA = 1.0;\n" : "  ALPHA = t.a;\n";
    c += "}\n";
    shaders[index] = make_shader("sprite", index, c);
    return shaders[index];
}

static Ref<ShaderMaterial> sprite_material(uint32_t tex, int v)
{
    uint64_t key = ((uint64_t)tex << 16) | (uint64_t)v;
    auto it = gs->sprite_materials.find(key);
    if (it != gs->sprite_materials.end()) return it->second;
    Ref<ShaderMaterial> m;
    m.instantiate();
    m->set_shader(sprite_shader(v));
    m->set_shader_parameter("flags", v);
    texture_entry &t = get_texture(tex);
    if (t.texture.is_valid()) m->set_shader_parameter("albedo", t.texture);
    gs->sprite_materials[key] = m;
    track_material(tex, m);
    return m;
}

// a quad (-1..1), UVs with v down
static Ref<ArrayMesh> unit_quad(void)
{
    static Ref<ArrayMesh> q;
    if (q.is_valid()) return q;
    PackedVector3Array v;
    PackedVector2Array uv;
    static const float c[6][2] = { { -1, 1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
    for (int i = 0; i < 6; i++)
    {
        v.push_back(Vector3(c[i][0], c[i][1], 0));
        uv.push_back(Vector2((c[i][0] + 1) * 0.5f, (1 - c[i][1]) * 0.5f));
    }
    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = v;
    arrays[Mesh::ARRAY_TEX_UV] = uv;
    q.instantiate();
    q->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
    return q;
}

// the colour of a sprite (d3d.ren's SpriteGetColor): its colour times the light grid unless FLAG_NOLIGHT
static Color sprite_color(uint32_t mw, uint32_t sd, uint32_t o)
{
    uint32_t flags = g32(o + OB_FLAGS);
    float tint[3] = { gf(sd + SD_VERTEXTINT), gf(sd + SD_VERTEXTINT + 4), gf(sd + SD_VERTEXTINT + 8) };
    float c[3];
    if (!(flags & FLAG_NOLIGHT))
    {
        float p[3] = { gf(o + OB_POS), gf(o + OB_POS + 4), gf(o + OB_POS + 8) }, lg[3];
        light_table(mw, p, lg);
        for (int i = 0; i < 3; i++) c[i] = g8(o + OB_COLOR + i) * tint[i] * (lg[i] / 255.0f);
    }
    else for (int i = 0; i < 3; i++) c[i] = g8(o + OB_COLOR + i) * tint[i];
    for (int i = 0; i < 3; i++) c[i] = (c[i] < 0) ? 0 : ((c[i] > 255) ? 1 : c[i] / 255.0f);
    return Color(c[0], c[1], c[2], g8(o + OB_COLOR + 3) / 255.0f);
}

static void draw_sprite(uint32_t mw, uint32_t sd, uint32_t o)
{
    uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
    uint32_t frame = g32(o + SP_CURFRAME);
    uint32_t tex = frame ? g32(frame) : 0;
    if (tex == 0) return;
    texture_entry &te = get_texture(tex);
    if (te.texture.is_null()) return;
    int close = (flags & FLAG_REALLYCLOSE) != 0;
    int v = 0;
    if (flags2 & FLAG2_ADDITIVE) v |= SPR_ADD;
    else if (flags2 & FLAG2_MULTIPLY) v |= SPR_MUL;
    if (flags & FLAG_SPRITE_NOZ) v |= SPR_NOZ;
    if (flags & FLAG_ROTATEABLESPRITE) v |= SPR_ROTATABLE;
    else
    {
        if (flags & FLAG_GLOWSPRITE) v |= SPR_GLOW;
        if (flags & FLAG_SPRITEBIAS) v |= SPR_BIAS;
    }
    if (close) v |= SPR_CLOSE;
    if (flags & FLAG_FOGDISABLE) v |= SPR_NOFOG;

    sprite_instance &si = gs->sprites[o];
    if (si.mi == NULL)
    {
        si.mi = memnew(MeshInstance3D);
        si.mi->set_mesh(unit_quad());
        (close ? (Node *)gs->camera : (Node *)gs->root)->add_child(si.mi);
        si.close = close;
    }
    else if (si.close != close)
    {
        si.mi->get_parent()->remove_child(si.mi);
        (close ? (Node *)gs->camera : (Node *)gs->root)->add_child(si.mi);
        si.close = close;
    }
    if ((si.tex != tex) || (si.variant != v))
    {
        si.mi->set_material_override(sprite_material(tex, v));
        si.tex = tex;
        si.variant = v;
    }
    // half the texture's size times the scale, in world units
    float hx = (1.0f / te.inv_w) * gf(o + OB_SCALE), hy = (1.0f / te.inv_h) * gf(o + OB_SCALE + 4);
    Transform3D xf;
    if (v & SPR_ROTATABLE)
    {
        Quaternion q(-gf(o + OB_ROTATION), -gf(o + OB_ROTATION + 4), gf(o + OB_ROTATION + 8), gf(o + OB_ROTATION + 12));
        if (q.length_squared() < 1e-6f) q = Quaternion();
        xf = Transform3D(Basis(q.normalized()), lt_pos(o + OB_POS));
    }
    else xf = Transform3D(Basis(), lt_pos(o + OB_POS));
    si.mi->set_transform(xf);
    float r = fmaxf(fabsf(hx), fabsf(hy)) * 2.0f + 1.0f;
    si.mi->set_custom_aabb(AABB(Vector3(-r, -r, -r), Vector3(2 * r, 2 * r, 2 * r)));
    static const StringName p_half_size("half_size"), p_color("color");
    si.mi->set_instance_shader_parameter(p_half_size, Vector2(hx, hy));
    si.mi->set_instance_shader_parameter(p_color, sprite_color(mw, sd, o));
    si.frame = gs->frame;
    if (!si.shown) { si.mi->set_visible(true); si.shown = 1; }
}

static void update_sprites(uint32_t mw, uint32_t sd)
{
    for_objects(mw, OT_SPRITE, [&](uint32_t o) {
        uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
        if (!(flags & FLAG_VISIBLE) || (flags2 & FLAG2_SKYOBJECT)) return;
        draw_sprite(mw, sd, o);
    });
    for (auto &kv : gs->sprites)
    {
        sprite_instance &si = kv.second;
        if ((si.frame != gs->frame) && si.shown) { si.mi->set_visible(false); si.shown = 0; }
    }
}

/* ------------------------------------------------------------------ */
/* particle systems                                                    */

// LTParticleSystem: the particles are a ring through a sentinel (m_ParticleHead), in the system's space
#define PS_HEAD 0x1b0
#define PS_TEXTURE 0x1f0
#define PS_SPRITEFRAME 0x204        // m_SpriteTracker.m_pCurFrame
#define PS_NPARTICLES 0x230
// PSParticle (0x3c bytes)
#define PP_COLOR 0x0c               // 0-255
#define PP_ALPHA 0x18               // 0-1
#define PP_SIZE 0x1c
#define PP_NEXT 0x20
#define PP_POS 0x24

static void draw_particles(uint32_t o)
{
    uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
    uint32_t n = g32(o + PS_NPARTICLES);
    if ((n == 0) || (n > 100000)) return;
    uint32_t tex = g32(o + PS_TEXTURE);
    if ((tex == 0) && g32(o + PS_SPRITEFRAME)) tex = g32(g32(o + PS_SPRITEFRAME));
    int close = (flags & FLAG_REALLYCLOSE) != 0;
    int v = SPR_PARTICLES;
    if (flags2 & FLAG2_ADDITIVE) v |= SPR_ADD;
    else if (flags2 & FLAG2_MULTIPLY) v |= SPR_MUL;
    if (close) v |= SPR_CLOSE;
    if (flags & FLAG_FOGDISABLE) v |= SPR_NOFOG;

    sprite_instance &si = gs->particles[o];
    if (si.mi == NULL)
    {
        si.mi = memnew(MeshInstance3D);
        si.mesh.instantiate();
        si.mi->set_mesh(si.mesh);
        (close ? (Node *)gs->camera : (Node *)gs->root)->add_child(si.mi);
        si.close = close;
    }
    else if (si.close != close)
    {
        si.mi->get_parent()->remove_child(si.mi);
        (close ? (Node *)gs->camera : (Node *)gs->root)->add_child(si.mi);
        si.close = close;
    }
    si.mi->set_transform(lt_object_transform(o));

    // the quads: the centre in each vertex, the corner offset (view space, 2 x the particle's size) in UV2. Room for
    // a number of particles that changes only now and then (a power of two, kept while the particles fill more than
    // a quarter of it): the surface is updated in place (dyn_update) as the count changes; the rest are empty quads
    float sr = g8(o + OB_COLOR) / 255.0f, sg = g8(o + OB_COLOR + 1) / 255.0f, sb = g8(o + OB_COLOR + 2) / 255.0f, sa = g8(o + OB_COLOR + 3) / 255.0f;
    uint32_t room = si.dyn.empty() ? 0 : (uint32_t)(si.dyn[0].count / 6);
    if ((n > room) || (n * 4 < room))
    {
        room = 16;
        while (room < n) room *= 2;
    }
    static dyn_arrays a;
    a.v.resize((int64_t)room * 6);
    a.uv.resize((int64_t)room * 6);
    a.uv2.resize((int64_t)room * 6);
    a.col.resize((int64_t)room * 6);
    Vector3 *P = a.v.ptrw();
    Vector2 *U = a.uv.ptrw(), *U2 = a.uv2.ptrw();
    Color *C = a.col.ptrw();
    static const float corner[6][2] = { { -1, 1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
    uint32_t head = o + PS_HEAD, k = 0;
    AABB box;
    float maxsize = 0.0f;
    for (uint32_t p = g32(head + PP_NEXT); (p != head) && (p != 0) && (k < n); p = g32(p + PP_NEXT))
    {
        Vector3 c = lt_pos(p + PP_POS);
        float h = 2.0f * gf(p + PP_SIZE);
        Color cc(sr * gf(p + PP_COLOR) / 255.0f, sg * gf(p + PP_COLOR + 4) / 255.0f, sb * gf(p + PP_COLOR + 8) / 255.0f, sa * gf(p + PP_ALPHA));
        for (int j = 0; j < 6; j++)
        {
            int i = k * 6 + j;
            P[i] = c;
            U[i] = Vector2((corner[j][0] + 1) * 0.5f, (1 - corner[j][1]) * 0.5f);
            U2[i] = Vector2(corner[j][0] * h, corner[j][1] * h);
            C[i] = cc;
        }
        if (k == 0) box = AABB(c, Vector3());
        else box.expand_to(c);
        if (h > maxsize) maxsize = h;
        k++;
    }
    // the rest: quads of size 0 (no pixels), transparent, at the first particle
    Vector3 rest = (k > 0) ? P[0] : Vector3();
    for (uint32_t i = k * 6; i < room * 6; i++)
    {
        P[i] = rest;
        U[i] = Vector2();
        U2[i] = Vector2();
        C[i] = Color(0, 0, 0, 0);
    }
    std::vector<dyn_part> parts;
    if (k > 0) parts.push_back({ &a, sprite_material(tex, v) });
    (void) dyn_update(DYN_PARTICLES, si.mesh, si.dyn, parts);
    if (k > 0) si.mi->set_custom_aabb(box.grow(maxsize + 1.0f));
    si.frame = gs->frame;
    if (!si.shown) { si.mi->set_visible(true); si.shown = 1; }
}

static void update_particles(uint32_t mw)
{
    uint32_t nsys = 0, nparts = 0;
    for_objects(mw, OT_PARTICLESYSTEM, [&](uint32_t o) {
        uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
        if (!(flags & FLAG_VISIBLE) || (flags2 & FLAG2_SKYOBJECT)) return;
        draw_particles(o);
        if ((winapi_debug >= 2) && ((gs->frame % 900) == 400) && g32(o + PS_NPARTICLES))
        {
            uint32_t pp = g32(o + PS_HEAD + PP_NEXT);
            eprintf("render3d:   particles %08x flags %08x/%08x color %08x n %u texture %08x pos %.0f %.0f %.0f, first at %.1f %.1f %.1f size %.2f color %.0f %.0f %.0f alpha %.2f\n",
                    o, flags, flags2, g32(o + OB_COLOR), g32(o + PS_NPARTICLES), g32(o + PS_TEXTURE), gf(o + OB_POS), gf(o + OB_POS + 4), gf(o + OB_POS + 8),
                    gf(pp + PP_POS), gf(pp + PP_POS + 4), gf(pp + PP_POS + 8), gf(pp + PP_SIZE), gf(pp + PP_COLOR), gf(pp + PP_COLOR + 4), gf(pp + PP_COLOR + 8), gf(pp + PP_ALPHA));
        }
        nsys++;
        nparts += g32(o + PS_NPARTICLES);
    });
    if ((winapi_debug >= 2) && ((gs->frame % 300) == 10))
        eprintf("render3d: %u visible particle systems, %u particles; %zu sprites, %zu models\n", nsys, nparts, gs->sprites.size(), gs->models.size());
    for (auto &kv : gs->particles)
    {
        sprite_instance &si = kv.second;
        if ((si.frame != gs->frame) && si.shown) { si.mi->set_visible(false); si.shown = 0; }
    }
}

/* ------------------------------------------------------------------ */
/* the sky                                                             */

// SceneDesc
#define SD_SKYDEF 0x84              // box min, max, view min, view max
#define SD_SKYOBJECTS 0xb4
#define SD_NSKYOBJECTS 0xb8
// MainWorld
#define MW_EXTENTSMIN 0x144
#define MW_EXTENTSMAX 0x150

// The sky's objects (SceneDesc::m_SkyObjects: world models) seen from the sky camera, which is in the sky box
// where the viewer is in the world (d3d_SetupSkyStuff: ViewMin + (ViewMax - ViewMin) x the viewer's place in the
// world's extents) and turned like the camera: copies of their meshes, moved by (camera - sky camera), with
// materials at the far plane in the objects' order (d3d_DrawSkyObjects draws them without the z buffer, in order).
// The world covers them except where its sky polies (SURF_SKY, not drawn) are.
static void update_sky(uint32_t mw, uint32_t sd)
{
    int n = (int)g32(sd + SD_NSKYOBJECTS);
    uint32_t list = g32(sd + SD_SKYOBJECTS);
    Vector3 cam = gs->camera->get_transform().origin;
    float c[3] = { cam.x, cam.y, -cam.z }, sky[3];
    for (int i = 0; i < 3; i++)
    {
        float mn = gf(mw + MW_EXTENTSMIN + 4 * i), mx = gf(mw + MW_EXTENTSMAX + 4 * i);
        float f = (mx != mn) ? (c[i] - mn) / (mx - mn) : 0.5f;
        float vmin = gf(sd + SD_SKYDEF + 24 + 4 * i), vmax = gf(sd + SD_SKYDEF + 36 + 4 * i);
        sky[i] = vmin + (vmax - vmin) * f;
    }
    gs->sky_root->set_transform(Transform3D(Basis(), cam - Vector3(sky[0], sky[1], -sky[2])));

    for (int i = 0; (i < n) && (i < 200) && list; i++)
    {
        uint32_t o = g32(list + 4 * i);
        if ((o == 0) || !(g32(o + OB_FLAGS) & FLAG_VISIBLE) || (g8(o + OB_TYPE) != OT_WORLDMODEL)) continue;
        uint32_t bsp = g32(o + WM_ORIGINALBSP);
        auto mit = gs->bsp_meshes.find(bsp);
        if (mit == gs->bsp_meshes.end()) continue;
        wm_instance &wi = gs->sky_instances[o];
        if (wi.mi == NULL)
        {
            wi.mi = memnew(MeshInstance3D);
            gs->sky_root->add_child(wi.mi);
            wi.bsp = 0;
            // the sky is far from everything: never culled
            wi.mi->set_extra_cull_margin(16384.0f);
            if (winapi_debug >= 2)
            {
                std::map<uint64_t, int> seen;
                for (uint32_t p = 0; p < g32(bsp + WB_NPOLIES); p++)
                {
                    uint32_t poly = g32(g32(bsp + WB_POLIES) + 4 * p), surf = poly ? g32(poly + WP_SURFACE) : 0;
                    if (surf) seen[((uint64_t)g32(surf + SF_FLAGS) << 32) | g32(surf + SF_TEXTURE)]++;
                    if (p < 2)
                    {
                        uint32_t vv = g32(poly + WP_PVERTICES);
                        eprintf("render3d: sky %d poly %u vertex colours %08x %08x %08x uv %.1f %.1f\n", i, p, g32(vv + PV_COLOR), g32(vv + PV_SIZE + PV_COLOR), g32(vv + 2 * PV_SIZE + PV_COLOR),
                                gf(vv + PV_U), gf(vv + PV_V));
                    }
                }
                for (auto &e : seen)
                {
                    texture_entry &te = get_texture((uint32_t)e.first);
                    eprintf("render3d: sky %d \"%s\": %d polies surface flags %08x texture %08x (%.0f x %.0f, alpha %d)\n", i, (const char *)G2H(bsp + 4), e.second,
                            (uint32_t)(e.first >> 32), (uint32_t)e.first, 1.0f / te.inv_w, 1.0f / te.inv_h, te.has_alpha);
                }
            }
        }
        if ((wi.bsp != bsp) || (wi.alpha != (uint8_t)i))
        {
            wi.mi->set_mesh(mit->second);
            std::vector<uint64_t> &keys = gs->bsp_keys[bsp];
            for (size_t k = 0; (k < keys.size()) && ((int64_t)k < mit->second->get_surface_count()); k++)
                wi.mi->set_surface_override_material((int)k, sky_material(keys[k], i));
            wi.bsp = bsp;
            wi.alpha = (uint8_t)i;      // (its place in the sky's order)
        }
        wi.mi->set_transform(lt_matrix(o + WM_TRANSFORM));
        if (!wi.shown) { wi.mi->set_visible(true); wi.shown = 1; }
        wi.frame = gs->frame;
    }
    for (auto &kv : gs->sky_instances)
    {
        wm_instance &wi = kv.second;
        if ((wi.frame != gs->frame) && wi.shown) { wi.mi->set_visible(false); wi.shown = 0; }
    }
}

/* ------------------------------------------------------------------ */
/* line systems                                                        */

// LineSystem: a ring of LSLine (two points: position, r, g, b, a in 0-1; prev 0x3c, next 0x40) through m_LineHead
#define LS_HEAD 0x1b8
#define LL_NEXT 0x40

static Ref<ShaderMaterial> line_material(int close)
{
    static Ref<ShaderMaterial> *m = new Ref<ShaderMaterial>[2];     // never destroyed (see world_shader)
    if (m[close].is_null())
    {
        std::string c = "shader_type spatial;\nrender_mode unshaded, cull_disabled, depth_draw_never;\n"
                         "vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n";
        if (close)
            c += "void vertex() {\n"
                 "  POSITION = PROJECTION_MATRIX * (MODELVIEW_MATRIX * vec4(VERTEX, 1.0));\n"
                 "  POSITION.z = POSITION.w * (0.9 + 0.1 * clamp(POSITION.z / POSITION.w, 0.0, 1.0));\n"
                 "}\n";
        c += "void fragment() {\n  ALBEDO = to_linear(COLOR.rgb);\n  ALPHA = COLOR.a;\n}\n";
        m[close].instantiate();
        m[close]->set_shader(make_shader("line", close, c));
    }
    return m[close];
}

// d3d_DrawLineSystem: lines in the system's space, colour as is, alpha x the object's alpha, no fog, no texture
static void draw_lines(uint32_t o)
{
    uint32_t flags = g32(o + OB_FLAGS);
    int close = (flags & FLAG_REALLYCLOSE) != 0;
    float oa = g8(o + OB_COLOR + 3) / 255.0f;
    static dyn_arrays a;
    PackedVector3Array &v = a.v;
    PackedColorArray &col = a.col;
    v.clear();
    col.clear();
    uint32_t head = o + LS_HEAD, n = 0;
    AABB box;
    for (uint32_t l = g32(head + LL_NEXT); (l != head) && (l != 0) && (n < 65536); l = g32(l + LL_NEXT), n++)
    {
        for (int k = 0; k < 2; k++)
        {
            uint32_t pt = l + k * 0x1c;
            Vector3 p = lt_pos(pt);
            v.push_back(p);
            col.push_back(Color(gf(pt + 0x0c), gf(pt + 0x10), gf(pt + 0x14), gf(pt + 0x18) * oa));
            if ((n == 0) && (k == 0)) box = AABB(p, Vector3()); else box.expand_to(p);
        }
    }
    sprite_instance &si = gs->lines[o];
    if (si.mi == NULL)
    {
        si.mi = memnew(MeshInstance3D);
        si.mesh.instantiate();
        si.mi->set_mesh(si.mesh);
        (close ? (Node *)gs->camera : (Node *)gs->root)->add_child(si.mi);
        si.close = close;
    }
    else if (si.close != close)
    {
        si.mi->get_parent()->remove_child(si.mi);
        (close ? (Node *)gs->camera : (Node *)gs->root)->add_child(si.mi);
        si.close = close;
    }
    si.mi->set_transform(lt_object_transform(o));
    std::vector<dyn_part> parts;
    if (n > 0) parts.push_back({ &a, line_material(close) });
    (void) dyn_update(DYN_LINES, si.mesh, si.dyn, parts, 1);
    if (n > 0) si.mi->set_custom_aabb(box.grow(1.0f));
    si.frame = gs->frame;
    if (!si.shown) { si.mi->set_visible(true); si.shown = 1; }
}

static void update_lines(uint32_t mw)
{
    for_objects(mw, OT_LINESYSTEM, [&](uint32_t o) {
        uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
        if (!(flags & FLAG_VISIBLE) || (flags2 & FLAG2_SKYOBJECT)) return;
        draw_lines(o);
    });
    for (auto &kv : gs->lines)
    {
        sprite_instance &si = kv.second;
        if ((si.frame != gs->frame) && si.shown) { si.mi->set_visible(false); si.shown = 0; }
    }
}

/* ------------------------------------------------------------------ */
/* poly grids                                                          */

// LTPolyGrid
#define PG_DATA 0x1b0               // width x height bytes (heights; signed unless FLAG_UNSIGNED)
#define PG_INDICES 0x1b4            // uint16 triangle list
#define PG_SPRITE 0x1b8
#define PG_SPRITEFRAME 0x1c4        // m_SpriteTracker.m_pCurFrame
#define PG_XPAN 0x1d4
#define PG_YPAN 0x1d8
#define PG_XSCALE 0x1dc
#define PG_YSCALE 0x1e0
#define PG_NINDICES 0x1e8
#define PG_WIDTH 0x1f8
#define PG_HEIGHT 0x1fc
#define PG_COLORTABLE 0x200         // 256 x (r, g, b, a) floats, 0-255
#define FLAG_UNSIGNED (1 << 1)

// d3d_DrawPolyGrid: a grid sample (column, height, row) goes through the object's transform with the scale
// ((w + 1) / w, 1, (h + 1) / h) x m_Scale, centred; its colour from the colour table by its height; the texture
// (the sprite's frame) panned and scaled
static void draw_polygrid(uint32_t sd, uint32_t o)
{
    uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
    uint32_t data = g32(o + PG_DATA), idx = g32(o + PG_INDICES), nidx = g32(o + PG_NINDICES);
    uint32_t w = g32(o + PG_WIDTH), h = g32(o + PG_HEIGHT);
    if ((data == 0) || (idx == 0) || (w < 2) || (h < 2) || (w * h > 65536) || (nidx == 0) || (nidx > 6 * 65536)) return;
    uint32_t tex = 0;
    if (g32(o + PG_SPRITE) && g32(o + PG_SPRITEFRAME)) tex = g32(g32(o + PG_SPRITEFRAME));
    texture_entry &te = get_texture(tex);
    float texw = 1.0f / te.inv_w, texh = 1.0f / te.inv_h;
    float sx = gf(o + OB_SCALE), sy = gf(o + OB_SCALE + 4), sz = gf(o + OB_SCALE + 8);
    float xs = gf(o + PG_XSCALE), ys = gf(o + PG_YSCALE);
    float uofs = gf(o + PG_XPAN) / texw, vofs = gf(o + PG_YPAN) / texh;
    float uscale = ((xs != 0.0f) ? 1.0f / xs : 1.0f) * sx / texw * ((float)w / (w - 1));
    float vscale = ((ys != 0.0f) ? 1.0f / ys : 1.0f) * sz / texh * ((float)h / (h - 1));
    float alpha = g8(o + OB_COLOR + 3) / 255.0f;
    float tint[3] = { gf(sd + SD_VERTEXTINT), gf(sd + SD_VERTEXTINT + 4), gf(sd + SD_VERTEXTINT + 8) };
    int is_unsigned = (flags & FLAG_UNSIGNED) != 0;
    float cx = (w - 1) * 0.5f, cz = (h - 1) * 0.5f;
    float gx = ((float)(w + 1) / w) * sx, gz = ((float)(h + 1) / h) * sz;

    static dyn_arrays a;
    PackedVector3Array &v = a.v;
    PackedVector2Array &uv = a.uv;
    PackedColorArray &col = a.col;
    PackedInt32Array &ind = a.ind;
    v.resize((int64_t)w * h);
    uv.resize((int64_t)w * h);
    col.resize((int64_t)w * h);
    Vector3 *P = v.ptrw();
    Vector2 *U = uv.ptrw();
    Color *C = col.ptrw();
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
        {
            uint32_t i = y * w + x;
            int d = is_unsigned ? (int)g8(data + i) : (int)(int8_t)g8(data + i);
            int ci = is_unsigned ? d : d + 128;
            uint32_t ce = o + PG_COLORTABLE + 16 * ci;
            float hy = (float)(is_unsigned ? d - 128 : d) * sy;
            P[i] = Vector3((x - cx) * gx, hy, -((y - cz) * gz));
            U[i] = Vector2((x + uofs) * uscale, (y + vofs) * vscale);
            C[i] = Color(fminf(gf(ce) * tint[0], 255.0f) / 255.0f, fminf(gf(ce + 4) * tint[1], 255.0f) / 255.0f, fminf(gf(ce + 8) * tint[2], 255.0f) / 255.0f,
                         fminf(alpha * gf(ce + 12), 255.0f) / 255.0f);
        }
    ind.resize(nidx);
    int32_t *I = ind.ptrw();
    for (uint32_t i = 0; i < nidx; i++)
    {
        uint32_t k = g16(idx + 2 * i);
        I[i] = (int32_t)((k < w * h) ? k : 0);
    }

    int v_ = SPR_MESH;
    if (flags2 & FLAG2_ADDITIVE) v_ |= SPR_ADD;
    if (flags & FLAG_FOGDISABLE) v_ |= SPR_NOFOG;
    sprite_instance &si = gs->polygrids[o];
    if (si.mi == NULL)
    {
        si.mi = memnew(MeshInstance3D);
        si.mesh.instantiate();
        si.mi->set_mesh(si.mesh);
        gs->root->add_child(si.mi);
    }
    // the object's position and rotation (the scale is in the vertices)
    Quaternion q(-gf(o + OB_ROTATION), -gf(o + OB_ROTATION + 4), gf(o + OB_ROTATION + 8), gf(o + OB_ROTATION + 12));
    if (q.length_squared() < 1e-6f) q = Quaternion();
    si.mi->set_transform(Transform3D(Basis(q.normalized()), lt_pos(o + OB_POS)));
    std::vector<dyn_part> parts;
    parts.push_back({ &a, sprite_material(tex, v_) });
    AABB box = dyn_update(DYN_POLYGRID, si.mesh, si.dyn, parts);
    si.mi->set_custom_aabb(box.grow(1.0f));
    si.frame = gs->frame;
    if (!si.shown) { si.mi->set_visible(true); si.shown = 1; }
}

static void update_polygrids(uint32_t mw, uint32_t sd)
{
    for_objects(mw, OT_POLYGRID, [&](uint32_t o) {
        uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
        if (!(flags & FLAG_VISIBLE) || (flags2 & FLAG2_SKYOBJECT)) return;
        draw_polygrid(sd, o);
    });
    for (auto &kv : gs->polygrids)
    {
        sprite_instance &si = kv.second;
        if ((si.frame != gs->frame) && si.shown) { si.mi->set_visible(false); si.shown = 0; }
    }
}

/* ------------------------------------------------------------------ */
/* canvases                                                            */

// Canvas: the game's draw function (+0x1b0, CanvasDrawFn(ILTCustomDraw *, HLOCALOBJ, void *user), user +0x1b4) draws
// triangle fans through the renderer's ILTCustomDraw (d3d.ren's CanvasDrawMgr): this one's methods are native
// (x64/canvas-asm.asm: thiscall stubs) and collect the fans, grouped by their state, into the canvas's mesh.
#define CV_FN 0x1b0
#define CV_USER 0x1b4
#define RS_GETSHAREDTEXTURE 0x04

enum { LTRSTATE_ALPHABLENDENABLE = 0, LTRSTATE_ZREADENABLE, LTRSTATE_ZWRITEENABLE, LTRSTATE_SRCBLEND, LTRSTATE_DESTBLEND, LTRSTATE_TEXADDR,
       LTRSTATE_COLOROP, LTRSTATE_ALPHAOP, NUM_LTRSTATES };
#define LTBLEND_ZERO 1
#define LTBLEND_ONE 2
#define LTBLEND_SRCCOLOR 3


typedef dyn_arrays canvas_batch;

// the ILTCustomDraw the game gets (its vtable pointer) and the vtable: below 2 GB (LOW_OBJ)
static struct { uint32_t object[4]; uint32_t vtable[5]; } canvas_iface_init;
LOW_SLOT(canvas_iface)

static struct
{
    uint32_t states[NUM_LTRSTATES];
    uint32_t texture;           // SharedTexture*
    int close;                  // the canvas is really close: its vertices are in the camera's space
    std::map<uint64_t, canvas_batch> *batches;  // by (texture, shader variant)
} cdraw;

extern "C" uint32_t canvas_DrawPrimitive_c(uint32_t self, uint32_t verts, uint32_t n, uint32_t flags)
{
    if ((n < 3) || (verts == 0) || (cdraw.batches == NULL)) return 1;
    // the state as a sprite shader variant: additive (ONE, ONE), multiplied (ZERO, SRCCOLOR), else alpha blended
    int v = SPR_MESH;
    uint32_t src = cdraw.states[LTRSTATE_SRCBLEND], dst = cdraw.states[LTRSTATE_DESTBLEND];
    if (cdraw.states[LTRSTATE_ALPHABLENDENABLE])
    {
        if ((src == LTBLEND_ONE) && (dst == LTBLEND_ONE)) v |= SPR_ADD;
        else if ((src == LTBLEND_ZERO) && (dst == LTBLEND_SRCCOLOR)) v |= SPR_MUL;
    }
    if (!cdraw.states[LTRSTATE_ZREADENABLE]) v |= SPR_NOZ;
    if (cdraw.close || (flags & FLAG_REALLYCLOSE)) v |= SPR_CLOSE;
    canvas_batch &b = (*cdraw.batches)[((uint64_t)cdraw.texture << 16) | (uint64_t)v];
    // LTVertex (0x20 bytes): position, rhw, colour b g r a, specular, u, v; a fan
    auto vert = [&](uint32_t k) {
        uint32_t p = verts + k * 0x20;
        b.v.push_back(lt_pos(p));
        b.uv.push_back(Vector2(gf(p + 0x18), gf(p + 0x1c)));
        float a = cdraw.states[LTRSTATE_ALPHABLENDENABLE] ? g8(p + 0x13) / 255.0f : 1.0f;
        b.col.push_back(Color(g8(p + 0x12) / 255.0f, g8(p + 0x11) / 255.0f, g8(p + 0x10) / 255.0f, a));
    };
    for (uint32_t k = 1; k + 1 < n; k++) { vert(0); vert(k + 1); vert(k); }
    return 0;
}

extern "C" uint32_t canvas_SetState_c(uint32_t self, uint32_t state, uint32_t val)
{
    if (state >= NUM_LTRSTATES) return 1;
    cdraw.states[state] = val;
    return 0;
}

extern "C" uint32_t canvas_GetState_c(uint32_t self, uint32_t state, uint32_t pval)
{
    if ((state >= NUM_LTRSTATES) || (pval == 0)) return 1;
    *(uint32_t *)G2H(pval) = cdraw.states[state];
    return 0;
}

extern "C" uint32_t canvas_SetTexture_c(uint32_t self, uint32_t name)
{
    cdraw.texture = 0;
    if ((name == 0) || (render_struct == NULL)) return 1;
    uint32_t args[1] = { name };
    cdraw.texture = call_game(*(uint32_t *)(render_struct + RS_GETSHAREDTEXTURE), 1, args);
    return (cdraw.texture != 0) ? 0 : 1;
}

extern "C" uint32_t canvas_GetTexelSize_c(uint32_t self, uint32_t pu, uint32_t pv)
{
    texture_entry &te = get_texture(cdraw.texture);
    if (pu != 0) *(float *)G2H(pu) = te.uv_w;
    if (pv != 0) *(float *)G2H(pv) = te.uv_h;
    return 0;
}

static void draw_canvas(uint32_t o)
{
    uint32_t fn = g32(o + CV_FN), flags = g32(o + OB_FLAGS);
    if (fn == 0) return;
    __typeof__(canvas_iface_init) *ci = &LOW_OBJ(canvas_iface);
    if (ci->vtable[0] == 0)
    {
        ci->vtable[0] = H2G(canvas_DrawPrimitive_asm2c);
        ci->vtable[1] = H2G(canvas_SetState_asm2c);
        ci->vtable[2] = H2G(canvas_GetState_asm2c);
        ci->vtable[3] = H2G(canvas_SetTexture_asm2c);
        ci->vtable[4] = H2G(canvas_GetTexelSize_asm2c);
        ci->object[0] = H2G(ci->vtable);
    }
    // CanvasDrawMgr::SaveAndSetCanvasStates' defaults
    static const uint32_t defaults[NUM_LTRSTATES] = { 0, 1, 1, 5, 6, 1, 3, 2 };
    memcpy(cdraw.states, defaults, sizeof(defaults));
    cdraw.texture = 0;
    cdraw.close = (flags & FLAG_REALLYCLOSE) != 0;
    std::map<uint64_t, canvas_batch> batches;
    cdraw.batches = &batches;
    uint32_t args[3] = { H2G(ci->object), o, g32(o + CV_USER) };
    call_game(fn, 3, args);
    cdraw.batches = NULL;
    if ((winapi_debug >= 2) && ((gs->frame % 300) == 0))
    {
        size_t nv = 0;
        for (auto &kv : batches) nv += kv.second.v.size();
        eprintf("render3d: canvas %08x drew %zu batches, %zu vertices\n", o, batches.size(), nv);
    }

    sprite_instance &si = gs->canvases[o];
    if (si.mi == NULL)
    {
        si.mi = memnew(MeshInstance3D);
        si.mesh.instantiate();
        si.mi->set_mesh(si.mesh);
        si.mi->set_extra_cull_margin(16384.0f);
        gs->root->add_child(si.mi);
    }
    // each batch padded to a room (triangles: a power of two, kept while it's more than a quarter full) with empty
    // triangles, so that the surfaces keep their size as the canvas draws more or less and are updated in place
    std::map<uint64_t, uint32_t> rooms;
    for (auto &kv : batches)
    {
        canvas_batch &b = kv.second;
        uint32_t tris = (uint32_t)(b.v.size() / 3), room = 0;
        if (tris == 0) continue;
        auto it = si.rooms.find(kv.first);
        if (it != si.rooms.end()) room = it->second;
        if ((tris > room) || (tris * 4 < room))
        {
            room = 16;
            while (room < tris) room *= 2;
        }
        rooms[kv.first] = room;
        Vector3 at = b.v[0];
        for (uint32_t i = tris * 3; i < room * 3; i++)
        {
            b.v.push_back(at);
            b.uv.push_back(Vector2());
            b.col.push_back(Color(0, 0, 0, 0));
        }
    }
    si.rooms.swap(rooms);
    std::vector<dyn_part> parts;
    for (auto &kv : batches)
    {
        if (kv.second.v.is_empty()) continue;
        int v = (int)(kv.first & 0xffff);
        if (flags & FLAG_FOGDISABLE) v |= SPR_NOFOG;
        parts.push_back({ &kv.second, sprite_material((uint32_t)(kv.first >> 16), v) });
    }
    AABB box = dyn_update(DYN_CANVAS, si.mesh, si.dyn, parts);
    if (!parts.empty()) si.mi->set_custom_aabb(box.grow(1.0f));
    // world space, or the camera's (really close)
    si.mi->set_transform(cdraw.close ? gs->camera->get_transform() : Transform3D());
    si.frame = gs->frame;
    if (!si.shown) { si.mi->set_visible(true); si.shown = 1; }
}

static void update_canvases(uint32_t mw)
{
    for_objects(mw, OT_CANVAS, [&](uint32_t o) {
        uint32_t flags = g32(o + OB_FLAGS), flags2 = g32(o + OB_FLAGS2);
        if (!(flags & FLAG_VISIBLE) || (flags2 & FLAG2_SKYOBJECT)) return;
        draw_canvas(o);
    });
    for (auto &kv : gs->canvases)
    {
        sprite_instance &si = kv.second;
        if ((si.frame != gs->frame) && si.shown) { si.mi->set_visible(false); si.shown = 0; }
    }
}

// the engine keeps a context in 32 bits: handles are 1-based indices into this table
static std::vector<world_context *> &contexts(void)
{
    static std::vector<world_context *> table;
    return table;
}

static world_context *find_context(uint32_t h)
{
    std::vector<world_context *> &t = contexts();
    return ((h >= 1) && (h <= t.size())) ? t[h - 1] : NULL;
}

extern "C" uint32_t r3d_create_context(uint32_t world)
{
    world_context *ctx = new world_context;
    ctx->magic = CONTEXT_MAGIC;
    ctx->world = world;
    ctx->built = 0;
    contexts().push_back(ctx);
    if (winapi_debug) eprintf("render3d: CreateContext world %08x -> %zu\n", world, contexts().size());
    return (uint32_t)contexts().size();
}

extern "C" void r3d_delete_context(uint32_t context)
{
    world_context *ctx = find_context(context);
    if (winapi_debug) eprintf("render3d: DeleteContext %u\n", context);
    if (ctx == NULL) return;
    ctx->world = 0;
    // the meshes go with the next world (Godot can only be called on the main thread)
    if ((gs != NULL) && (gs->current == ctx)) gs->current = NULL;
}

/* ------------------------------------------------------------------ */
/* shader preloading                                                   */

// As Godot's documentation recommends ("Reducing stutter from shader (pipeline) compilations",
// tutorials/performance/pipeline_compilations): every shader is made when the renderer starts (r3d_init: ren_Init,
// before the game's first frame), and the scene keeps a hidden instance of each, with each vertex format it's used
// with, for the whole run. Forward+ and Mobile compile the pipelines of every surface added to the scene, hidden ones
// too, at the next frame in the background (the "surface" step), and again for all of them when a rendering feature
// is turned on later (MSAA, ...): an effect's pipelines are ready before it first appears. Otherwise the first object
// with a new shader waits for the compilation: with a cold cache (the first run, a new Godot or driver) that stalled
// the frame 30-70 ms for each (m1s1's first shots). The Compatibility renderer (OpenGL) can't precompile: there the
// documented way is the legacy one, drawing them in the view for a frame - they're shown for PRELOAD_FRAMES frames
// (zero-area triangles: nothing shows; their AABB keeps them in the view), then hidden. Godot keeps the results in
// its caches (user://shader_cache, the driver's pipeline cache).
enum { WU_UV = 1, WU_UV2 = 2, WU_COLOR = 4, WU_CUSTOM = 8, WU_LINES = 16 };
#define PRELOAD_FRAMES 3

static struct
{
    Node3D *node;               // the hidden instances (kept)
    int pending;                // the log after the first frame(s); compatibility: shown until then
    int compatibility;
    uint64_t frame;             // Godot's frame count when they were made
    uint64_t start_us;
} preload;

// a surface with the arrays of the objects' meshes (world: UV, UV2, colour, 4 x custom; models, poly grids,
// canvases: UV, colour; sprites: UV; particles: UV, UV2, colour; lines: colour): one triangle (line) with all its
// vertices at the origin and transparent - no pixels when drawn (compatibility)
static void preload_surface(const Ref<ArrayMesh> &mesh, int format, const Ref<Material> &m)
{
    static const int64_t custom_shift[4] = { Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT, Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT, Mesh::ARRAY_FORMAT_CUSTOM2_SHIFT, Mesh::ARRAY_FORMAT_CUSTOM3_SHIFT };
    int n = (format & WU_LINES) ? 2 : 3;
    PackedVector3Array v;
    PackedVector2Array uv;
    PackedColorArray col;
    PackedFloat32Array custom;
    for (int i = 0; i < n; i++)
    {
        v.push_back(Vector3(0, 0, 0));
        uv.push_back(Vector2(0, 0));
        col.push_back(Color(0, 0, 0, 0));
        for (int k = 0; k < 4; k++) custom.push_back(0.0f);
    }
    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = v;
    if (format & WU_UV) arrays[Mesh::ARRAY_TEX_UV] = uv;
    if (format & WU_UV2) arrays[Mesh::ARRAY_TEX_UV2] = uv;
    if (format & WU_COLOR) arrays[Mesh::ARRAY_COLOR] = col;
    int64_t fmt = 0;
    if (format & WU_CUSTOM)
        for (int k = 0; k < 4; k++)
        {
            arrays[Mesh::ARRAY_CUSTOM0 + k] = custom;
            fmt |= (int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << custom_shift[k];
        }
    mesh->add_surface_from_arrays((format & WU_LINES) ? Mesh::PRIMITIVE_LINES : Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), (BitField<Mesh::ArrayFormat>)fmt);
    mesh->surface_set_material(mesh->get_surface_count() - 1, m);
}

// an AABB that contains every camera (compatibility: never culled)
static void preload_instance(const Ref<ArrayMesh> &mesh, float transparency = 0.0f)
{
    MeshInstance3D *mi = memnew(MeshInstance3D);
    mi->set_mesh(mesh);
    mi->set_custom_aabb(AABB(Vector3(-1e7f, -1e7f, -1e7f), Vector3(2e7f, 2e7f, 2e7f)));
    if (transparency > 0.0f) mi->set_transparency(transparency);
    preload.node->add_child(mi);
}

static Ref<ShaderMaterial> preload_material(const Ref<Shader> &sh, int flags = -1)
{
    Ref<ShaderMaterial> m;
    m.instantiate();
    m->set_shader(sh);
    if (flags >= 0) m->set_shader_parameter("flags", flags);
    return m;
}

static void preload_shaders(void)
{
    // the world kinds in use: the world and world models (blendings), models (also really close), the sky (not
    // alpha tested: sky_material)
    static const int world_kinds[] = { MAT_OPAQUE, MAT_ALPHA_TEST, MAT_BLEND, MAT_ADD };
    static const int sky_kinds[] = { MAT_SKY | MAT_OPAQUE, MAT_SKY | MAT_BLEND, MAT_SKY | MAT_ADD };
    RenderingServer *rs = RenderingServer::get_singleton();
    preload.start_us = Time::get_singleton()->get_ticks_usec();
    preload.frame = Engine::get_singleton()->get_frames_drawn();
    preload.compatibility = (rs->get_current_rendering_method() == String("gl_compatibility"));
    // not under the scene's root: that's hidden and shown with the game's scenes
    preload.node = memnew(Node3D);
    preload.node->set_visible(preload.compatibility);
    gs->viewport->add_child(preload.node);

    Ref<ArrayMesh> world, faded, sky, model, quad, particles, lines;
    world.instantiate(); faded.instantiate(); sky.instantiate(); model.instantiate(); quad.instantiate(); particles.instantiate(); lines.instantiate();
    const int W = WU_UV | WU_UV2 | WU_COLOR | WU_CUSTOM, M = WU_UV | WU_COLOR;
    for (int k : world_kinds)
    {
        preload_surface(world, W, preload_material(world_shader(k)));
        preload_surface(faded, W, preload_material(world_shader(k)));         // world models fading out
        preload_surface(model, M, preload_material(world_shader(k)));
        preload_surface(model, M, preload_material(world_shader(k | MAT_REALLYCLOSE)));
    }
    for (int k : sky_kinds) preload_surface(sky, W, preload_material(world_shader(k)));
    // the sprite shaders: quads, meshes (poly grids, canvases), particle systems (no depth test off)
    static const int blends[3] = { 0, SPR_ADD, SPR_MUL };
    for (int b : blends)
        for (int noz = 0; noz < 2; noz++)
        {
            int v = b | (noz ? SPR_NOZ : 0);
            preload_surface(quad, WU_UV, preload_material(sprite_shader(v), v));
            preload_surface(model, M, preload_material(sprite_shader(v), v | SPR_MESH));
            if (!noz) preload_surface(particles, WU_UV | WU_UV2 | WU_COLOR, preload_material(sprite_shader(v), v | SPR_PARTICLES));
        }
    for (int close = 0; close < 2; close++) preload_surface(lines, WU_COLOR | WU_LINES, line_material(close));
    preload_instance(world);
    preload_instance(faded, 0.5f);
    preload_instance(sky);
    preload_instance(model);
    preload_instance(quad);
    preload_instance(particles);
    preload_instance(lines);
    shaders_preloaded = 1;
    preload.pending = 1;
    if (winapi_debug) eprintf("render3d: preloading shaders (%s, %.1f ms)\n", preload.compatibility ? "compatibility: drawn" : "hidden instances", (Time::get_singleton()->get_ticks_usec() - preload.start_us) / 1000.0);
}

// after Godot's first frame (Forward+, Mobile: the pipelines are queued) or PRELOAD_FRAMES frames (compatibility:
// the shaders are compiled; hidden from then on)
static void preload_update(void)
{
    if (!preload.pending) return;
    uint64_t drawn = Engine::get_singleton()->get_frames_drawn() - preload.frame;
    if (drawn < (preload.compatibility ? PRELOAD_FRAMES : 1)) return;
    preload.pending = 0;
    if (preload.compatibility) preload.node->set_visible(false);
    if (winapi_debug)
        eprintf("render3d: shaders preloaded (pipelines so far: surface %u, draw %u; seen after %u Godot frames)\n",
                (unsigned)RenderingServer::get_singleton()->get_rendering_info(RenderingServer::RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE),
                (unsigned)RenderingServer::get_singleton()->get_rendering_info(RenderingServer::RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW),
                (unsigned)drawn);
}

/* ------------------------------------------------------------------ */
/* the scene                                                           */

extern "C" int r3d_render_scene(uint8_t *scene)
{
    if (!ready()) return 0;
    uint32_t sd = H2G(scene);

    // the world of this scene: SceneDesc::m_hRenderContext (0x34)
    world_context *ctx = find_context(g32(sd + 0x34));
    if ((ctx != NULL) && (ctx->world != 0) && (ctx != gs->current))
    {
        gs->current = ctx;
        build_world(ctx);
    }

    // the camera
    Vector3 pos = lt_pos(sd + SD_POS);
    static int ncam;
    if ((winapi_debug >= 2) && ((ncam++ % 300) == 0))
        eprintf("render3d: camera %.0f %.0f %.0f rot %.3f %.3f %.3f %.3f fov %.3f %.3f far %.0f rect %d %d %d %d\n", gf(sd + SD_POS), gf(sd + SD_POS + 4), gf(sd + SD_POS + 8),
                gf(sd + SD_ROTATION), gf(sd + SD_ROTATION + 4), gf(sd + SD_ROTATION + 8), gf(sd + SD_ROTATION + 12), gf(sd + SD_XFOV), gf(sd + SD_YFOV), gf(sd + SD_FARZ),
                (int)g32(sd + SD_RECT), (int)g32(sd + SD_RECT + 4), (int)g32(sd + SD_RECT + 8), (int)g32(sd + SD_RECT + 12));
    if ((winapi_debug >= 2) && (ncam == 1))
    {
        static const struct { int ofs; const char *name; } v[] = { { 0x1c, "ModelLightAdd" }, { 0x28, "ModelDirAdd" }, { 0x38, "Unknown38" }, { 0x44, "Unknown44" },
            { 0x50, "GlobalLightScale" }, { 0x5c, "GlobalVertexTint" }, { 0x68, "ModelDirAdd2" }, { 0x74, "GlobalLightAdd" } };
        for (auto &e : v) eprintf("render3d: scene %s %.3f %.3f %.3f\n", e.name, gf(sd + e.ofs), gf(sd + e.ofs + 4), gf(sd + e.ofs + 8));
        eprintf("render3d: scene frame time %.4f, draw mode %u, sky objects %u\n", gf(sd + 0x80), g32(sd), g32(sd + 0xb8));
    }
    if ((winapi_debug >= 2) && (ctx != NULL) && (ctx->world != 0) && ((ncam == 2) || (ncam == 900)))
    {
        eprintf("render3d: FogEnable %.0f FogNearZ %.0f FogFarZ %.0f FogR/G/B %.0f %.0f %.0f SkyFogNearZ %.0f SkyFogFarZ %.0f\n", cvar("FogEnable", -1), cvar("FogNearZ", -1),
                cvar("FogFarZ", -1), cvar("FogR", -1), cvar("FogG", -1), cvar("FogB", -1), cvar("SkyFogNearZ", -1), cvar("SkyFogFarZ", -1));
        dump_objects(ctx->world);
        eprintf("render3d: sky box %.0f %.0f %.0f - %.0f %.0f %.0f view %.0f %.0f %.0f - %.0f %.0f %.0f, %d sky objects, world extents %.0f %.0f %.0f - %.0f %.0f %.0f\n",
                gf(sd + 0x84), gf(sd + 0x88), gf(sd + 0x8c), gf(sd + 0x90), gf(sd + 0x94), gf(sd + 0x98), gf(sd + 0x9c), gf(sd + 0xa0), gf(sd + 0xa4), gf(sd + 0xa8), gf(sd + 0xac), gf(sd + 0xb0),
                (int)g32(sd + 0xb8), gf(ctx->world + 0x144), gf(ctx->world + 0x148), gf(ctx->world + 0x14c), gf(ctx->world + 0x150), gf(ctx->world + 0x154), gf(ctx->world + 0x158));
        for (uint32_t i = 0; (i < g32(sd + 0xb8)) && (i < 64); i++)
        {
            uint32_t o = g32(g32(sd + 0xb4) + 4 * i);
            eprintf("render3d:   sky %08x type %u flags %08x/%08x color %08x pos %.0f %.0f %.0f %s\n", o, g8(o + OB_TYPE), g32(o + OB_FLAGS), g32(o + OB_FLAGS2), g32(o + OB_COLOR),
                    gf(o + OB_POS), gf(o + OB_POS + 4), gf(o + OB_POS + 8), ((g8(o + OB_TYPE) == OT_WORLDMODEL) && g32(o + WM_ORIGINALBSP)) ? (const char *)G2H(g32(o + WM_ORIGINALBSP) + 4) : "");
        }
    }
    float qx = gf(sd + SD_ROTATION), qy = gf(sd + SD_ROTATION + 4), qz = gf(sd + SD_ROTATION + 8), qw = gf(sd + SD_ROTATION + 12);
    Quaternion q(-qx, -qy, qz, qw);
    if (q.length_squared() < 1e-6f) q = Quaternion();
    gs->camera->set_transform(Transform3D(Basis(q.normalized()), pos));
    // debugging: AVP2_R3D_CAMLOG=1 logs the camera each frame (motion smoothness: position steps vs frame times)
    static int camlog = -1;
    if (camlog < 0) camlog = (getenv("AVP2_R3D_CAMLOG") != NULL);
    if (camlog)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        eprintf("cam %llu %.3f %.3f %.3f %.5f %.5f %.5f %.5f\n", (unsigned long long)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000,
                pos.x, pos.y, pos.z, qx, qy, qz, qw);
    }
    // debugging: a fixed camera "x y z yaw pitch" (LithTech coordinates, degrees; yaw 0 looks along +z, 90 along +x,
    // pitch up)
    if (getenv("AVP2_R3D_CAMERA"))
    {
        float c[5] = { 0, 0, 0, 0, 0 };
        sscanf(getenv("AVP2_R3D_CAMERA"), "%f %f %f %f %f", &c[0], &c[1], &c[2], &c[3], &c[4]);
        Basis b = Basis(Vector3(0, 1, 0), -c[3] * 0.0174532925f) * Basis(Vector3(1, 0, 0), c[4] * 0.0174532925f);
        gs->camera->set_transform(Transform3D(b, Vector3(c[0], c[1], -c[2])));
    }
    float yfov = gf(sd + SD_YFOV), farz = gf(sd + SD_FARZ);
    if (!(yfov > 0.01f && yfov < 3.0f)) yfov = 1.2f;
    gs->camera->set_fov(yfov * 180.0f / 3.14159265f);
    gs->camera->set_near(1.0f);
    gs->camera->set_far((farz > 10.0f) ? farz : 10000.0f);

    fog_update();
    post_update(sd);
    update_texture_states();
    update_animated_surfaces();
    if (getenv("AVP2_R3D_DUMPSTATES") && ((gs->frame % 300) == 100))
    {
        // debugging: the textures' D3D state changes (SharedTexture::m_pStateChange)
        int n = 0;
        for (auto &kv : gs->textures)
        {
            uint32_t st = kv.first;
            if (st == 0) continue;
            uint32_t sc = g32(st + 0x34);
            if ((n < 0) && (sc == 0))
            {
                n++;
                eprintf("render3d: texture %08x:", st);
                for (int i = 0; i < 16; i++) eprintf(" %08x", g32(st + 4 * i));
                uint32_t cl = g32(st + 0x38);
                eprintf(" cmd \"%.60s\"\n", ((cl > 0x10000) && (cl < 0x80000000u)) ? (const char *)G2H(cl) : "?");
            }
            if (sc == 0) continue;
            if (n++ > 400) break;
            const char *cmd = g32(st + 0x38) ? (const char *)G2H(g32(st + 0x38)) : "";
            eprintf("render3d: texture %08x \"%.40s\" state %08x: render", st, cmd, sc);
            for (uint32_t r = g32(sc); (r < g32(sc + 4)) && (r != 0); r += 8) eprintf(" %u=%u", g32(r), g32(r + 4));
            eprintf(" texture");
            for (uint32_t t = g32(sc + 12); (t < g32(sc + 16)) && (t != 0); t += 12) eprintf(" %u:%u=%u", g32(t), g32(t + 4), g32(t + 8));
            eprintf("\n");
        }
    }
    if (winapi_debug)
    {
        // frame rate (debugging)
        static uint64_t t0;
        static uint32_t frames;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (t0 == 0) t0 = now;
        frames++;
        if (now - t0 >= 5000)
        {
            eprintf("render3d: %.1f frames/s\n", frames * 1000.0 / (double)(now - t0));
            t0 = now;
            frames = 0;
        }
    }

    // the objects (after the camera: models are culled and lit with it)
    if ((ctx != NULL) && (ctx->world != 0) && (ctx == gs->current))
    {
        update_attachments(ctx->world);
        update_world_models(ctx->world);
        update_sky(ctx->world, sd);
        update_world_lights(ctx->world);
        update_anims(ctx->world);
        update_models(ctx->world, sd);
        update_sprites(ctx->world, sd);
        update_particles(ctx->world);
        update_lines(ctx->world);
        update_polygrids(ctx->world, sd);
        update_canvases(ctx->world);
    }

    preload_update();
    if (winapi_debug && ((gs->frame % 1200) == 600))
    {
        static const char *const names[DYN_KINDS] = { "models", "particles", "lines", "poly grids", "canvases" };
        char text[256];
        int len = 0;
        for (int k = 0; k < DYN_KINDS; k++)
            len += snprintf(text + len, sizeof(text) - len, "%s%s %llu/%llu", k ? ", " : "", names[k], (unsigned long long)dyn_stats.in_place[k],
                            (unsigned long long)dyn_stats.rebuilt[k]);
        eprintf("render3d: dynamic meshes (updated in place / rebuilt): %s\n", text);
        memset(&dyn_stats, 0, sizeof(dyn_stats));
    }
    if (!gs->shown)
    {
        gs->root->set_visible(true);
        gs->picture->set_visible(true);
        gs->shown = 1;
    }
    update_viewport_mode();
    return 1;
}

// host_godot.cpp's frame statistics: the 3D view's GPU time (an invalid RID until the scene is made)
RID r3d_viewport_rid(void)
{
    return ((gs != NULL) && (gs->viewport != NULL)) ? gs->viewport->get_viewport_rid() : RID();
}

// the 3D view is drawn only while it's shown (or while the shaders are preloaded by drawing them: compatibility)
static void update_viewport_mode(void)
{
    SubViewport::UpdateMode mode = (gs->shown || preload.pending) ? SubViewport::UPDATE_ALWAYS : SubViewport::UPDATE_DISABLED;
    if (gs->viewport->get_update_mode() != mode) gs->viewport->set_update_mode(mode);
}

extern "C" void r3d_end_frame(int drawn)
{
    // from the game's first frame on: the scene is made (and the shaders preloaded) while Godot draws the 2D frames
    if (!ready()) return;
    preload_update();
    if (!drawn && gs->shown)
    {
        gs->root->set_visible(false);
        gs->picture->set_visible(false);
        gs->shown = 0;
    }
    update_viewport_mode();
}
