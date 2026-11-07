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
 *  Done: world geometry (every BSP but the VisBSP, where it was placed) with textures, lightmaps (light animation
 *  frames, packed into atlas pages) and vertex light (Gouraud polies: the animations' per-vertex colours), D3D's
 *  arithmetic (sRGB values, lightmaps 2x), the camera. To do: moving world models (doors), models, sprites,
 *  particles, poly grids, lines, canvases, dynamic lights, animated light, shadow-map light animations, sky, fog,
 *  detail and environment textures.
 *
 *  Debugging: AVP2_R3D_FULLBRIGHT=1 (textures only), AVP2_R3D_LMSCALE / AVP2_R3D_VCSCALE (light scales),
 *  AVP2_R3D_DUMPLM=<dir> (lightmap pages as PNG).
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

struct godot_scene
{
    Node3D *root;
    Camera3D *camera;
    world_context *current;
    std::vector<MeshInstance3D *> world_meshes;
    std::map<uint32_t, texture_entry> textures;     // by SharedTexture*
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

static const char *shader_code(int kind)
{
    static std::string code[4];
    if (code[kind].empty())
    {
        std::string modes = "unshaded, cull_disabled";
        if (kind == MAT_ADD) modes += ", blend_add";
        // D3D's arithmetic: texture * light in the stored (sRGB) values, the lightmap stage 2x (MODULATE2X, as
        // d3d.ren uses when the card has it), the vertex colour 1x; the result is converted for Godot (linear)
        char scales[160];
        snprintf(scales, sizeof(scales), "const float LM_SCALE = %.3f;\nconst float VC_SCALE = %.3f;\n",
                 getenv("AVP2_R3D_LMSCALE") ? atof(getenv("AVP2_R3D_LMSCALE")) : 2.0, getenv("AVP2_R3D_VCSCALE") ? atof(getenv("AVP2_R3D_VCSCALE")) : 1.0);
        std::string c = "shader_type spatial;\nrender_mode " + modes + ";\n"
            "uniform sampler2D albedo : filter_linear_mipmap_anisotropic, repeat_enable;\n"
            "uniform sampler2D lightmap : filter_linear, repeat_disable;\n"
            "uniform float use_lightmap = 0.0;\n" + std::string(scales) +
            "vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n"
            "void fragment() {\n"
            "  vec4 t = texture(albedo, UV);\n"
            "  vec3 light = (use_lightmap > 0.5) ? texture(lightmap, UV2).rgb * LM_SCALE : COLOR.rgb * VC_SCALE;\n"
            "  ALBEDO = to_linear(clamp(t.rgb * light, 0.0, 1.0));\n";
        if (getenv("AVP2_R3D_FULLBRIGHT")) c += "  ALBEDO = to_linear(t.rgb);\n";
        if (kind == MAT_ALPHA_TEST) c += "  ALPHA = t.a;\n  ALPHA_SCISSOR_THRESHOLD = 0.5;\n";
        if (kind == MAT_BLEND) c += "  ALPHA = t.a * COLOR.a;\n";
        if (kind == MAT_ADD) c += "  ALPHA = 1.0;\n";
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
    Ref<Shader> shaders[4];
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
/* the world                                                           */

struct batch
{
    PackedVector3Array v;
    PackedVector2Array uv, uv2;
    PackedColorArray col;
};

static void clear_world(void)
{
    for (MeshInstance3D *mi : gs->world_meshes) mi->queue_free();
    gs->world_meshes.clear();
    delete wm;
    wm = new world_materials();
}

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

    // polygons grouped by (texture, material kind, lightmap page)
    std::map<uint64_t, batch> batches;
    uint32_t npolies_total = 0, ntris = 0, nlm = 0;

    static uint32_t texels[0x400];
    static uint8_t vlight[1024][3];
    for (uint32_t i = 0; i < nmodels; i++)
    {
        uint32_t wd = g32(models + 4 * i);
        if (wd == 0) continue;
        uint32_t bsp = g32(wd + WD_ORIGINALBSP);
        if (bsp == 0) continue;
        uint32_t wif = g32(bsp + WB_WORLDINFOFLAGS);
        uint32_t polies = g32(bsp + WB_POLIES), npolies = g32(bsp + WB_NPOLIES);
        // every BSP but the visibility one, where it was placed in the level (world models' points are in world
        // space; the ones that move - doors - should follow their objects: not done yet)
        if (wif & WIF_VISBSP) continue;
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
                nlm++;
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
                ntris++;
            }
            npolies_total++;
        }
    }
    upload_lm_pages();

    // meshes: a surface per batch (Godot allows 256 surfaces per mesh)
    Ref<ArrayMesh> mesh;
    int surfaces = 0;
    for (auto &kv : batches)
    {
        if (mesh.is_null() || (surfaces == 250))
        {
            if (mesh.is_valid())
            {
                MeshInstance3D *mi = memnew(MeshInstance3D);
                mi->set_mesh(mesh);
                gs->root->add_child(mi);
                gs->world_meshes.push_back(mi);
            }
            mesh.instantiate();
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
    if (mesh.is_valid())
    {
        MeshInstance3D *mi = memnew(MeshInstance3D);
        mi->set_mesh(mesh);
        gs->root->add_child(mi);
        gs->world_meshes.push_back(mi);
    }
    ctx->built = 1;
    if (winapi_debug) eprintf("render3d: built %u polies (%u lightmapped, %zu lightmap pages), %u triangles, %zu batches, %zu textures\n",
                              npolies_total, nlm, wm->pages.size(), ntris, batches.size(), gs->textures.size());

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
    float qx = gf(sd + SD_ROTATION), qy = gf(sd + SD_ROTATION + 4), qz = gf(sd + SD_ROTATION + 8), qw = gf(sd + SD_ROTATION + 12);
    Quaternion q(-qx, -qy, qz, qw);
    if (q.length_squared() < 1e-6f) q = Quaternion();
    gs->camera->set_transform(Transform3D(Basis(q.normalized()), pos));
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
