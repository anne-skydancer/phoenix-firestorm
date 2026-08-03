#version 450
// Ground P3: the legacy 4-detail terrain SPLAT -- verbatim from stock terrainF.glsl:44-69. Writes the
// G-buffer: RT0 = blended detail albedo (sRGB-encoded; the resolve srgb_to_linear's it, exactly as
// softenLightF does) + FLAG.a (HAS_ATMOS 0.34); RT1 = world normal + env(0). Detail textures are
// sampled at one world-tiled UV (WRAP); the alpha ramp (CLAMP) cross-fades the 4 details by the
// per-vertex composition (shifted 0/-2/-1) + noise.
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_detail_uv;
layout(location = 2) in vec2 v_comp;   // .x = composition [0,3], .y = noise [0,1]
layout(set = 0, binding = 1) uniform texture2D detail_0;
layout(set = 0, binding = 2) uniform texture2D detail_1;
layout(set = 0, binding = 3) uniform texture2D detail_2;
layout(set = 0, binding = 4) uniform texture2D detail_3;
layout(set = 0, binding = 5) uniform texture2D alpha_ramp;
layout(set = 0, binding = 6) uniform sampler samp_wrap;   // detail (repeat)
layout(set = 0, binding = 7) uniform sampler samp_clamp;  // ramp (clamp)
layout(location = 0) out vec4 rt0;  // albedo.rgb (sRGB) + flag.a
layout(location = 1) out vec4 rt1;  // world normal.xyz + env.w
const float GBUFFER_FLAG_HAS_ATMOS = 0.34;
void main() {
    vec2 uv = v_detail_uv;
    vec3 c0 = texture(sampler2D(detail_0, samp_wrap), uv).rgb;
    vec3 c1 = texture(sampler2D(detail_1, samp_wrap), uv).rgb;
    vec3 c2 = texture(sampler2D(detail_2, samp_wrap), uv).rgb;
    vec3 c3 = texture(sampler2D(detail_3, samp_wrap), uv).rgb;
    float comp = v_comp.x;
    float noise = v_comp.y;
    // stock: alpha1 = ramp(comp), alpha2 = ramp(comp-2), alphaFinal = ramp(comp-1); .a channel.
    float a1 = texture(sampler2D(alpha_ramp, samp_clamp), vec2(comp,       noise)).a;
    float a2 = texture(sampler2D(alpha_ramp, samp_clamp), vec2(comp - 2.0, noise)).a;
    float aF = texture(sampler2D(alpha_ramp, samp_clamp), vec2(comp - 1.0, noise)).a;
    vec3 albedo = mix(mix(c3, c2, a2), mix(c1, c0, a1), aF);
    rt0 = vec4(max(albedo, vec3(0.0)), GBUFFER_FLAG_HAS_ATMOS);
    rt1 = vec4(normalize(v_normal), 0.0);
}
