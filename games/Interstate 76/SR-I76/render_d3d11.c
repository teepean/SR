/**
 *
 *  Rendering backend: Direct3D 11 (Windows).
 *
 *  Same design as render_gl.c: 2D pictures are uploaded to a texture and drawn letterboxed into the window;
 *  Glide draws into front/back render targets at (Glide resolution * glide_scale) with a 32-bit float depth
 *  buffer, one pixel shader implements the Glide pixel pipeline (texture combine, color/alpha combine, chroma
 *  key, fog, Z/W depth) from a constant buffer. The shaders are compiled at startup with d3dcompiler_47.dll.
 *
 */

#ifdef _WIN32

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <SDL_syswm.h>
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "render_backend.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int winapi_debug;    // winapi.h (not included: it clashes with windows.h)

#define eprintf(...) fprintf(stderr,__VA_ARGS__)
#define RELEASE(p) do { if ((p) != NULL) { (p)->Release(); (p) = NULL; } } while (0)


/* ------------------------------------------------------------------ */
/* shaders                                                             */

static const char shader_source[] =
    "Texture2D tex0 : register(t0);\n"
    "SamplerState smp0 : register(s0);\n"
    "\n"
    "cbuffer QuadCB : register(b0) { int q_keyed; int3 q_pad; };\n"
    "struct QuadV { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "QuadV quad_vs(float2 p : POSITION) {\n"
    "    QuadV o;\n"
    "    o.uv = p;\n"
    "    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n"
    "float4 quad_ps(QuadV i) : SV_Target {\n"
    "    float4 c = tex0.Sample(smp0, i.uv);\n"
    "    if (q_keyed != 0 && c.a < 0.5) discard;\n"
    "    return float4(c.rgb, 1.0);\n"
    "}\n"
    "\n"
    "cbuffer GlideCB : register(b1) {\n"
    "    float2 screen; float2 texscale;\n"
    "    int4 cc; int4 ac; int4 tc; int4 inv;\n"
    "    float4 constcolor;\n"
    "    float4 chroma_color;\n"
    "    float4 fog_color;\n"
    "    int textured; int chroma; int fog; int depth_mode;\n"
    "    float4 fog_table[16];\n"
    "};\n"
    "struct GlideIn { float3 xyz : POSITION; float3 rgb : COLOR; float3 ooz_a_oow : TEXCOORD1; float3 tex : TEXCOORD2; };\n"
    "struct GlideV {\n"
    "    float4 pos : SV_Position;\n"
    "    noperspective float4 color : COLOR0;\n"
    "    noperspective float2 depth : TEXCOORD0;\n"      // ooz, oow
    "    float2 st : TEXCOORD1;\n"
    "};\n"
    "GlideV glide_vs(GlideIn i) {\n"
    "    GlideV o;\n"
    "    float oow = i.ooz_a_oow.z;\n"
    "    float w = (oow > 0.0) ? 1.0 / oow : 1.0;\n"
    "    float2 ndc = float2(i.xyz.x / screen.x * 2.0 - 1.0, 1.0 - i.xyz.y / screen.y * 2.0);\n"
    "    o.pos = float4(ndc * w, 0.0, w);\n"
    "    o.color = float4(i.rgb, i.ooz_a_oow.y) / 255.0;\n"
    "    o.depth = float2(i.ooz_a_oow.x, oow);\n"
    "    float tw = (i.tex.z > 0.0) ? i.tex.z : ((oow > 0.0) ? oow : 1.0);\n"
    "    o.st = i.tex.xy / tw * texscale;\n"
    "    return o;\n"
    "}\n"
    "float factor_s(int f, float local, float local_a, float other_a, float tex_a) {\n"
    "    if (f == 0) return 0.0;\n"
    "    if (f == 1) return local;\n"
    "    if (f == 2) return other_a;\n"
    "    if (f == 3) return local_a;\n"
    "    if (f == 4) return tex_a;\n"
    "    if (f == 5) return 0.0;\n"
    "    if (f == 9) return 1.0 - local;\n"
    "    if (f == 10) return 1.0 - other_a;\n"
    "    if (f == 11) return 1.0 - local_a;\n"
    "    if (f == 12) return 1.0 - tex_a;\n"
    "    return 1.0;\n"
    "}\n"
    "float3 factor_v(int f, float3 local, float local_a, float other_a, float tex_a) {\n"
    "    if (f == 1) return local;\n"
    "    if (f == 9) return float3(1.0, 1.0, 1.0) - local;\n"
    "    return factor_s(f, 0.0, local_a, other_a, tex_a).xxx;\n"
    "}\n"
    "float3 func_v(int fn, float3 f, float3 local, float local_a, float3 other) {\n"
    "    if (fn == 0) return float3(0.0, 0.0, 0.0);\n"
    "    if (fn == 1) return local;\n"
    "    if (fn == 2) return local_a.xxx;\n"
    "    if (fn == 3) return f * other;\n"
    "    if (fn == 4) return f * other + local;\n"
    "    if (fn == 5) return f * other + local_a.xxx;\n"
    "    if (fn == 6) return f * (other - local);\n"
    "    if (fn == 7) return f * (other - local) + local;\n"
    "    if (fn == 8) return f * (other - local) + local_a.xxx;\n"
    "    if (fn == 9) return f * (-local) + local;\n"
    "    if (fn == 16) return f * (-local) + local_a.xxx;\n"
    "    return float3(0.0, 0.0, 0.0);\n"
    "}\n"
    "float func_s(int fn, float f, float local, float other) {\n"
    "    return func_v(fn, f.xxx, local.xxx, local, other.xxx).x;\n"
    "}\n"
    "float fog_value(int i) { return fog_table[i >> 2][i & 3]; }\n"
    "float2 gmod(float2 x, float2 y) { return x - y * floor(x / y); }\n"
    "struct GlideOut { float4 color : SV_Target; float depth : SV_Depth; };\n"
    "GlideOut glide_ps(GlideV v) {\n"
    "    GlideOut o;\n"
    "    float4 iter = saturate(v.color);\n"
    "    float4 tex = float4(1.0, 1.0, 1.0, 1.0);\n"
    "    if (textured != 0) {\n"
    "        float4 t = tex0.Sample(smp0, v.st);\n"
    "        float3 frgb = factor_v(tc.y, t.rgb, t.a, 0.0, 0.0);\n"
    "        tex.rgb = func_v(tc.x, frgb, t.rgb, t.a, float3(0.0, 0.0, 0.0));\n"
    "        float fa = factor_s(tc.w, t.a, t.a, 0.0, 0.0);\n"
    "        tex.a = func_s(tc.z, fa, t.a, 0.0);\n"
    "        if (inv.z != 0) tex.rgb = float3(1.0, 1.0, 1.0) - tex.rgb;\n"
    "        if (inv.w != 0) tex.a = 1.0 - tex.a;\n"
    "        tex = saturate(tex);\n"
    "    }\n"
    "    float a_local = (ac.z == 0) ? iter.a : ((ac.z == 1) ? constcolor.a : saturate(v.depth.x / 65535.0));\n"
    "    float a_other = (ac.w == 0) ? iter.a : ((ac.w == 1) ? tex.a : constcolor.a);\n"
    "    float3 c_local = (cc.z == 1) ? constcolor.rgb : iter.rgb;\n"
    "    float4 c_other = (cc.w == 0) ? iter : ((cc.w == 1) ? tex : constcolor);\n"
    "    float3 cf = factor_v(cc.y, c_local, a_local, c_other.a, tex.a);\n"
    "    float3 rgb = func_v(cc.x, cf, c_local, a_local, c_other.rgb);\n"
    "    if (inv.x != 0) rgb = float3(1.0, 1.0, 1.0) - rgb;\n"
    "    float af = factor_s(ac.y, a_local, a_local, a_other, tex.a);\n"
    "    float alpha = func_s(ac.x, af, a_local, a_other);\n"
    "    if (inv.y != 0) alpha = 1.0 - alpha;\n"
    "    rgb = saturate(rgb);\n"
    "    alpha = saturate(alpha);\n"
    // chroma key: compare the color combine unit's "other" input; for textures the nearest texel
    "    if (chroma != 0) {\n"
    "        float3 key = c_other.rgb;\n"
    "        if (cc.w == 1 && textured != 0) {\n"
    "            uint tw, th;\n"
    "            tex0.GetDimensions(tw, th);\n"
    "            float2 ts = float2(tw, th);\n"
    "            key = tex0.Load(int3(gmod(floor(v.st * ts), ts), 0)).rgb;\n"
    "        }\n"
    "        if (all(floor(key * 255.0 + 0.5) == chroma_color.rgb)) discard;\n"
    "    }\n"
    "    int fm = fog & 0xFF;\n"
    "    if (fm != 0) {\n"
    "        float f = 0.0;\n"
    "        if (fm == 1) f = iter.a;\n"
    "        else if (fm == 2) {\n"
    "            float w = 1.0 / max(v.depth.y, 1e-9);\n"
    "            float lw = log2(max(w, 1.0));\n"
    "            float fi = clamp(lw * 4.0, 0.0, 63.0);\n"
    "            int i0 = int(fi);\n"
    "            int i1 = min(i0 + 1, 63);\n"
    "            f = lerp(fog_value(i0), fog_value(i1), fi - float(i0));\n"
    "        }\n"
    "        else if (fm == 3) f = saturate(v.depth.x / 65535.0);\n"
    "        rgb = lerp(rgb, fog_color.rgb, saturate(f));\n"
    "    }\n"
    "    o.color = float4(rgb, alpha);\n"
    "    if (depth_mode == 1 || depth_mode == 3) o.depth = saturate(v.depth.x / 65535.0);\n"
    "    else if (depth_mode == 2 || depth_mode == 4) o.depth = saturate(log2(1.0 / max(v.depth.y, 1e-9) + 1.0) / 32.0);\n"
    "    else o.depth = 0.5;\n"
    "    return o;\n"
    "}\n";

// GlideCB (HLSL packing: 16-byte registers)
typedef struct {
    float screen[2], texscale[2];
    int32_t cc[4], ac[4], tc[4], inv[4];
    float constcolor[4];
    float chroma_color[4];
    float fog_color[4];
    int32_t textured, chroma, fog, depth_mode;
    float fog_table[64];
} glide_cb;

typedef struct {
    int32_t keyed, pad[3];
} quad_cb;


/* ------------------------------------------------------------------ */
/* state                                                               */

static SDL_Window *window;
static HWND hwnd;
static ID3D11Device *device;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *swapchain;
static ID3D11RenderTargetView *swap_rtv;
static int swap_w, swap_h;
static int vsync;

static ID3D11VertexShader *quad_vs, *glide_vs;
static ID3D11PixelShader *quad_ps, *glide_ps;
static ID3D11InputLayout *quad_layout, *glide_layout;
static ID3D11Buffer *quad_vb, *quad_cbuf, *glide_cbuf, *glide_vb;
static UINT glide_vb_size, glide_vb_pos;
static ID3D11RasterizerState *raster;
static ID3D11SamplerState *sampler_linear, *sampler_point;
static ID3D11BlendState *blend_off;
static ID3D11DepthStencilState *depth_off;
static int quad_keyed = -1;
static glide_cb glide_cb_last;
static int glide_cb_valid;

// 2D picture / LFB writes: dynamic BGRA textures
typedef struct {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    int w, h;
} dyn_texture;
static dyn_texture tex_2d, tex_lfb;

// Glide
static int glide_is_open_flag, glide_w, glide_h, glide_scale;
static ID3D11Texture2D *color_tex[2];
static ID3D11RenderTargetView *color_rtv[2];
static ID3D11ShaderResourceView *color_srv[2];
static ID3D11Texture2D *depth_tex;
static ID3D11DepthStencilView *depth_dsv;
static int back_index;
static uint32_t last_glide_present;

// Glide textures: handle = index + 1
typedef struct {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} glide_texture;
static glide_texture *textures;
static int num_textures, max_textures;

// pipeline state caches
#define MAX_CACHE 64
static struct { uint32_t key; ID3D11BlendState *state; } blend_cache[MAX_CACHE];
static int num_blend;
static struct { uint32_t key; ID3D11DepthStencilState *state; } depth_cache[MAX_CACHE];
static int num_depth;
static struct { uint32_t key; ID3D11SamplerState *state; } sampler_cache[MAX_CACHE];
static int num_sampler;

// last presented picture for frame dumps
static uint32_t *last_2d;
static int last_2d_w, last_2d_h;
static uint32_t *last_window;
static int last_window_w, last_window_h;


/* ------------------------------------------------------------------ */
/* helpers                                                             */

typedef HRESULT (WINAPI *d3dcompile_func)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static d3dcompile_func p_D3DCompile;

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = p_D3DCompile(shader_source, sizeof(shader_source) - 1, "i76", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
    {
        eprintf("render_d3d11: shader %s: compile error 0x%08lx\n%s\n", entry, (unsigned long)hr, (errors != NULL) ? (const char *)errors->GetBufferPointer() : "");
        RELEASE(errors);
        return NULL;
    }
    RELEASE(errors);
    return code;
}

static void d3d_drawable_size(int *w, int *h)
{
    SDL_GetWindowSizeInPixels(window, w, h);
}

static int create_swap_rtv(void)
{
    ID3D11Texture2D *back = NULL;
    if (FAILED(swapchain->GetBuffer(0, IID_ID3D11Texture2D, (void **)&back))) return 0;
    HRESULT hr = device->CreateRenderTargetView(back, NULL, &swap_rtv);
    back->Release();
    return SUCCEEDED(hr);
}

// resizes the swap chain buffers to the window size
static void check_swapchain_size(void)
{
    int w, h;
    d3d_drawable_size(&w, &h);
    if ((w <= 0) || (h <= 0) || ((w == swap_w) && (h == swap_h))) return;
    ctx->OMSetRenderTargets(0, NULL, NULL);
    RELEASE(swap_rtv);
    if (FAILED(swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) eprintf("render_d3d11: ResizeBuffers failed\n");
    create_swap_rtv();
    swap_w = w;
    swap_h = h;
}

static void set_viewport(float x, float y, float w, float h)
{
    D3D11_VIEWPORT vp;
    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width = w;
    vp.Height = h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
}

static void upload_dyn_texture(dyn_texture *t, int w, int h, const uint32_t *pixels)
{
    D3D11_MAPPED_SUBRESOURCE m;
    int y;

    if ((t->tex == NULL) || (t->w != w) || (t->h != h))
    {
        D3D11_TEXTURE2D_DESC d;
        RELEASE(t->srv);
        RELEASE(t->tex);
        memset(&d, 0, sizeof(d));
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;     // XRGB/ARGB8888 little endian = B,G,R,A bytes
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateTexture2D(&d, NULL, &t->tex))) return;
        device->CreateShaderResourceView(t->tex, NULL, &t->srv);
        t->w = w;
        t->h = h;
    }
    if (FAILED(ctx->Map(t->tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    for (y = 0; y < h; y++) memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, pixels + (size_t)y * w, (size_t)w * 4);
    ctx->Unmap(t->tex, 0);
}

static void draw_quad(ID3D11ShaderResourceView *srv, ID3D11SamplerState *sampler, int keyed)
{
    UINT stride = 8, offset = 0;

    if (keyed != quad_keyed)
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(quad_cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            quad_cb cb;
            memset(&cb, 0, sizeof(cb));
            cb.keyed = keyed;
            memcpy(m.pData, &cb, sizeof(cb));
            ctx->Unmap(quad_cbuf, 0);
            quad_keyed = keyed;
        }
    }
    ctx->IASetInputLayout(quad_layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 1, &quad_vb, &stride, &offset);
    ctx->VSSetShader(quad_vs, NULL, 0);
    ctx->PSSetShader(quad_ps, NULL, 0);
    ctx->PSSetConstantBuffers(0, 1, &quad_cbuf);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->RSSetState(raster);
    ctx->OMSetBlendState(blend_off, NULL, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(depth_off, 0);
    ctx->Draw(6, 0);
}

// copies a texture (B8G8R8A8 or R8G8B8A8) to memory; returns the mapped staging texture's data in *out (w * h * 4)
static int read_texture(ID3D11Texture2D *src, int w, int h, uint8_t *out)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *staging = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    int y;

    src->GetDesc(&d);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&d, NULL, &staging))) return 0;
    ctx->CopyResource(staging, src);
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
    {
        staging->Release();
        return 0;
    }
    for (y = 0; y < h; y++) memcpy(out + (size_t)y * w * 4, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
    ctx->Unmap(staging, 0);
    staging->Release();
    return 1;
}


/* ------------------------------------------------------------------ */
/* init                                                                */

static uint32_t d3d_window_flags(void)
{
    return 0;
}

static int d3d_init(SDL_Window *w)
{
    static const float quad[12] = { 0,0, 1,0, 0,1, 1,0, 1,1, 0,1 };
    SDL_SysWMinfo info;
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    ID3DBlob *vs1, *ps1, *vs2, *ps2;
    HMODULE compiler;
    const char *s;
    HRESULT hr;

    window = w;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || (info.subsystem != SDL_SYSWM_WINDOWS))
    {
        eprintf("render_d3d11: no Win32 window: %s\n", SDL_GetError());
        return 0;
    }
    hwnd = info.info.win.window;

    compiler = LoadLibraryA("d3dcompiler_47.dll");
    if (compiler == NULL) compiler = LoadLibraryA("d3dcompiler_43.dll");
    p_D3DCompile = (compiler != NULL) ? (d3dcompile_func) GetProcAddress(compiler, "D3DCompile") : NULL;
    if (p_D3DCompile == NULL)
    {
        eprintf("render_d3d11: d3dcompiler_47.dll not available\n");
        return 0;
    }

    d3d_drawable_size(&swap_w, &swap_h);
    memset(&sd, 0, sizeof(sd));
    sd.BufferDesc.Width = swap_w;
    sd.BufferDesc.Height = swap_h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 3, D3D11_SDK_VERSION, &sd, &swapchain, &device, &level, &ctx);
    if (FAILED(hr))
    {
        eprintf("render_d3d11: no hardware device (0x%08lx), trying WARP\n", (unsigned long)hr);
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, 3, D3D11_SDK_VERSION, &sd, &swapchain, &device, &level, &ctx);
    }
    if (FAILED(hr))
    {
        eprintf("render_d3d11: D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n", (unsigned long)hr);
        return 0;
    }
    if (!create_swap_rtv()) return 0;

    // Alt+Enter is handled by SDL (borderless fullscreen)
    {
        IDXGIFactory *factory = NULL;
        if (SUCCEEDED(swapchain->GetParent(IID_IDXGIFactory, (void **)&factory)))
        {
            factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            factory->Release();
        }
    }

    s = config_get("vsync");
    vsync = (s != NULL) ? atoi(s) : 1;

    {
        IDXGIDevice *dxgi_device = NULL;
        IDXGIAdapter *adapter = NULL;
        DXGI_ADAPTER_DESC ad;
        char name[128] = "?";
        if (SUCCEEDED(device->QueryInterface(IID_IDXGIDevice, (void **)&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad)))
        {
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), NULL, NULL);
        }
        RELEASE(adapter);
        RELEASE(dxgi_device);
        if (winapi_debug) eprintf("render_d3d11: %s, feature level %u.%u\n", name, (unsigned)level >> 12, ((unsigned)level >> 8) & 0xF);
    }

    vs1 = compile("quad_vs", "vs_4_0");
    ps1 = compile("quad_ps", "ps_4_0");
    vs2 = compile("glide_vs", "vs_4_0");
    ps2 = compile("glide_ps", "ps_4_0");
    if ((vs1 == NULL) || (ps1 == NULL) || (vs2 == NULL) || (ps2 == NULL)) return 0;

    device->CreateVertexShader(vs1->GetBufferPointer(), vs1->GetBufferSize(), NULL, &quad_vs);
    device->CreatePixelShader(ps1->GetBufferPointer(), ps1->GetBufferSize(), NULL, &quad_ps);
    device->CreateVertexShader(vs2->GetBufferPointer(), vs2->GetBufferSize(), NULL, &glide_vs);
    device->CreatePixelShader(ps2->GetBufferPointer(), ps2->GetBufferSize(), NULL, &glide_ps);
    {
        D3D11_INPUT_ELEMENT_DESC quad_el[1] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };
        // render_glide_vertex: x y z r g b ooz a oow sow tow tmu_oow
        D3D11_INPUT_ELEMENT_DESC glide_el[4] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };
        device->CreateInputLayout(quad_el, 1, vs1->GetBufferPointer(), vs1->GetBufferSize(), &quad_layout);
        device->CreateInputLayout(glide_el, 4, vs2->GetBufferPointer(), vs2->GetBufferSize(), &glide_layout);
    }
    vs1->Release();
    ps1->Release();
    vs2->Release();
    ps2->Release();
    if ((quad_vs == NULL) || (quad_ps == NULL) || (glide_vs == NULL) || (glide_ps == NULL) || (quad_layout == NULL) || (glide_layout == NULL))
    {
        eprintf("render_d3d11: can't create the shaders\n");
        return 0;
    }

    {
        D3D11_BUFFER_DESC bd;
        D3D11_SUBRESOURCE_DATA init;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = sizeof(quad);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        init.pSysMem = quad;
        init.SysMemPitch = 0;
        init.SysMemSlicePitch = 0;
        device->CreateBuffer(&bd, &init, &quad_vb);

        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = sizeof(quad_cb);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, NULL, &quad_cbuf);
        bd.ByteWidth = sizeof(glide_cb);
        device->CreateBuffer(&bd, NULL, &glide_cbuf);

        glide_vb_size = 1024 * 1024;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = glide_vb_size;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, NULL, &glide_vb);
    }

    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;     // w-buffered Glide geometry: depth comes from the shader
        device->CreateRasterizerState(&rd, &raster);
    }
    {
        D3D11_SAMPLER_DESC sd2;
        memset(&sd2, 0, sizeof(sd2));
        sd2.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd2.AddressU = sd2.AddressV = sd2.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd2.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sd2, &sampler_linear);
        sd2.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        device->CreateSamplerState(&sd2, &sampler_point);
    }
    {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof(bd));
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &blend_off);
    }
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof(dd));
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        device->CreateDepthStencilState(&dd, &depth_off);
    }
    return 1;
}

static void d3d_shutdown(void)
{
    int i;
    if (ctx != NULL) ctx->ClearState();
    for (i = 0; i < num_blend; i++) RELEASE(blend_cache[i].state);
    for (i = 0; i < num_depth; i++) RELEASE(depth_cache[i].state);
    for (i = 0; i < num_sampler; i++) RELEASE(sampler_cache[i].state);
    num_blend = num_depth = num_sampler = 0;
    RELEASE(swap_rtv);
    RELEASE(swapchain);
    RELEASE(ctx);
    RELEASE(device);
    window = NULL;
}


/* ------------------------------------------------------------------ */
/* 2D                                                                  */

static void d3d_present_2d(const uint32_t *pixels, int w, int h)
{
    static const float black[4] = { 0, 0, 0, 1 };
    int vx, vy, vw, vh;

    if (device == NULL) return;
    check_swapchain_size();
    upload_dyn_texture(&tex_2d, w, h, pixels);

    ctx->OMSetRenderTargets(1, &swap_rtv, NULL);
    ctx->ClearRenderTargetView(swap_rtv, black);
    render_viewport(w, h, &vx, &vy, &vw, &vh);
    set_viewport((float)vx, (float)vy, (float)vw, (float)vh);
    draw_quad(tex_2d.srv, sampler_linear, 0);
    swapchain->Present(vsync ? 1 : 0, 0);

    if ((last_2d_w != w) || (last_2d_h != h))
    {
        free(last_2d);
        last_2d = (uint32_t *)malloc((size_t)w * h * 4);
        last_2d_w = w;
        last_2d_h = h;
    }
    memcpy(last_2d, pixels, (size_t)w * h * 4);
}

static uint32_t *d3d_read_last(int *w, int *h)
{
    uint32_t *p;

    if (glide_is_open_flag && (last_window != NULL))
    {
        p = (uint32_t *)malloc((size_t)last_window_w * last_window_h * 4);
        memcpy(p, last_window, (size_t)last_window_w * last_window_h * 4);
        *w = last_window_w;
        *h = last_window_h;
        return p;
    }
    if (last_2d == NULL) return NULL;
    p = (uint32_t *)malloc((size_t)last_2d_w * last_2d_h * 4);
    memcpy(p, last_2d, (size_t)last_2d_w * last_2d_h * 4);
    *w = last_2d_w;
    *h = last_2d_h;
    return p;
}


/* ------------------------------------------------------------------ */
/* Glide                                                               */

static void d3d_glide_close(void);

static int d3d_glide_open(int width, int height)
{
    static const float clear0[4] = { 0, 0, 0, 0 };
    D3D11_TEXTURE2D_DESC d;
    const char *s;
    int i, fw, fh;

    if (device == NULL) return 0;
    if (glide_is_open_flag) d3d_glide_close();

    s = config_get("glide_scale");
    glide_scale = (s != NULL) ? atoi(s) : 2;
    if (glide_scale < 1) glide_scale = 1;
    if (glide_scale > 8) glide_scale = 8;

    glide_w = width;
    glide_h = height;
    fw = width * glide_scale;
    fh = height * glide_scale;

    memset(&d, 0, sizeof(d));
    d.Width = fw;
    d.Height = fh;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (i = 0; i < 2; i++)
    {
        if (FAILED(device->CreateTexture2D(&d, NULL, &color_tex[i])) ||
            FAILED(device->CreateRenderTargetView(color_tex[i], NULL, &color_rtv[i])) ||
            FAILED(device->CreateShaderResourceView(color_tex[i], NULL, &color_srv[i])))
        {
            eprintf("render_d3d11: can't create the Glide buffers\n");
            return 0;
        }
        ctx->ClearRenderTargetView(color_rtv[i], clear0);
    }
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&d, NULL, &depth_tex)) || FAILED(device->CreateDepthStencilView(depth_tex, NULL, &depth_dsv)))
    {
        eprintf("render_d3d11: can't create the depth buffer\n");
        return 0;
    }
    ctx->ClearDepthStencilView(depth_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

    back_index = 0;
    glide_is_open_flag = 1;
    glide_cb_valid = 0;
    if (winapi_debug) eprintf("render_d3d11: Glide screen %dx%d, scale %d\n", width, height, glide_scale);
    return 1;
}

static void d3d_glide_close(void)
{
    int i;
    if (!glide_is_open_flag) return;
    ctx->OMSetRenderTargets(0, NULL, NULL);
    for (i = 0; i < 2; i++)
    {
        RELEASE(color_srv[i]);
        RELEASE(color_rtv[i]);
        RELEASE(color_tex[i]);
    }
    RELEASE(depth_dsv);
    RELEASE(depth_tex);
    glide_is_open_flag = 0;
}

static int d3d_glide_is_open(void)
{
    return glide_is_open_flag;
}

static int d3d_glide_texture_create(int w, int h, const uint32_t *rgba)
{
    D3D11_TEXTURE2D_DESC d;
    glide_texture t;
    int i;

    memset(&d, 0, sizeof(d));
    d.Width = w;
    d.Height = h;
    d.MipLevels = 0;            // full chain
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;      // r in the lowest byte
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(device->CreateTexture2D(&d, NULL, &t.tex))) return 0;
    if (FAILED(device->CreateShaderResourceView(t.tex, NULL, &t.srv)))
    {
        t.tex->Release();
        return 0;
    }
    ctx->UpdateSubresource(t.tex, 0, NULL, rgba, w * 4, 0);
    ctx->GenerateMips(t.srv);

    for (i = 0; i < num_textures; i++)
    {
        if (textures[i].tex == NULL) break;
    }
    if (i == num_textures)
    {
        if (num_textures == max_textures)
        {
            max_textures = max_textures ? max_textures * 2 : 256;
            textures = (glide_texture *)realloc(textures, max_textures * sizeof(glide_texture));
        }
        num_textures++;
    }
    textures[i] = t;
    return i + 1;
}

static void d3d_glide_texture_destroy(int texture)
{
    if ((texture <= 0) || (texture > num_textures)) return;
    RELEASE(textures[texture - 1].srv);
    RELEASE(textures[texture - 1].tex);
}

static D3D11_BLEND blend_factor(int f, int is_src, int is_alpha)
{
    switch (f)
    {
        case 0: return D3D11_BLEND_ZERO;
        case 1: return D3D11_BLEND_SRC_ALPHA;
        case 2: return is_src ? (is_alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR) : (is_alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR);
        case 3: return D3D11_BLEND_DEST_ALPHA;
        case 4: return D3D11_BLEND_ONE;
        case 5: return D3D11_BLEND_INV_SRC_ALPHA;
        case 6: return is_src ? (is_alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR) : (is_alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR);
        case 7: return D3D11_BLEND_INV_DEST_ALPHA;
        case 15: return is_src ? D3D11_BLEND_SRC_ALPHA_SAT : D3D11_BLEND_ONE;   // dst: PREFOG_COLOR (approximation)
        default: return is_src ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}

static ID3D11BlendState *get_blend(const render_glide_state *st)
{
    uint32_t key;
    D3D11_BLEND_DESC bd;
    int i;

    if ((st->rgb_src == 4) && (st->rgb_dst == 0) && (st->alpha_src == 4) && (st->alpha_dst == 0)) return blend_off;
    key = (st->rgb_src & 0xFF) | ((st->rgb_dst & 0xFF) << 8) | ((st->alpha_src & 0xFF) << 16) | ((uint32_t)(st->alpha_dst & 0xFF) << 24);
    for (i = 0; i < num_blend; i++)
    {
        if (blend_cache[i].key == key) return blend_cache[i].state;
    }
    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = blend_factor(st->rgb_src, 1, 0);
    bd.RenderTarget[0].DestBlend = blend_factor(st->rgb_dst, 0, 0);
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = blend_factor(st->alpha_src, 1, 1);
    bd.RenderTarget[0].DestBlendAlpha = blend_factor(st->alpha_dst, 0, 1);
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (num_blend == MAX_CACHE)
    {
        RELEASE(blend_cache[0].state);
        memmove(blend_cache, blend_cache + 1, (MAX_CACHE - 1) * sizeof(blend_cache[0]));
        num_blend--;
    }
    blend_cache[num_blend].key = key;
    blend_cache[num_blend].state = NULL;
    device->CreateBlendState(&bd, &blend_cache[num_blend].state);
    return blend_cache[num_blend++].state;
}

static ID3D11DepthStencilState *get_depth(const render_glide_state *st)
{
    uint32_t key;
    D3D11_DEPTH_STENCIL_DESC dd;
    int i, enable, write;

    enable = (st->depth_mode != 0);
    write = enable && st->depth_mask;
    if (!enable) return depth_off;
    key = (st->depth_func & 7) | (write << 3);
    for (i = 0; i < num_depth; i++)
    {
        if (depth_cache[i].key == key) return depth_cache[i].state;
    }
    memset(&dd, 0, sizeof(dd));
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = (D3D11_COMPARISON_FUNC)(D3D11_COMPARISON_NEVER + (st->depth_func & 7));   // GR_CMP_* order = D3D order
    if (num_depth == MAX_CACHE) return depth_off;
    depth_cache[num_depth].key = key;
    depth_cache[num_depth].state = NULL;
    device->CreateDepthStencilState(&dd, &depth_cache[num_depth].state);
    return depth_cache[num_depth++].state;
}

static ID3D11SamplerState *get_sampler(const render_glide_state *st)
{
    uint32_t key;
    D3D11_SAMPLER_DESC sd;
    int i, min_linear, mag_linear;

    min_linear = (st->filter_min == 1);     // GR_TEXTUREFILTER_BILINEAR = 1
    mag_linear = (st->filter_mag == 1);
    key = min_linear | (mag_linear << 1) | ((st->mipmap != 0) << 2) | ((st->clamp_s == 1) << 3) | ((st->clamp_t == 1) << 4);
    for (i = 0; i < num_sampler; i++)
    {
        if (sampler_cache[i].key == key) return sampler_cache[i].state;
    }
    memset(&sd, 0, sizeof(sd));
    // mipmaps: nearest level (like GL_*_MIPMAP_NEAREST in render_gl.c)
    sd.Filter = (D3D11_FILTER)((min_linear ? 0x10 : 0) | (mag_linear ? 0x04 : 0));
    sd.AddressU = (st->clamp_s == 1) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;   // GR_TEXTURECLAMP_CLAMP = 1
    sd.AddressV = (st->clamp_t == 1) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = (st->mipmap != 0) ? D3D11_FLOAT32_MAX : 0.0f;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (num_sampler == MAX_CACHE) return sampler_linear;
    sampler_cache[num_sampler].key = key;
    sampler_cache[num_sampler].state = NULL;
    device->CreateSamplerState(&sd, &sampler_cache[num_sampler].state);
    return sampler_cache[num_sampler++].state;
}

static void bind_back_buffer(void)
{
    ctx->OMSetRenderTargets(1, &color_rtv[back_index], depth_dsv);
    set_viewport(0.0f, 0.0f, (float)(glide_w * glide_scale), (float)(glide_h * glide_scale));
}

static void d3d_glide_draw(const render_glide_state *st, const render_glide_vertex *vertices, int count, int primitive)
{
    glide_cb cb;
    UINT bytes, stride = sizeof(render_glide_vertex), offset;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *sampler;

    if (!glide_is_open_flag || (count <= 0)) return;

    bind_back_buffer();

    memset(&cb, 0, sizeof(cb));
    cb.screen[0] = (float)glide_w;
    cb.screen[1] = (float)glide_h;
    cb.texscale[0] = st->s_scale;
    cb.texscale[1] = st->t_scale;
    cb.cc[0] = st->cc_function; cb.cc[1] = st->cc_factor; cb.cc[2] = st->cc_local; cb.cc[3] = st->cc_other;
    cb.ac[0] = st->ac_function; cb.ac[1] = st->ac_factor; cb.ac[2] = st->ac_local; cb.ac[3] = st->ac_other;
    cb.tc[0] = st->tc_rgb_function; cb.tc[1] = st->tc_rgb_factor; cb.tc[2] = st->tc_alpha_function; cb.tc[3] = st->tc_alpha_factor;
    cb.inv[0] = st->cc_invert; cb.inv[1] = st->ac_invert; cb.inv[2] = st->tc_rgb_invert; cb.inv[3] = st->tc_alpha_invert;
    cb.constcolor[0] = ((st->constant_color >> 16) & 0xFF) / 255.0f;
    cb.constcolor[1] = ((st->constant_color >> 8) & 0xFF) / 255.0f;
    cb.constcolor[2] = (st->constant_color & 0xFF) / 255.0f;
    cb.constcolor[3] = (st->constant_color >> 24) / 255.0f;
    cb.chroma_color[0] = (float)((st->chromakey_value >> 16) & 0xFF);
    cb.chroma_color[1] = (float)((st->chromakey_value >> 8) & 0xFF);
    cb.chroma_color[2] = (float)(st->chromakey_value & 0xFF);
    cb.fog_color[0] = ((st->fog_color >> 16) & 0xFF) / 255.0f;
    cb.fog_color[1] = ((st->fog_color >> 8) & 0xFF) / 255.0f;
    cb.fog_color[2] = (st->fog_color & 0xFF) / 255.0f;
    cb.textured = (st->texture != 0);
    cb.chroma = st->chromakey_enable;
    cb.fog = st->fog_mode;
    cb.depth_mode = st->depth_mode;
    memcpy(cb.fog_table, st->fog_table, sizeof(cb.fog_table));
    if (!glide_cb_valid || (memcmp(&cb, &glide_cb_last, sizeof(cb)) != 0))
    {
        if (SUCCEEDED(ctx->Map(glide_cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            memcpy(m.pData, &cb, sizeof(cb));
            ctx->Unmap(glide_cbuf, 0);
            glide_cb_last = cb;
            glide_cb_valid = 1;
        }
    }

    // vertices: ring buffer (NO_OVERWRITE, DISCARD when full)
    bytes = (UINT)count * sizeof(render_glide_vertex);
    if (bytes > glide_vb_size)
    {
        D3D11_BUFFER_DESC bd;
        RELEASE(glide_vb);
        glide_vb_size = bytes * 2;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = glide_vb_size;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&bd, NULL, &glide_vb))) return;
        glide_vb_pos = glide_vb_size;
    }
    if (glide_vb_pos + bytes > glide_vb_size)
    {
        if (FAILED(ctx->Map(glide_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        glide_vb_pos = 0;
    }
    else
    {
        if (FAILED(ctx->Map(glide_vb, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) return;
    }
    memcpy((uint8_t *)m.pData + glide_vb_pos, vertices, bytes);
    ctx->Unmap(glide_vb, 0);
    offset = glide_vb_pos;
    glide_vb_pos += bytes;

    if ((st->texture > 0) && (st->texture <= num_textures)) srv = textures[st->texture - 1].srv;
    sampler = get_sampler(st);

    ctx->IASetInputLayout(glide_layout);
    ctx->IASetPrimitiveTopology((primitive == 0) ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : ((primitive == 1) ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_POINTLIST));
    ctx->IASetVertexBuffers(0, 1, &glide_vb, &stride, &offset);
    ctx->VSSetShader(glide_vs, NULL, 0);
    ctx->VSSetConstantBuffers(1, 1, &glide_cbuf);
    ctx->PSSetShader(glide_ps, NULL, 0);
    ctx->PSSetConstantBuffers(1, 1, &glide_cbuf);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->RSSetState(raster);
    ctx->OMSetBlendState(get_blend(st), NULL, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(get_depth(st), 0);
    ctx->Draw(count, 0);
}

static void d3d_glide_clear(uint32_t color, uint8_t alpha, uint16_t depth, int color_mask, int depth_mask)
{
    if (!glide_is_open_flag) return;
    if (color_mask)
    {
        float c[4];
        c[0] = ((color >> 16) & 0xFF) / 255.0f;
        c[1] = ((color >> 8) & 0xFF) / 255.0f;
        c[2] = (color & 0xFF) / 255.0f;
        c[3] = alpha / 255.0f;
        ctx->ClearRenderTargetView(color_rtv[back_index], c);
    }
    if (depth_mask) ctx->ClearDepthStencilView(depth_dsv, D3D11_CLEAR_DEPTH, depth / 65535.0f, 0);
}

// presents the Glide front buffer in the window
static void present_front(void)
{
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11ShaderResourceView *none = NULL;
    int vx, vy, vw, vh;

    last_glide_present = SDL_GetTicks();
    check_swapchain_size();
    ctx->OMSetRenderTargets(1, &swap_rtv, NULL);
    ctx->ClearRenderTargetView(swap_rtv, black);
    render_viewport(glide_w, glide_h, &vx, &vy, &vw, &vh);
    set_viewport((float)vx, (float)vy, (float)vw, (float)vh);
    draw_quad(color_srv[back_index ^ 1], sampler_linear, 0);
    ctx->PSSetShaderResources(0, 1, &none);

    if (getenv("I76_DUMP_FRAMES") != NULL)
    {
        // debugging: keep what is actually shown in the window
        ID3D11Texture2D *back = NULL;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_ID3D11Texture2D, (void **)&back)))
        {
            if ((last_window_w != swap_w) || (last_window_h != swap_h))
            {
                free(last_window);
                last_window = (uint32_t *)malloc((size_t)swap_w * swap_h * 4);
                last_window_w = swap_w;
                last_window_h = swap_h;
            }
            read_texture(back, swap_w, swap_h, (uint8_t *)last_window);     // B8G8R8A8 = XRGB8888
            back->Release();
        }
    }
    swapchain->Present(vsync ? 1 : 0, 0);
}

static void d3d_glide_swap(void)
{
    if (!glide_is_open_flag) return;
    back_index ^= 1;
    present_front();
}

static void d3d_glide_refresh(int force)
{
    if (!glide_is_open_flag) return;
    if (force || (SDL_GetTicks() - last_glide_present >= 33)) present_front();
}

static void d3d_glide_read_565(int buffer, uint16_t *dst, int stride_pixels)
{
    int fw, fh, x, y;
    uint8_t *tmp;

    if (!glide_is_open_flag) return;
    fw = glide_w * glide_scale;
    fh = glide_h * glide_scale;
    tmp = (uint8_t *)malloc((size_t)fw * fh * 4);
    if (read_texture(color_tex[(buffer == 1) ? back_index : (back_index ^ 1)], fw, fh, tmp))
    {
        for (y = 0; y < glide_h; y++)
        {
            const uint8_t *row = tmp + (size_t)y * glide_scale * fw * 4;
            for (x = 0; x < glide_w; x++)
            {
                const uint8_t *p = row + (size_t)x * glide_scale * 4;      // R, G, B, A
                dst[(size_t)y * stride_pixels + x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
            }
        }
    }
    free(tmp);
}

static void d3d_glide_write_argb(int buffer, const uint32_t *src)
{
    ID3D11ShaderResourceView *none = NULL;
    if (!glide_is_open_flag) return;
    upload_dyn_texture(&tex_lfb, glide_w, glide_h, src);
    ctx->OMSetRenderTargets(1, &color_rtv[(buffer == 1) ? back_index : (back_index ^ 1)], NULL);
    set_viewport(0.0f, 0.0f, (float)(glide_w * glide_scale), (float)(glide_h * glide_scale));
    draw_quad(tex_lfb.srv, sampler_point, 1);
    ctx->PSSetShaderResources(0, 1, &none);
}

const render_backend render_backend_d3d11 = {
    "Direct3D 11",
    d3d_window_flags, d3d_init, d3d_shutdown, d3d_drawable_size, d3d_present_2d, d3d_read_last,
    d3d_glide_open, d3d_glide_close, d3d_glide_is_open, d3d_glide_texture_create, d3d_glide_texture_destroy,
    d3d_glide_draw, d3d_glide_clear, d3d_glide_swap, d3d_glide_refresh, d3d_glide_read_565, d3d_glide_write_argb
};

#ifdef __cplusplus
}
#endif

#endif
