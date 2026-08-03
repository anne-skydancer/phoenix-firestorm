#version 450
// Ground P3: terrain -> deferred G-buffer with the 4-detail splat. World-space mesh
// (pos + normal + texcoord1). texcoord1 = (composition band [0,3], per-vertex noise [0,1]), computed
// engine-side from stock generateHeights/eval (terrain_noise). Detail UV = planar texgen matching
// stock OBJECT_PLANE_S/T: pos.xy*detail_scale + offset (offset = fmod(region_origin_global, tile)).
layout(location = 0) in vec3 pos;      // world/agent-space vertex
layout(location = 1) in vec3 normal;   // world/agent-space normal
layout(location = 2) in vec2 tc1;      // .x = composition [0,3], .y = noise [0,1]
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj;   // world -> clip (reverse-Z)
    vec4 sun_dir;     // unused in the G-buffer fill
    vec4 sunlight;    // unused
    vec4 ambient;     // unused
    vec4 misc;        // .x = detail_scale, .y = offset_x, .z = offset_y
} u;
layout(location = 0) out vec3 v_normal;    // world-space (normalized in the fragment)
layout(location = 1) out vec2 v_detail_uv; // detail-texture UV
layout(location = 2) out vec2 v_comp;      // .x = composition, .y = noise
void main() {
    v_normal = normal;
    v_detail_uv = pos.xy * u.misc.x + vec2(u.misc.y, u.misc.z);
    v_comp = tc1;
    gl_Position = u.view_proj * vec4(pos, 1.0);
}
