#version 450
// post-processing of the presented 3D picture: FXAA (compact variant of Timothy Lottes' FXAA),
// sharpening (unsharp mask against the 4 neighbours), gamma
layout(set = 1, binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform Push { int keyed; int fxaa; float sharpen; float gamma; vec2 texel; } pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
vec3 fetch(vec2 uv) { return texture(u_tex, uv).rgb; }
vec3 fxaa(vec2 uv) {
    vec2 t = pc.texel;
    vec3 luma = vec3(0.299, 0.587, 0.114);
    vec3 rgbM = fetch(uv);
    float lNW = dot(fetch(uv + vec2(-1.0, -1.0) * t), luma);
    float lNE = dot(fetch(uv + vec2(1.0, -1.0) * t), luma);
    float lSW = dot(fetch(uv + vec2(-1.0, 1.0) * t), luma);
    float lSE = dot(fetch(uv + vec2(1.0, 1.0) * t), luma);
    float lM = dot(rgbM, luma);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, vec2(-8.0), vec2(8.0)) * t;
    vec3 rgbA = 0.5 * (fetch(uv + dir * (1.0 / 3.0 - 0.5)) + fetch(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 rgbB = rgbA * 0.5 + 0.25 * (fetch(uv - dir * 0.5) + fetch(uv + dir * 0.5));
    float lB = dot(rgbB, luma);
    return ((lB < lMin) || (lB > lMax)) ? rgbA : rgbB;
}
void main() {
    vec3 c = (pc.fxaa != 0) ? fxaa(v_uv) : fetch(v_uv);
    if (pc.sharpen > 0.0) {
        vec3 blur = 0.25 * (fetch(v_uv + vec2(pc.texel.x, 0.0)) + fetch(v_uv - vec2(pc.texel.x, 0.0)) + fetch(v_uv + vec2(0.0, pc.texel.y)) + fetch(v_uv - vec2(0.0, pc.texel.y)));
        c = clamp(c + (c - blur) * pc.sharpen * 2.0, 0.0, 1.0);
    }
    if (pc.gamma != 1.0) c = pow(c, vec3(1.0 / pc.gamma));
    o_color = vec4(c, 1.0);
}
