layout(set = 0, binding = 0) uniform Glide {
    vec2 screen; vec2 texscale;
    ivec4 cc; ivec4 ac; ivec4 tc; ivec4 inv;
    vec4 constcolor;
    vec4 chroma_color;
    vec4 fog_color;
    int textured; int chroma; int fog; int depth_mode;
    vec4 fog_table[16];
} u;
