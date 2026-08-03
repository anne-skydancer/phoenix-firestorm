#version 450
// PBR-2: the PBR terrain SPLAT, BASE-COLOR first. Idiomatic port of stock pbrterrainF.glsl's
// HEIGHTMAP_WITH_NOISE (paint_type 0) path, base-color channel only. Structurally identical to the
// shipped legacy splat (terrain_gb.frag) -- SAME alpha-ramp weight blend (the PBR "tm" convex weights
// ARE the legacy nested-mix weights; the 0.1 usage-threshold/renormalize is a sampling optimization,
// deferred with normal/MR/emissive to PBR-5) -- but the sources are GLTF base-color materials, not
// legacy detail textures. Differences from legacy: (a) base color is sRGB source -> srgb_to_linear;
// (b) per-material GLTF baseColor factor modulates; (c) writes LINEAR base + FLAG.a = HAS_PBR (0.67),
// so the resolve runs the metallic-roughness BRDF instead of the legacy softenLight path.
//
// KHR per-material texture transforms + the region-SW-origin UV are folded into the fed detail_scale
// for now (shared planar UV, like legacy); independent per-material transforms arrive with the feed.
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_detail_uv;
layout(location = 2) in vec2 v_comp;   // .x = composition [0,3], .y = noise [0,1]
layout(set = 0, binding = 1) uniform texture2D base_0;   // GLTF base-color, material 0
layout(set = 0, binding = 2) uniform texture2D base_1;
layout(set = 0, binding = 3) uniform texture2D base_2;
layout(set = 0, binding = 4) uniform texture2D base_3;
layout(set = 0, binding = 5) uniform texture2D alpha_ramp;
layout(set = 0, binding = 6) uniform sampler samp_wrap;   // base color (repeat)
layout(set = 0, binding = 7) uniform sampler samp_clamp;  // ramp (clamp)
layout(set = 0, binding = 8) uniform Factors {
    vec4 base_factor[4];   // per-material GLTF baseColor factor (rgb used; a = base alpha / cutoff)
} f;
layout(location = 0) out vec4 rt0;  // base color.rgb (LINEAR) + flag.a
layout(location = 1) out vec4 rt1;  // world normal.xyz + env.w
const float GBUFFER_FLAG_HAS_PBR = 0.67;

// stock srgbF.glsl srgb_to_linear
vec3 srgb_to_linear(vec3 cs) {
    vec3 lo = cs / 12.92;
    vec3 hi = pow((cs + 0.055) / 1.055, vec3(2.4));
    return mix(lo, hi, vec3(greaterThan(cs, vec3(0.04045))));
}

void main() {
    vec2 uv = v_detail_uv;
    // Sample -> linearize (sRGB source) -> GLTF baseColor factor, per material.
    vec3 c0 = srgb_to_linear(texture(sampler2D(base_0, samp_wrap), uv).rgb) * f.base_factor[0].rgb;
    vec3 c1 = srgb_to_linear(texture(sampler2D(base_1, samp_wrap), uv).rgb) * f.base_factor[1].rgb;
    vec3 c2 = srgb_to_linear(texture(sampler2D(base_2, samp_wrap), uv).rgb) * f.base_factor[2].rgb;
    vec3 c3 = srgb_to_linear(texture(sampler2D(base_3, samp_wrap), uv).rgb) * f.base_factor[3].rgb;
    float comp = v_comp.x;
    float noise = v_comp.y;
    // Same alpha-ramp weights as the shipped legacy splat: alpha1=ramp(comp), alpha2=ramp(comp-2),
    // alphaFinal=ramp(comp-1); nested mix = convex weighted sum of the 4 materials.
    float a1 = texture(sampler2D(alpha_ramp, samp_clamp), vec2(comp,       noise)).a;
    float a2 = texture(sampler2D(alpha_ramp, samp_clamp), vec2(comp - 2.0, noise)).a;
    float aF = texture(sampler2D(alpha_ramp, samp_clamp), vec2(comp - 1.0, noise)).a;
    vec3 base = mix(mix(c3, c2, a2), mix(c1, c0, a1), aF);   // LINEAR
    rt0 = vec4(max(base, vec3(0.0)), GBUFFER_FLAG_HAS_PBR);
    rt1 = vec4(normalize(v_normal), 0.0);
}
