/**
 *
 *  Rendering backend: OpenGL 3.3 core (Linux; also usable on Windows).
 *
 *  2D: the GDI framebuffer is uploaded to a texture and drawn letterboxed into the window.
 *  Glide: front/back framebuffer objects at (Glide resolution * I76_GLIDE_SCALE) with a 32-bit float
 *  depth buffer; one shader implements the Glide pixel pipeline (texture combine, color/alpha combine,
 *  chroma key, fog, Z/W depth) from uniforms. grBufferSwap blits the new front buffer to the window.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#define GL_GLEXT_PROTOTYPES 0
#include <GL/glcorearb.h>
#include "render_backend.h"
#include "config.h"
#include "winapi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define eprintf(...) fprintf(stderr,__VA_ARGS__)

/* ------------------------------------------------------------------ */
/* GL function loading                                                 */

#define GL_FUNCTIONS \
    X(PFNGLGETSTRINGPROC, glGetString) \
    X(PFNGLGETERRORPROC, glGetError) \
    X(PFNGLENABLEPROC, glEnable) \
    X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLSCISSORPROC, glScissor) \
    X(PFNGLCLEARCOLORPROC, glClearColor) \
    X(PFNGLCLEARDEPTHPROC, glClearDepth) \
    X(PFNGLCLEARPROC, glClear) \
    X(PFNGLCOLORMASKPROC, glColorMask) \
    X(PFNGLDEPTHMASKPROC, glDepthMask) \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) \
    X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM1FPROC, glUniform1f) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLUNIFORM3FPROC, glUniform3f) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLUNIFORM2IPROC, glUniform2i) \
    X(PFNGLUNIFORM4IPROC, glUniform4i) \
    X(PFNGLUNIFORM1FVPROC, glUniform1fv) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)

#define X(type, name) static type p_##name;
GL_FUNCTIONS
#undef X

static int load_gl(void)
{
#define X(type, name) \
    p_##name = (type) SDL_GL_GetProcAddress(#name); \
    if (p_##name == NULL) { eprintf("render_gl: missing %s\n", #name); return 0; }
    GL_FUNCTIONS
#undef X
    return 1;
}

// call through the loaded pointers
#define glGetString p_glGetString
#define glGetError p_glGetError
#define glEnable p_glEnable
#define glDisable p_glDisable
#define glViewport p_glViewport
#define glScissor p_glScissor
#define glClearColor p_glClearColor
#define glClearDepth p_glClearDepth
#define glClear p_glClear
#define glColorMask p_glColorMask
#define glDepthMask p_glDepthMask
#define glDepthFunc p_glDepthFunc
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glPixelStorei p_glPixelStorei
#define glReadPixels p_glReadPixels
#define glGenTextures p_glGenTextures
#define glDeleteTextures p_glDeleteTextures
#define glBindTexture p_glBindTexture
#define glActiveTexture p_glActiveTexture
#define glTexImage2D p_glTexImage2D
#define glTexSubImage2D p_glTexSubImage2D
#define glTexParameteri p_glTexParameteri
#define glGenerateMipmap p_glGenerateMipmap
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glUniform1f p_glUniform1f
#define glUniform2f p_glUniform2f
#define glUniform3f p_glUniform3f
#define glUniform4f p_glUniform4f
#define glUniform2i p_glUniform2i
#define glUniform4i p_glUniform4i
#define glUniform1fv p_glUniform1fv
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glGenBuffers p_glGenBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glVertexAttribPointer p_glVertexAttribPointer
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDrawArrays p_glDrawArrays
#define glGenFramebuffers p_glGenFramebuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glBlitFramebuffer p_glBlitFramebuffer


/* ------------------------------------------------------------------ */
/* shaders                                                             */

static const char *quad_vs =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"     // 0..1, (0,0) = top left
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_pos;\n"
    "    gl_Position = vec4(a_pos.x * 2.0 - 1.0, 1.0 - a_pos.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char *quad_fs =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_keyed;\n"                    // discard texels with alpha 0 (LFB writes)
    "out vec4 o_color;\n"
    "void main() {\n"
    "    vec4 c = texture(u_tex, v_uv);\n"
    "    if (u_keyed != 0 && c.a < 0.5) discard;\n"
    "    o_color = vec4(c.rgb, 1.0);\n"
    "}\n";

static const char *glide_vs =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_xyz;\n"
    "layout(location = 1) in vec3 a_rgb;\n"
    "layout(location = 2) in vec3 a_ooz_a_oow;\n"
    "layout(location = 3) in vec3 a_tex;\n"     // sow, tow, tmu oow
    "uniform vec2 u_screen;\n"
    "uniform vec2 u_texscale;\n"
    "noperspective out vec4 v_color;\n"
    "noperspective out vec2 v_depth;\n"         // ooz, oow
    "smooth out vec2 v_st;\n"
    "void main() {\n"
    "    float oow = a_ooz_a_oow.z;\n"
    "    float w = (oow > 0.0) ? 1.0 / oow : 1.0;\n"
    "    vec2 ndc = vec2(a_xyz.x / u_screen.x * 2.0 - 1.0, 1.0 - a_xyz.y / u_screen.y * 2.0);\n"
    "    gl_Position = vec4(ndc * w, 0.0, w);\n"
    "    v_color = vec4(a_rgb, a_ooz_a_oow.y) / 255.0;\n"
    "    v_depth = vec2(a_ooz_a_oow.x, oow);\n"
    "    float tw = (a_tex.z > 0.0) ? a_tex.z : ((oow > 0.0) ? oow : 1.0);\n"
    "    v_st = a_tex.xy / tw * u_texscale;\n"
    "    gl_PointSize = 1.0;\n"
    "}\n";

static const char *glide_fs =
    "#version 330 core\n"
    "noperspective in vec4 v_color;\n"
    "noperspective in vec2 v_depth;\n"
    "smooth in vec2 v_st;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_textured;\n"
    "uniform ivec4 u_cc;\n"                     // function, factor, local, other
    "uniform ivec4 u_ac;\n"
    "uniform ivec4 u_tc;\n"                     // rgb function, rgb factor, alpha function, alpha factor
    "uniform ivec4 u_inv;\n"                    // cc invert, ac invert, tc rgb invert, tc alpha invert
    "uniform vec4 u_const;\n"
    "uniform int u_chroma;\n"
    "uniform vec3 u_chroma_color;\n"
    "uniform int u_fog;\n"
    "uniform vec3 u_fog_color;\n"
    "uniform float u_fog_table[64];\n"
    "uniform int u_depth_mode;\n"
    "out vec4 o_color;\n"
    "\n"
    "float factor_s(int f, float local, float local_a, float other_a, float tex_a) {\n"
    "    if (f == 0) return 0.0;\n"
    "    if (f == 1) return local;\n"
    "    if (f == 2) return other_a;\n"
    "    if (f == 3) return local_a;\n"
    "    if (f == 4) return tex_a;\n"
    "    if (f == 5) return 0.0;\n"             // LOD fraction
    "    if (f == 9) return 1.0 - local;\n"
    "    if (f == 10) return 1.0 - other_a;\n"
    "    if (f == 11) return 1.0 - local_a;\n"
    "    if (f == 12) return 1.0 - tex_a;\n"
    "    return 1.0;\n"                         // ONE, ONE_MINUS_LOD_FRACTION
    "}\n"
    "vec3 factor_v(int f, vec3 local, float local_a, float other_a, float tex_a) {\n"
    "    if (f == 1) return local;\n"
    "    if (f == 9) return vec3(1.0) - local;\n"
    "    return vec3(factor_s(f, 0.0, local_a, other_a, tex_a));\n"
    "}\n"
    "vec3 func_v(int fn, vec3 f, vec3 local, float local_a, vec3 other) {\n"
    "    if (fn == 0) return vec3(0.0);\n"
    "    if (fn == 1) return local;\n"
    "    if (fn == 2) return vec3(local_a);\n"
    "    if (fn == 3) return f * other;\n"
    "    if (fn == 4) return f * other + local;\n"
    "    if (fn == 5) return f * other + vec3(local_a);\n"
    "    if (fn == 6) return f * (other - local);\n"
    "    if (fn == 7) return f * (other - local) + local;\n"
    "    if (fn == 8) return f * (other - local) + vec3(local_a);\n"
    "    if (fn == 9) return f * (-local) + local;\n"
    "    if (fn == 16) return f * (-local) + vec3(local_a);\n"
    "    return vec3(0.0);\n"
    "}\n"
    "float func_s(int fn, float f, float local, float other) {\n"
    "    return func_v(fn, vec3(f), vec3(local), local, vec3(other)).x;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec4 iter = clamp(v_color, 0.0, 1.0);\n"
    "    vec4 tex = vec4(1.0);\n"
    "    if (u_textured != 0) {\n"
    "        vec4 t = texture(u_tex, v_st);\n"
    // TMU combine: local = texel, other = upstream TMU (none = 0)
    "        vec3 frgb = factor_v(u_tc.y, t.rgb, t.a, 0.0, 0.0);\n"
    "        tex.rgb = func_v(u_tc.x, frgb, t.rgb, t.a, vec3(0.0));\n"
    "        float fa = factor_s(u_tc.w, t.a, t.a, 0.0, 0.0);\n"
    "        tex.a = func_s(u_tc.z, fa, t.a, 0.0);\n"
    "        if (u_inv.z != 0) tex.rgb = vec3(1.0) - tex.rgb;\n"
    "        if (u_inv.w != 0) tex.a = 1.0 - tex.a;\n"
    "        tex = clamp(tex, 0.0, 1.0);\n"
    "    }\n"
    // alpha combine inputs
    "    float a_local = (u_ac.z == 0) ? iter.a : ((u_ac.z == 1) ? u_const.a : clamp(v_depth.x / 65535.0, 0.0, 1.0));\n"
    "    float a_other = (u_ac.w == 0) ? iter.a : ((u_ac.w == 1) ? tex.a : u_const.a);\n"
    // color combine inputs
    "    vec3 c_local = (u_cc.z == 1) ? u_const.rgb : iter.rgb;\n"
    "    vec4 c_other = (u_cc.w == 0) ? iter : ((u_cc.w == 1) ? tex : u_const);\n"
    "    vec3 cf = factor_v(u_cc.y, c_local, a_local, c_other.a, tex.a);\n"
    "    vec3 rgb = func_v(u_cc.x, cf, c_local, a_local, c_other.rgb);\n"
    "    if (u_inv.x != 0) rgb = vec3(1.0) - rgb;\n"
    "    float af = factor_s(u_ac.y, a_local, a_local, a_other, tex.a);\n"
    "    float alpha = func_s(u_ac.x, af, a_local, a_other);\n"
    "    if (u_inv.y != 0) alpha = 1.0 - alpha;\n"
    "    rgb = clamp(rgb, 0.0, 1.0);\n"
    "    alpha = clamp(alpha, 0.0, 1.0);\n"
    // chroma key: the Voodoo compares the color combine unit's "other" input; for textures the
    // nearest texel is compared so that bilinear filtering doesn't leave key-colored fringes
    "    if (u_chroma != 0) {\n"
    "        vec3 key = c_other.rgb;\n"
    "        if (u_cc.w == 1 && u_textured != 0) {\n"
    "            ivec2 ts = textureSize(u_tex, 0);\n"
    "            key = texelFetch(u_tex, ivec2(mod(floor(v_st * vec2(ts)), vec2(ts))), 0).rgb;\n"
    "        }\n"
    "        if (all(equal(floor(key * 255.0 + 0.5), u_chroma_color))) discard;\n"
    "    }\n"
    // fog
    "    int fm = u_fog & 0xFF;\n"
    "    if (fm != 0) {\n"
    "        float f = 0.0;\n"
    "        if (fm == 1) f = iter.a;\n"
    "        else if (fm == 2) {\n"
    "            float w = 1.0 / max(v_depth.y, 1e-9);\n"
    // table index i covers w = 2^(3+(i>>2)) / (8-(i&3)) (guFogTableIndexToW)
    "            float lw = log2(max(w, 1.0));\n"
    "            float fi = clamp((lw - 0.0) * 4.0, 0.0, 63.0);\n"
    "            int i0 = int(fi);\n"
    "            int i1 = min(i0 + 1, 63);\n"
    "            f = mix(u_fog_table[i0], u_fog_table[i1], fi - float(i0));\n"
    "        }\n"
    "        else if (fm == 3) f = clamp(v_depth.x / 65535.0, 0.0, 1.0);\n"
    "        rgb = mix(rgb, u_fog_color, clamp(f, 0.0, 1.0));\n"
    "    }\n"
    "    o_color = vec4(rgb, alpha);\n"
    "    if (u_depth_mode == 1 || u_depth_mode == 3) gl_FragDepth = clamp(v_depth.x / 65535.0, 0.0, 1.0);\n"
    "    else if (u_depth_mode == 2 || u_depth_mode == 4) gl_FragDepth = clamp(log2(1.0 / max(v_depth.y, 1e-9) + 1.0) / 32.0, 0.0, 1.0);\n"
    "    else gl_FragDepth = 0.5;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        eprintf("render_gl: shader compile error:\n%s\n", log);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *vs, const char *fs)
{
    GLuint p, v, f;
    GLint ok;
    v = compile(GL_VERTEX_SHADER, vs);
    f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        eprintf("render_gl: link error:\n%s\n", log);
        return 0;
    }
    return p;
}


/* ------------------------------------------------------------------ */
/* state                                                               */

static SDL_Window *window;
static SDL_GLContext context;
static GLuint quad_prog, quad_vao, quad_vbo, tex_2d;
static int tex_2d_w, tex_2d_h;
static GLint u_quad_tex, u_quad_keyed;

static GLuint glide_prog, glide_vao, glide_vbo;
static struct {
    GLint screen, texscale, tex, textured, cc, ac, tc, inv, constant, chroma, chroma_color, fog, fog_color, fog_table, depth_mode;
} u;

static int glide_open, glide_w, glide_h, glide_scale;
static GLuint fbo[2], fbo_color[2], fbo_depth;  // fbo[back], fbo[front]
static int back_index;                          // index of the back buffer in fbo[]
static GLuint lfb_tex;

// last presented picture for frame dumps
static uint32_t *last_2d;
static int last_2d_w, last_2d_h;
static uint32_t *last_window;
static int last_window_w, last_window_h;

static void gl_glide_close(void);

static uint32_t gl_window_flags(void)
{
    return SDL_WINDOW_OPENGL;
}

static int gl_init(SDL_Window *w)
{
    static const float quad[12] = { 0,0, 1,0, 0,1, 1,0, 1,1, 0,1 };
    const char *vsync;

    window = w;
    context = SDL_GL_CreateContext(window);
    if (context == NULL)
    {
        eprintf("render_gl: can't create an OpenGL 3.3 context: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_MakeCurrent(window, context);
    if (!load_gl()) return 0;

    vsync = config_get("vsync");
    SDL_GL_SetSwapInterval((vsync != NULL) ? atoi(vsync) : 1);

    if (winapi_debug) eprintf("render_gl: %s / %s / %s (SDL video driver %s)\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION), SDL_GetCurrentVideoDriver());

    quad_prog = link_program(quad_vs, quad_fs);
    glide_prog = link_program(glide_vs, glide_fs);
    if (!quad_prog || !glide_prog) return 0;

    u_quad_tex = glGetUniformLocation(quad_prog, "u_tex");
    u_quad_keyed = glGetUniformLocation(quad_prog, "u_keyed");

#define U(name) u.name = glGetUniformLocation(glide_prog, "u_" #name)
    U(screen); U(texscale); U(tex); U(textured); U(cc); U(ac); U(tc); U(inv);
    u.constant = glGetUniformLocation(glide_prog, "u_const");
    U(chroma); U(chroma_color); U(fog); U(fog_color); U(fog_table); U(depth_mode);
#undef U

    glGenVertexArrays(1, &quad_vao);
    glBindVertexArray(quad_vao);
    glGenBuffers(1, &quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
    glEnableVertexAttribArray(0);

    glGenVertexArrays(1, &glide_vao);
    glBindVertexArray(glide_vao);
    glGenBuffers(1, &glide_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, glide_vbo);
    // render_glide_vertex: x y z r g b ooz a oow sow tow tmu_oow
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(render_glide_vertex), (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(render_glide_vertex), (void *)12);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(render_glide_vertex), (void *)24);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(render_glide_vertex), (void *)36);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glBindVertexArray(0);

    glGenTextures(1, &tex_2d);
    glGenTextures(1, &lfb_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    return 1;
}

static void gl_shutdown(void)
{
    if (context != NULL) SDL_GL_DeleteContext(context);
    context = NULL;
    window = NULL;
}

static void gl_drawable_size(int *w, int *h)
{
    SDL_GL_GetDrawableSize(window, w, h);
}

static void upload_texture(GLuint tex, int *tw, int *th, int w, int h, GLenum format, const void *pixels, GLint filter)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if ((*tw != w) || (*th != h))
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, format, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        *tw = w;
        *th = h;
    }
    else
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, GL_UNSIGNED_BYTE, pixels);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

static void draw_quad(GLuint tex, int keyed)
{
    glUseProgram(quad_prog);
    glUniform1i(u_quad_tex, 0);
    glUniform1i(u_quad_keyed, keyed);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

static void gl_present_2d(const uint32_t *pixels, int w, int h)
{
    int vx, vy, vw, vh, ww, wh;

    if (context == NULL) return;

    // XRGB8888 little endian = B,G,R,X bytes
    upload_texture(tex_2d, &tex_2d_w, &tex_2d_h, w, h, GL_BGRA, pixels, GL_LINEAR);

    SDL_GL_GetDrawableSize(window, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    render_viewport(w, h, &vx, &vy, &vw, &vh);
    glViewport(vx, wh - vy - vh, vw, vh);
    draw_quad(tex_2d, 0);
    SDL_GL_SwapWindow(window);

    if ((last_2d_w != w) || (last_2d_h != h))
    {
        free(last_2d);
        last_2d = (uint32_t *)malloc((size_t)w * h * 4);
        last_2d_w = w;
        last_2d_h = h;
    }
    memcpy(last_2d, pixels, (size_t)w * h * 4);
}

static uint32_t *gl_read_last(int *w, int *h)
{
    uint32_t *p;
    int y;

    if (glide_open && (last_window != NULL))
    {
        p = (uint32_t *)malloc((size_t)last_window_w * last_window_h * 4);
        memcpy(p, last_window, (size_t)last_window_w * last_window_h * 4);
        *w = last_window_w;
        *h = last_window_h;
        return p;
    }
    if (glide_open)
    {
        int fw = glide_w * glide_scale, fh = glide_h * glide_scale;
        uint32_t *tmp = (uint32_t *)malloc((size_t)fw * fh * 4);
        p = (uint32_t *)malloc((size_t)fw * fh * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[back_index ^ 1]);
        glReadPixels(0, 0, fw, fh, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        for (y = 0; y < fh; y++) memcpy(p + (size_t)y * fw, tmp + (size_t)(fh - 1 - y) * fw, (size_t)fw * 4);
        free(tmp);
        *w = fw;
        *h = fh;
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

static int gl_glide_open(int width, int height)
{
    int i, fw, fh;
    const char *s;

    if (context == NULL) return 0;
    if (glide_open) gl_glide_close();

    s = config_get("glide_scale");
    glide_scale = (s != NULL) ? atoi(s) : 2;
    if (glide_scale < 1) glide_scale = 1;
    if (glide_scale > 8) glide_scale = 8;

    glide_w = width;
    glide_h = height;
    fw = width * glide_scale;
    fh = height * glide_scale;

    glGenTextures(2, fbo_color);
    glGenTextures(1, &fbo_depth);
    glGenFramebuffers(2, fbo);

    glBindTexture(GL_TEXTURE_2D, fbo_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, fw, fh, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    for (i = 0; i < 2; i++)
    {
        glBindTexture(GL_TEXTURE_2D, fbo_color[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_color[i], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, fbo_depth, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            eprintf("render_gl: incomplete framebuffer\n");
            return 0;
        }
        glViewport(0, 0, fw, fh);
        glClearColor(0, 0, 0, 0);
        glClearDepth(1.0);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    back_index = 0;
    glide_open = 1;

    if (winapi_debug) eprintf("render_gl: Glide screen %dx%d, scale %d\n", width, height, glide_scale);
    return 1;
}

static void gl_glide_close(void)
{
    if (!glide_open) return;
    glDeleteFramebuffers(2, fbo);
    glDeleteTextures(2, fbo_color);
    glDeleteTextures(1, &fbo_depth);
    glide_open = 0;
}

static int gl_glide_is_open(void)
{
    return glide_open;
}

static int gl_glide_texture_create(int w, int h, const uint32_t *rgba)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    return (int)tex;
}

static void gl_glide_texture_destroy(int texture)
{
    GLuint tex = (GLuint)texture;
    if (tex != 0) glDeleteTextures(1, &tex);
}

static GLenum blend_factor(int f, int is_src)
{
    switch (f)
    {
        case 0: return GL_ZERO;
        case 1: return GL_SRC_ALPHA;
        case 2: return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
        case 3: return GL_DST_ALPHA;
        case 4: return GL_ONE;
        case 5: return GL_ONE_MINUS_SRC_ALPHA;
        case 6: return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
        case 7: return GL_ONE_MINUS_DST_ALPHA;
        case 15: return is_src ? GL_SRC_ALPHA_SATURATE : GL_ONE;   // dst: PREFOG_COLOR (approximation)
        default: return is_src ? GL_ONE : GL_ZERO;
    }
}

static void bind_back_buffer(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[back_index]);
    glViewport(0, 0, glide_w * glide_scale, glide_h * glide_scale);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
}

static void gl_glide_draw(const render_glide_state *st, const render_glide_vertex *vertices, int count, int primitive)
{
    if (!glide_open || (count <= 0)) return;

    bind_back_buffer();

    if ((st->rgb_src == 4) && (st->rgb_dst == 0) && (st->alpha_src == 4) && (st->alpha_dst == 0))
    {
        glDisable(GL_BLEND);
    }
    else
    {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(blend_factor(st->rgb_src, 1), blend_factor(st->rgb_dst, 0), blend_factor(st->alpha_src, 1), blend_factor(st->alpha_dst, 0));
    }

    if (st->depth_mode != 0)
    {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_NEVER + (st->depth_func & 7));
    }
    else
    {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask((st->depth_mode != 0) && st->depth_mask ? GL_TRUE : GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(glide_prog);
    glUniform2f(u.screen, (float)glide_w, (float)glide_h);
    glUniform2f(u.texscale, st->s_scale, st->t_scale);
    glUniform1i(u.tex, 0);
    glUniform1i(u.textured, st->texture != 0);
    glUniform4i(u.cc, st->cc_function, st->cc_factor, st->cc_local, st->cc_other);
    glUniform4i(u.ac, st->ac_function, st->ac_factor, st->ac_local, st->ac_other);
    glUniform4i(u.tc, st->tc_rgb_function, st->tc_rgb_factor, st->tc_alpha_function, st->tc_alpha_factor);
    glUniform4i(u.inv, st->cc_invert, st->ac_invert, st->tc_rgb_invert, st->tc_alpha_invert);
    glUniform4f(u.constant, ((st->constant_color >> 16) & 0xFF) / 255.0f, ((st->constant_color >> 8) & 0xFF) / 255.0f, (st->constant_color & 0xFF) / 255.0f, (st->constant_color >> 24) / 255.0f);
    glUniform1i(u.chroma, st->chromakey_enable);
    glUniform3f(u.chroma_color, (float)((st->chromakey_value >> 16) & 0xFF), (float)((st->chromakey_value >> 8) & 0xFF), (float)(st->chromakey_value & 0xFF));
    glUniform1i(u.fog, st->fog_mode);
    glUniform3f(u.fog_color, ((st->fog_color >> 16) & 0xFF) / 255.0f, ((st->fog_color >> 8) & 0xFF) / 255.0f, (st->fog_color & 0xFF) / 255.0f);
    glUniform1fv(u.fog_table, 64, st->fog_table);
    glUniform1i(u.depth_mode, st->depth_mode);

    if (st->texture != 0)
    {
        GLint mag = (st->filter_mag == 1) ? GL_LINEAR : GL_NEAREST;     // GR_TEXTUREFILTER_BILINEAR = 1
        GLint min;
        if (st->mipmap == 0) min = (st->filter_min == 1) ? GL_LINEAR : GL_NEAREST;
        else min = (st->filter_min == 1) ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)st->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (st->clamp_s == 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);   // GR_TEXTURECLAMP_CLAMP = 1
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (st->clamp_t == 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }

    glBindVertexArray(glide_vao);
    glBindBuffer(GL_ARRAY_BUFFER, glide_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * sizeof(render_glide_vertex), vertices, GL_STREAM_DRAW);
    glDrawArrays((primitive == 0) ? GL_TRIANGLES : ((primitive == 1) ? GL_LINES : GL_POINTS), 0, count);
    glBindVertexArray(0);
}

static void gl_glide_clear(uint32_t color, uint8_t alpha, uint16_t depth, int color_mask, int depth_mask)
{
    GLbitfield bits = 0;
    if (!glide_open) return;
    bind_back_buffer();
    glDisable(GL_BLEND);
    if (color_mask)
    {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f, alpha / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (depth_mask)
    {
        glDepthMask(GL_TRUE);
        glClearDepth(depth / 65535.0);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (bits) glClear(bits);
}

static uint32_t last_glide_present;

// presents the Glide front buffer in the window
static void present_front(void)
{
    int vx, vy, vw, vh, ww, wh;

    last_glide_present = SDL_GetTicks();
    SDL_GL_GetDrawableSize(window, &ww, &wh);
    render_viewport(glide_w, glide_h, &vx, &vy, &vw, &vh);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[back_index ^ 1]);
    glBlitFramebuffer(0, 0, glide_w * glide_scale, glide_h * glide_scale, vx, wh - vy - vh, vx + vw, wh - vy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (getenv("I76_DUMP_FRAMES") != NULL)
    {
        // debugging: keep what is actually shown in the window
        int y;
        uint32_t *tmp = (uint32_t *)malloc((size_t)ww * wh * 4);
        glReadPixels(0, 0, ww, wh, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
        if ((last_window_w != ww) || (last_window_h != wh))
        {
            free(last_window);
            last_window = (uint32_t *)malloc((size_t)ww * wh * 4);
            last_window_w = ww;
            last_window_h = wh;
        }
        for (y = 0; y < wh; y++) memcpy(last_window + (size_t)y * ww, tmp + (size_t)(wh - 1 - y) * ww, (size_t)ww * 4);
        free(tmp);
    }
    SDL_GL_SwapWindow(window);
}

static void gl_glide_swap(void)
{
    if (!glide_open) return;
    back_index ^= 1;
    present_front();
}

static void gl_glide_refresh(int force)
{
    // the game doesn't swap while it waits for input (e.g. the in-game menu); some compositors only show
    // a frame once the next one arrives, so keep presenting the front buffer
    if (!glide_open) return;
    if (force || (SDL_GetTicks() - last_glide_present >= 33)) present_front();
}

static void gl_glide_read_565(int buffer, uint16_t *dst, int stride_pixels)
{
    int fw, fh, x, y;
    uint8_t *tmp;

    if (!glide_open) return;
    fw = glide_w * glide_scale;
    fh = glide_h * glide_scale;
    tmp = (uint8_t *)malloc((size_t)fw * fh * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[(buffer == 1) ? back_index : (back_index ^ 1)]);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    for (y = 0; y < glide_h; y++)
    {
        const uint8_t *row = tmp + (size_t)(fh - 1 - y * glide_scale) * fw * 4;
        for (x = 0; x < glide_w; x++)
        {
            const uint8_t *p = row + (size_t)x * glide_scale * 4;
            dst[(size_t)y * stride_pixels + x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
    free(tmp);
}

static void gl_glide_write_argb(int buffer, const uint32_t *src)
{
    static int lfb_w, lfb_h;
    if (!glide_open) return;
    upload_texture(lfb_tex, &lfb_w, &lfb_h, glide_w, glide_h, GL_BGRA, src, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[(buffer == 1) ? back_index : (back_index ^ 1)]);
    glViewport(0, 0, glide_w * glide_scale, glide_h * glide_scale);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    draw_quad(lfb_tex, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

const render_backend render_backend_gl = {
    "OpenGL",
    gl_window_flags, gl_init, gl_shutdown, gl_drawable_size, gl_present_2d, gl_read_last,
    gl_glide_open, gl_glide_close, gl_glide_is_open, gl_glide_texture_create, gl_glide_texture_destroy,
    gl_glide_draw, gl_glide_clear, gl_glide_swap, gl_glide_refresh, gl_glide_read_565, gl_glide_write_argb
};

#ifdef __cplusplus
}
#endif
