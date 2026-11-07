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
 *  To do: sprites, particles, poly grids, lines, canvases, dynamic light on the world, animated light, shadow-map
 *  light animations, sky, fog, detail and environment textures, model LODs and static lights for models.
 *
 *  Debugging: AVP2_R3D_FULLBRIGHT=1 (textures only), AVP2_R3D_LMSCALE / AVP2_R3D_VCSCALE (light scales),
 *  AVP2_R3D_DUMPLM=<dir> (lightmap pages as PNG), AVP2_R3D_CAMERA="x y z yaw pitch" (a fixed camera),
 *  AVP2_DEBUG=2 (the objects, once).
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
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
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/array.hpp>

#include "render3d.h"

using namespace godot;

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

extern "C" int winapi_debug;
extern "C" int ghost_ready(void);
extern "C" int host_is_godot(void);
extern "C" uint32_t call_game(uint32_t func, uint32_t nargs, const uint32_t *args);
extern "C" uint32_t call_game_this(uint32_t func, uint32_t nargs, const uint32_t *args);

/* ------------------------------------------------------------------ */
/* game memory                                                         */

static inline uint32_t g32(uint32_t a) { return *(const uint32_t *)(uintptr_t)a; }
static inline uint16_t g16(uint32_t a) { return *(const uint16_t *)(uintptr_t)a; }
static inline uint8_t g8(uint32_t a) { return *(const uint8_t *)(uintptr_t)a; }
static inline float gf(uint32_t a) { return *(const float *)(uintptr_t)a; }

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
// TextureData
#define TD_BASEWIDTH 0x10           // m_Header.m_BaseWidth, m_BaseHeight
#define TD_BPP 0x22                 // m_Header.m_Extra[2]
#define TD_MIP0 0xcc                // TextureMipData: width, height, data, pitch
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

struct texture_entry
{
    Ref<ImageTexture> texture;  // null: none (missing or unsupported)
    int has_alpha;
    float inv_w, inv_h;         // world UVs are in texels of the DTX's base size
};

// a world model object's mesh instance
struct wm_instance
{
    MeshInstance3D *mi = NULL;
    uint32_t bsp = 0;           // its BSP (the mesh it has)
    uint32_t frame = 0;         // the last frame it was drawn in
    uint8_t alpha = 255;
    int shown = 0;
};

// a model object's mesh, rebuilt when its pose or light changes
struct model_instance
{
    MeshInstance3D *mi = NULL;
    Ref<ArrayMesh> mesh;
    uint64_t key = 0;           // hash of what the mesh was built from
    uint32_t frame = 0;
    int shown = 0;
    int close = 0;              // parented to the camera
    float sun_pos[3] = { 0, 0, 0 };
    float sun = -1.0f;          // how much of the sun reaches it (< 0: not known yet)
};

struct godot_scene
{
    Node3D *root;
    Camera3D *camera;
    world_context *current;
    std::vector<MeshInstance3D *> world_meshes;     // the static world
    std::map<uint32_t, Ref<ArrayMesh>> bsp_meshes;  // world models' BSPs, by WorldBsp*
    std::map<uint32_t, wm_instance> wm_instances;   // by object (LTObject*)
    std::map<uint32_t, model_instance> models;        // by object
    std::map<uint32_t, texture_entry> textures;     // by SharedTexture*
    uint32_t frame;
    int shown;
};
static godot_scene *gs;

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
        gs->root = memnew(Node3D);
        tree->get_root()->add_child(gs->root);
        gs->camera = memnew(Camera3D);
        gs->root->add_child(gs->camera);
        gs->camera->set_current(true);
        gs->camera->set_keep_aspect_mode(Camera3D::KEEP_HEIGHT);
        RenderingServer::get_singleton()->set_default_clear_color(Color(0, 0, 0));
        gs->root->set_visible(false);
    }
    return 1;
}

extern "C" void r3d_set_render_struct(uint8_t *rs)
{
    render_struct = rs;
}

/* ------------------------------------------------------------------ */
/* textures                                                            */

static texture_entry make_texture(uint32_t shared)
{
    texture_entry e;
    e.has_alpha = 0;
    e.inv_w = e.inv_h = 1.0f / 64.0f;
    if ((shared == 0) || (render_struct == NULL)) return e;

    uint32_t td = call_game(*(uint32_t *)(render_struct + RS_GETTEXTURE), 1, &shared);
    if (td == 0) return e;

    uint32_t base_w = g16(td + TD_BASEWIDTH), base_h = g16(td + TD_BASEWIDTH + 2);
    if (base_w) e.inv_w = 1.0f / (float)base_w;
    if (base_h) e.inv_h = 1.0f / (float)base_h;
    int bpp = g8(td + TD_BPP);
    if (bpp == 0) bpp = BPP_32;     // DtxHeader::GetBPPIdent
    uint32_t w = g32(td + TD_MIP0), h = g32(td + TD_MIP0 + 4), data = g32(td + TD_MIP0 + 8);
    int32_t pitch = (int32_t)g32(td + TD_MIP0 + 12);
    if ((w == 0) || (h == 0) || (w > 4096) || (h > 4096) || (data == 0)) return e;

    Ref<Image> img;
    PackedByteArray px;
    if (bpp == BPP_32)
    {
        px.resize((int64_t)w * h * 4);
        uint8_t *d = px.ptrw();
        for (uint32_t y = 0; y < h; y++)
        {
            const uint8_t *s = (const uint8_t *)(uintptr_t)(data + y * pitch);
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
        memcpy(px.ptrw(), (const void *)(uintptr_t)data, size);
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
            const uint16_t *s = (const uint16_t *)(uintptr_t)(data + y * pitch);
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
    gs->textures.erase((uint32_t)(uintptr_t)texture);
}

/* ------------------------------------------------------------------ */
/* materials                                                           */

// texture * light: the lightmap (UV2) for lightmapped polies, else the vertex colour (Gouraud); the colours are
// the game's (sRGB) values, multiplied in linear space (= in sRGB up to the curve's shape)
enum { MAT_OPAQUE = 0, MAT_ALPHA_TEST = 1, MAT_BLEND = 2, MAT_ADD = 3 };
// "really close" objects (the player's weapon): their depth is squeezed in front of everything else
#define MAT_REALLYCLOSE 4
#define MAT_KINDS 8

static const char *shader_code(int kind)
{
    static std::string code[MAT_KINDS];
    if (code[kind].empty())
    {
        int blend = kind & 3;
        std::string modes = "unshaded, cull_disabled";
        if (blend == MAT_ADD) modes += ", blend_add";
        // D3D's arithmetic: texture * light in the stored (sRGB) values, the lightmap stage 2x (MODULATE2X, as
        // d3d.ren uses when the card has it), the vertex colour 1x; the result is converted for Godot (linear)
        char scales[160];
        snprintf(scales, sizeof(scales), "const float LM_SCALE = %.3f;\nconst float VC_SCALE = %.3f;\n",
                 getenv("AVP2_R3D_LMSCALE") ? atof(getenv("AVP2_R3D_LMSCALE")) : 2.0, getenv("AVP2_R3D_VCSCALE") ? atof(getenv("AVP2_R3D_VCSCALE")) : 1.0);
        std::string c = "shader_type spatial;\nrender_mode " + modes + ";\n"
            "uniform sampler2D albedo : filter_linear_mipmap_anisotropic, repeat_enable;\n"
            "uniform sampler2D lightmap : filter_linear, repeat_disable;\n"
            "uniform float use_lightmap = 0.0;\n" + std::string(scales) +
            "vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n";
        // reverse z (near 1, far 0): the really close objects get [0.9, 1]
        if (kind & MAT_REALLYCLOSE)
            c += "void vertex() {\n"
                 "  POSITION = PROJECTION_MATRIX * (MODELVIEW_MATRIX * vec4(VERTEX, 1.0));\n"
                 "  POSITION.z = POSITION.w * (0.9 + 0.1 * clamp(POSITION.z / POSITION.w, 0.0, 1.0));\n"
                 "}\n";
        c += "void fragment() {\n"
            "  vec4 t = texture(albedo, UV);\n"
            "  vec3 light = (use_lightmap > 0.5) ? texture(lightmap, UV2).rgb * LM_SCALE : COLOR.rgb * VC_SCALE;\n"
            "  ALBEDO = to_linear(clamp(t.rgb * light, 0.0, 1.0));\n";
        if (getenv("AVP2_R3D_FULLBRIGHT")) c += "  ALBEDO = to_linear(t.rgb);\n";
        if (blend == MAT_ALPHA_TEST) c += "  ALPHA = t.a;\n  ALPHA_SCISSOR_THRESHOLD = 0.5;\n";
        if (blend == MAT_BLEND) c += "  ALPHA = t.a * COLOR.a;\n";
        if (blend == MAT_ADD) c += "  ALPHA = 1.0;\n";
        c += "}\n";
        code[kind] = c;
    }
    return code[kind].c_str();
}

struct lm_page
{
    int size;
    std::vector<uint32_t> px;   // 0x00RRGGBB
    int x, y, row_h;
    Ref<ImageTexture> texture;
};

struct world_materials
{
    Ref<Shader> shaders[MAT_KINDS];
    std::map<uint64_t, Ref<ShaderMaterial>> materials;
    std::vector<lm_page> pages;
};
static world_materials *wm;

static Ref<ShaderMaterial> get_material(uint32_t shared, int kind, int page)
{
    uint64_t key = ((uint64_t)shared << 16) | ((uint64_t)(page + 1) << 4) | (uint64_t)kind;
    auto it = wm->materials.find(key);
    if (it != wm->materials.end()) return it->second;

    if (wm->shaders[kind].is_null())
    {
        wm->shaders[kind].instantiate();
        wm->shaders[kind]->set_code(shader_code(kind));
    }
    Ref<ShaderMaterial> m;
    m.instantiate();
    m->set_shader(wm->shaders[kind]);
    texture_entry &t = get_texture(shared);
    if (t.texture.is_valid()) m->set_shader_parameter("albedo", t.texture);
    if ((page >= 0) && (page < (int)wm->pages.size()))
    {
        m->set_shader_parameter("lightmap", wm->pages[page].texture);
        m->set_shader_parameter("use_lightmap", 1.0f);
    }
    wm->materials[key] = m;
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
    const uint8_t *p = (const uint8_t *)(uintptr_t)in, *e = p + len;
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
static int lm_place(const uint32_t *texels, int w, int h, int *ox, int *oy)
{
    int bw = w + 2, bh = h + 2;
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
    for (int y = -1; y <= h; y++)
    {
        int sy = (y < 0) ? 0 : ((y >= h) ? h - 1 : y);
        uint32_t *row = &pg->px[(size_t)(pg->y + 1 + y) * pg->size + pg->x + 1];
        for (int x = -1; x <= w; x++)
        {
            int sx = (x < 0) ? 0 : ((x >= w) ? w - 1 : x);
            row[x] = texels[sy * w + sx];
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

// the poly's lightmap (texels 0x00RRGGBB): its animations' frames, the first one as is, the others added at
// their intensity
static int poly_lightmap(uint32_t mw, uint32_t poly, uint32_t *texels, int *w, int *h)
{
    static uint32_t layer[0x400];
    *w = g8(poly + WP_LMWIDTH);
    *h = g8(poly + WP_LMHEIGHT);
    int n = *w * *h;
    if ((n == 0) || (n > 0x400)) return 0;
    uint32_t refs = g32(poly + WP_LMANIMREFS), nrefs = g32(poly + WP_NLMANIMREFS);
    if ((refs == 0) || (nrefs == 0) || (nrefs > 64)) return 0;
    int layers = 0;
    for (uint32_t r = 0; r < nrefs; r++)
    {
        uint32_t anim, lf = anim_frame(mw, refs + 4 * r, &anim);
        if ((lf == 0) || g32(anim + LA_SHADOWMAP)) continue;
        uint32_t data = g32(lf), len = g16(lf + 4);
        if ((data == 0) || (len == 0)) continue;
        memset(layer, 0, (size_t)n * 4);
        if (!decompress_lm(data, len, layer, (uint32_t)n)) continue;
        if (layers == 0) memcpy(texels, layer, (size_t)n * 4);
        else
        {
            uint32_t scale = (uint32_t)(gf(anim + LA_BLEND) * 255.0f);
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
        layers++;
    }
    return layers > 0;
}

// a Gouraud poly's vertex colours (r, g, b per vertex): the sum of its animations' frames
static void poly_vertex_light(uint32_t mw, uint32_t poly, uint32_t n, uint8_t (*rgb)[3])
{
    memset(rgb, 0, (size_t)n * 3);
    uint32_t refs = g32(poly + WP_LMANIMREFS), nrefs = g32(poly + WP_NLMANIMREFS);
    if ((refs == 0) || (nrefs > 64)) return;
    for (uint32_t r = 0; r < nrefs; r++)
    {
        uint32_t anim, lf = anim_frame(mw, refs + 4 * r, &anim);
        if (lf == 0) continue;
        uint32_t vr = g32(lf + LF_VERTR), vg = g32(lf + LF_VERTG), vb = g32(lf + LF_VERTB), nv = g8(lf + LF_NVERTS);
        if ((vr == 0) || (vg == 0) || (vb == 0)) continue;
        for (uint32_t i = 0; (i < n) && (i < nv); i++)
        {
            uint32_t c;
            c = rgb[i][0] + g8(vr + i); rgb[i][0] = (uint8_t)((c > 255) ? 255 : c);
            c = rgb[i][1] + g8(vg + i); rgb[i][1] = (uint8_t)((c > 255) ? 255 : c);
            c = rgb[i][2] + g8(vb + i); rgb[i][2] = (uint8_t)((c > 255) ? 255 : c);
        }
    }
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
                uint32_t model = g32(o + 0x1f8), vt = g32(o);
                eprintf("render3d:     vtable %08x [18] %08x model %08x skins %08x %08x %08x %08x hidden %08x transforms %08x/%u light %.2f %.2f %.2f\n", vt, vt ? g32(vt + 0x48) : 0, model,
                        g32(o + 0x1c0), g32(o + 0x1c4), g32(o + 0x1c8), g32(o + 0x1cc), g32(o + 0x24c), g32(o + 0x238), g32(o + 0x23c), gf(o + 0x2c0), gf(o + 0x2c4), gf(o + 0x2c8));
                if (model == 0) return;
                uint32_t fn = g32(model + 4);
                eprintf("render3d:     \"%s\" pieces %u nodes %u (flat %u) weights %u verts %u tris %u\n", fn ? (const char *)(uintptr_t)fn : "?", g32(model + 0x3c), g32(model + 0x78), g32(model + 0x2c),
                        g32(model + 0x5c), g32(model + 0x64), g32(model + 0x68));
                for (uint32_t i = 0; (i < g32(model + 0x3c)) && (i < 8); i++)
                {
                    uint32_t pc = g32(g32(model + 0x38) + 4 * i);
                    eprintf("render3d:       piece \"%.32s\" texture %u verts %u tris %u lods %u\n", (const char *)(uintptr_t)(pc + 0x48), g32(pc + 0x24), g32(pc + 8), g32(pc + 0x18), g32(pc + 0x3c));
                }
            }
            if ((t == OT_WORLDMODEL) || (t == OT_CONTAINER))
            {
                uint32_t bsp = g32(o + WM_ORIGINALBSP);
                if (bsp == 0) return;
                eprintf("render3d:     bsp %08x/%08x/%08x \"%.64s\" wif %08x polies %u translation %.1f %.1f %.1f\n", bsp, g32(o + WM_WORLDBSP), g32(o + WM_VALIDBSP),
                        (const char *)(uintptr_t)(bsp + WB_NAME), g32(bsp + WB_WORLDINFOFLAGS), g32(bsp + WB_NPOLIES),
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
};
// polygons grouped by (texture, material kind, lightmap page)
typedef std::map<uint64_t, batch> batch_map;

struct build_stats
{
    uint32_t polies, tris, lightmapped;
};

static void clear_world(void)
{
    for (MeshInstance3D *mi : gs->world_meshes) mi->queue_free();
    gs->world_meshes.clear();
    for (auto &kv : gs->wm_instances) kv.second.mi->queue_free();
    gs->wm_instances.clear();
    for (auto &kv : gs->models) kv.second.mi->queue_free();
    gs->models.clear();
    gs->bsp_meshes.clear();
    delete wm;
    wm = new world_materials();
}

// a BSP's polygons into batches, in the BSP's own coordinates (where it was placed in the level)
static void add_bsp(uint32_t mw, uint32_t bsp, float grid, batch_map &batches, build_stats &st)
{
    static uint32_t texels[0x400];
    static uint8_t vlight[1024][3];
    uint32_t polies = g32(bsp + WB_POLIES), npolies = g32(bsp + WB_NPOLIES);
    if ((polies == 0) || (npolies > 1000000)) return;
    for (uint32_t p = 0; p < npolies; p++)
    {
        uint32_t poly = g32(polies + 4 * p);
        if (poly == 0) continue;
        uint32_t surface = g32(poly + WP_SURFACE);
        if (surface == 0) continue;
        uint32_t sflags = g32(surface + SF_FLAGS);
        if (sflags & (SURF_INVISIBLE | SURF_NONEXISTANT | SURF_SKY)) continue;
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
        texture_entry &te = get_texture(tex);

        // the lightmap
        int lw, lh, page = -1, lx = 0, ly = 0;
        float right[3], up[3], org[3] = { 0, 0, 0 };
        if (!(sflags & SURF_BRIGHT) && poly_lightmap(mw, poly, texels, &lw, &lh))
        {
            page = lm_place(texels, lw, lh, &lx, &ly);
            lm_axes(poly, right, up);
            org[0] = gf(poly + WP_LMORIGIN); org[1] = gf(poly + WP_LMORIGIN + 4); org[2] = gf(poly + WP_LMORIGIN + 8);
            st.lightmapped++;
        }
        batch &b = batches[((uint64_t)tex << 16) | ((uint64_t)(page + 1) << 4) | (uint64_t)kind];
        if ((page < 0) && (n <= 1024)) poly_vertex_light(mw, poly, n, vlight);
        for (k = 1; k + 1 < n; k++)
        {
            uint32_t idx[3] = { 0, k + 1, k };     // the z flip mirrors the winding
            for (int j = 0; j < 3; j++)
            {
                uint32_t pv = verts + idx[j] * PV_SIZE, pt = g32(pv + PV_VEC);
                b.v.push_back(lt_pos(pt));
                b.uv.push_back(Vector2(gf(pv + PV_U) * te.inv_w, gf(pv + PV_V) * te.inv_h));
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
        mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
        int page = (int)((kv.first >> 4) & 0xfff) - 1;
        mesh->surface_set_material(surfaces, get_material((uint32_t)(kv.first >> 16), (int)(kv.first & 0xf), page));
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
    if (winapi_debug) eprintf("render3d: %u light animations, lightmap grid %.1f\n", g32(mw + MW_LIGHTANIMS + 8), grid);

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
            eprintf("render3d: bsp %u %08x \"%.64s\" wif %08x polies %u translation %.1f %.1f %.1f\n", i, bsp, (const char *)(uintptr_t)(bsp + 4), wif, g32(bsp + WB_NPOLIES),
                    gf(bsp + 0xe8), gf(bsp + 0xec), gf(bsp + 0xf0));
        if (wif & WIF_VISBSP) continue;
        if (wif & (WIF_MAINWORLD | WIF_TERRAIN)) add_bsp(mw, bsp, grid, batches, st);
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
    }
    ctx->built = 1;
    if (winapi_debug) eprintf("render3d: built %u polies (%u lightmapped, %zu lightmap pages), %u triangles, %zu static batches, %zu world model meshes, %zu textures\n",
                              st.polies, st.lightmapped, wm->pages.size(), st.tris, batches.size(), gs->bsp_meshes.size(), gs->textures.size());
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
#define MI_MODEL 0x1f8              // m_AnimTracker.m_TimeRef.m_pModel
#define MI_HIDDENPIECES 0x24c
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
extern "C" void lithtech_i_IntersectSegment(void);
#define MW_WORLDTREE 0x6c
#define WT_ROOT 0x0c
#define WTN_RADIUS 0x40

// CastRayAtSky: does the segment from p along d end on a sky poly? (the query and its result in 32-bit memory)
static int ray_at_sky(uint32_t mw, const float p[3], const float d[3])
{
    static uint32_t query[16], info[16];
    float *q = (float *)query;
    memset(query, 0, sizeof(query));
    memset(info, 0, sizeof(info));
    for (int i = 0; i < 3; i++) { q[i] = p[i]; q[3 + i] = p[i] + d[i]; }
    query[9] = 4;       // INTERSECT_HPOLY
    uint32_t args[4] = { (uint32_t)(uintptr_t)query, (uint32_t)(uintptr_t)info, mw + MW_WORLDTREE, 0 };
    if (!call_game((uint32_t)(uintptr_t)&lithtech_i_IntersectSegment, 4, args)) return 0;
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
    mi.mi->set_transform(xf);
    mi.frame = gs->frame;
    if (!mi.shown) { mi.mi->set_visible(true); mi.shown = 1; }

    // rebuilt only when the pose, the light, the skins or the hidden pieces changed
    uint64_t key = 0xcbf29ce484222325ull;
    key = hash_bytes(key, (const void *)(uintptr_t)tr, (size_t)nnodes * 0x40);
    key = hash_bytes(key, base, sizeof(base));
    key = hash_bytes(key, (const void *)(uintptr_t)(o + MI_SKINS), 16);
    key = hash_bytes(key, (const void *)(uintptr_t)(o + MI_HIDDENPIECES), 4);
    key = hash_bytes(key, &alpha, 1);
    key = hash_bytes(key, &flags2, 4);
    key = hash_bytes(key, &model, 4);
    for (const model_light &L : ml) key = hash_bytes(key, &L, sizeof(L));
    if ((key == mi.key) && (mi.mesh->get_surface_count() > 0)) return;
    mi.key = key;

    // a surface per skin
    struct surf { PackedVector3Array v; PackedVector2Array uv; PackedColorArray col; };
    surf s[4];
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
        surf &S = s[tex];
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
                    render_struct ? gf((uint32_t)(uintptr_t)render_struct + RS_LIGHTCOLOR) : 0.0f, render_struct ? gf((uint32_t)(uintptr_t)render_struct + RS_LIGHTCOLOR + 4) : 0.0f,
                    render_struct ? gf((uint32_t)(uintptr_t)render_struct + RS_LIGHTCOLOR + 8) : 0.0f, render_struct ? gf((uint32_t)(uintptr_t)render_struct + RS_LIGHTDIR) : 0.0f,
                    render_struct ? gf((uint32_t)(uintptr_t)render_struct + RS_LIGHTDIR + 4) : 0.0f, render_struct ? gf((uint32_t)(uintptr_t)render_struct + RS_LIGHTDIR + 8) : 0.0f,
                    gf(sd + 0x74), gf(sd + 0x78), gf(sd + 0x7c), gf(sd + 0x50), gf(sd + 0x54), gf(sd + 0x58), render_struct ? g32((uint32_t)(uintptr_t)render_struct + 0x150) : 0);
            eprintf("render3d: really close model \"%s\" fov offset %u %.3f %.3f, box %.1f %.1f %.1f - %.1f %.1f %.1f, at %.2f %.2f %.2f\n", g32(model + 4) ? (const char *)(uintptr_t)g32(model + 4) : "?",
                    g32(model + 0x140), gf(model + 0x144), gf(model + 0x148), box.position.x, box.position.y, box.position.z, box.get_end().x, box.get_end().y, box.get_end().z,
                    xf.origin.x, xf.origin.y, xf.origin.z);
        }
    }
    mi.mesh->clear_surfaces();
    int nsurf = 0;
    for (int t = 0; t < 4; t++)
    {
        if (s[t].v.is_empty()) continue;
        uint32_t skin = g32(o + MI_SKINS + 4 * t);
        int kind = MAT_OPAQUE;
        if (flags2 & FLAG2_ADDITIVE) kind = MAT_ADD;
        else if (alpha < 255) kind = MAT_BLEND;
        else if (get_texture(skin).has_alpha) kind = MAT_ALPHA_TEST;
        if (close) kind |= MAT_REALLYCLOSE;
        Array arrays;
        arrays.resize(Mesh::ARRAY_MAX);
        arrays[Mesh::ARRAY_VERTEX] = s[t].v;
        arrays[Mesh::ARRAY_TEX_UV] = s[t].uv;
        arrays[Mesh::ARRAY_COLOR] = s[t].col;
        mi.mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
        mi.mesh->surface_set_material(nsurf++, get_material(skin, kind, -1));
    }
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
/* the scene                                                           */

extern "C" int r3d_render_scene(uint8_t *scene)
{
    if (!ready()) return 0;
    uint32_t sd = (uint32_t)(uintptr_t)scene;

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
    if ((winapi_debug >= 2) && (ctx != NULL) && (ctx->world != 0) && ((ncam == 2) || (ncam == 900))) dump_objects(ctx->world);
    if ((ctx != NULL) && (ctx->world != 0) && (ctx == gs->current))
    {
        update_attachments(ctx->world);
        update_world_models(ctx->world);
        update_models(ctx->world, sd);
    }
    float qx = gf(sd + SD_ROTATION), qy = gf(sd + SD_ROTATION + 4), qz = gf(sd + SD_ROTATION + 8), qw = gf(sd + SD_ROTATION + 12);
    Quaternion q(-qx, -qy, qz, qw);
    if (q.length_squared() < 1e-6f) q = Quaternion();
    gs->camera->set_transform(Transform3D(Basis(q.normalized()), pos));
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

    if (!gs->shown)
    {
        gs->root->set_visible(true);
        gs->shown = 1;
    }
    return 1;
}

extern "C" void r3d_end_frame(int drawn)
{
    if ((gs == NULL) || !ghost_ready()) return;
    if (!drawn && gs->shown)
    {
        gs->root->set_visible(false);
        gs->shown = 0;
    }
}
