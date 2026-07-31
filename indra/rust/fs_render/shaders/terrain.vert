#version 450
// Faithful port of class1/deferred/terrainV.glsl for the live bridge.
// Detail UVs are planar texgen from OBJECT-SPACE position (dot with the shipped
// object_plane_s/t -- scale + per-region offsets baked in by the viewer);
// ramp coords come from TEXCOORD1 with the -(2,0)/-(1,0) offset trio.
layout(location = 0) in vec4 pos;
layout(location = 1) in vec2 uv1; // TEXCOORD1: parcel composition coords
layout(set = 0, binding = 0) uniform U {
    mat4 mvp;
    vec4 color_u;
    vec4 plane_s;
    vec4 plane_t;
    mat4 texmat; // unused by terrain (identity in the main pass)
    vec4 khr_sr;
    vec4 khr_off;
    vec4 flags;
} u;
layout(location = 0) out vec2 v_detail;
layout(location = 1) out vec4 v_ramp01;  // .xy = tc (alpha1), .zw = tc-(2,0) (alpha2)
layout(location = 2) out vec2 v_rampF;   // tc-(1,0) (alphaFinal)
void main() {
    vec4 p = vec4(pos.xyz, 1.0);
    v_detail = vec2(dot(p, u.plane_s), dot(p, u.plane_t));
    v_ramp01.xy = uv1;
    v_ramp01.zw = uv1 - vec2(2.0, 0.0);
    v_rampF = uv1 - vec2(1.0, 0.0);
    gl_Position = u.mvp * p;
}
