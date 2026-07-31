#version 450
layout(location = 0) in vec4 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(set = 0, binding = 0) uniform U {
    mat4 mvp; vec4 color_u; vec4 plane_s; vec4 plane_t; mat4 texmat;
    vec4 khr_sr;   // KHR scale.xy, rotation, _
    vec4 khr_off;  // KHR offset.xy
    vec4 flags;    // .x = blending enabled at draw time
} u;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out int v_texidx;

// Faithful to textureUtilV.glsl texture_transform(): SL anim matrix first, then the
// KHR transform (offset_mat * rotation_mat * scale_mat) in a y-flipped frame.
// GLTF faces ship RAW planar UVs (llface.cpp:1545 skips CPU baking) -- their scale
// lives entirely in the KHR term.
vec2 apply_uv_transforms(vec2 tc) {
    tc = (u.texmat * vec4(tc, 0.0, 1.0)).xy;
    tc.y = 1.0 - tc.y;
    float c = cos(u.khr_sr.z);
    float s = sin(u.khr_sr.z);
    mat3 scale_mat = mat3(u.khr_sr.x,0,0, 0,u.khr_sr.y,0, 0,0,1);
    mat3 offset_mat = mat3(1,0,0, 0,1,0, u.khr_off.x, u.khr_off.y, 1);
    mat3 rotation_mat = mat3(c,-s,0, s,c,0, 0,0,1);
    tc = (offset_mat * rotation_mat * scale_mat * vec3(tc, 1.0)).xy;
    tc.y = 1.0 - tc.y;
    return tc;
}

void main() {
    v_uv = apply_uv_transforms(uv);
    v_color = color;
    // position.w carries the batch texture index as a RAW INTEGER BIT PATTERN
    // (llface.cpp:2111 writes *(S32*)&val = index) -- floatBitsToInt, never int().
    // Only INDEXED programs pack a texture index in position.w; for PBR/materials/
    // avatar draws .w is real data and indexing it sampled stale neighbor units
    // (the settings-change texture swap).
    v_texidx = (u.flags.y > 0.5) ? clamp(floatBitsToInt(pos.w), 0, 3) : 0;
    gl_Position = u.mvp * vec4(pos.xyz, 1.0);
}
