#version 450
// 2D pictures and LFB writes (keyed: texels with alpha 0 are left unchanged)
layout(set = 1, binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform Push { int keyed; int fxaa; float sharpen; float gamma; vec2 texel; } pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 c = texture(u_tex, v_uv);
    if (pc.keyed != 0 && c.a < 0.5) discard;
    o_color = vec4(c.rgb, 1.0);
}
