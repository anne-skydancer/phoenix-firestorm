#version 450
// Ground P2a: write the terrain G-buffer. Faithful to stock legacy terrainF.glsl's contract:
//   RT0 = albedo, sRGB-ENCODED, with the FLAG in .a (HAS_ATMOS 0.34). The resolve srgb_to_linear's it,
//         exactly as softenLightF does ("legacy shaders are still writing sRGB to gbuffer").
//   RT1 = world-space normal (raw -- our normal RT is RGBA16F, so no spheremap encode is needed),
//         .w = env intensity (0 for terrain).
// Flat placeholder albedo for now; the 4-detail splat is P3.
layout(location = 0) in vec3 v_normal;
layout(location = 0) out vec4 rt0;  // albedo.rgb (sRGB) + flag.a
layout(location = 1) out vec4 rt1;  // world normal.xyz + env.w
const float GBUFFER_FLAG_HAS_ATMOS = 0.34;
void main() {
    vec3 albedo = vec3(0.52, 0.52, 0.47);  // sRGB placeholder ground (P3 = the 4-detail splat)
    rt0 = vec4(albedo, GBUFFER_FLAG_HAS_ATMOS);
    rt1 = vec4(normalize(v_normal), 0.0);
}
