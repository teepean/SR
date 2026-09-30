#version 450
// Glide vertices: screen coordinates (Glide resolution), rgb 0..255, ooz, alpha 0..255, oow, sow/tow/tmu oow
layout(set = 0, binding = 0) uniform Glide {
    vec2 screen; vec2 texscale;
    ivec4 cc; ivec4 ac; ivec4 tc; ivec4 inv;
    vec4 constcolor;
    vec4 chroma_color;
    vec4 fog_color;
    int textured; int chroma; int fog; int depth_mode;
    vec4 fog_table[16];
} u;
layout(location = 0) in vec3 a_xyz;
layout(location = 1) in vec3 a_rgb;
layout(location = 2) in vec3 a_ooz_a_oow;
layout(location = 3) in vec3 a_tex;
layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec2 v_depth;
layout(location = 2) out vec2 v_st;
void main() {
    float oow = a_ooz_a_oow.z;
    float w = (oow > 0.0) ? 1.0 / oow : 1.0;
    vec2 ndc = vec2(a_xyz.x / u.screen.x * 2.0 - 1.0, a_xyz.y / u.screen.y * 2.0 - 1.0);
    gl_Position = vec4(ndc * w, 0.0, w);
    gl_PointSize = 1.0;
    v_color = vec4(a_rgb, a_ooz_a_oow.y) / 255.0;
    v_depth = vec2(a_ooz_a_oow.x, oow);
    float tw = (a_tex.z > 0.0) ? a_tex.z : ((oow > 0.0) ? oow : 1.0);
    v_st = a_tex.xy / tw * u.texscale;
}
