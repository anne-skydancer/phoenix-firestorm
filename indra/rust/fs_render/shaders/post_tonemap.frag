#version 450
#extension GL_EXT_samplerless_texture_functions : require
// S1: the mandatory final tonemap+exposure+gamma pass. Faithful port of stock LL:
//   toneMap()          tonemapUtilF.glsl:121-151
//   PBRNeutral / ACES  tonemapUtilF.glsl:37-114
//   legacyGamma        postDeferredGammaCorrect.glsl:38-44
//   srgb conversions   srgbF.glsl:26
// This is the HDR->LDR step whose absence blew the scene to white: raw EEP radiance x 1.5
// scales x sky_hdr_scale routinely exceeds 1.0, and without this pass it hard-clamps.
//
// Single encode site (D4): the swapchain is sRGB, so its ROP applies linear->sRGB to whatever
// we output. We compute the FINAL sRGB display pixel `disp` (tonemap -> linear_to_srgb ->
// optional legacyGamma), then emit its pre-image `srgb_to_linear(disp)` so the ROP re-encodes
// back to exactly `disp`. legacyGamma is defined on the sRGB-encoded value, so it MUST run
// after linear_to_srgb -- this is the only faithful placement without changing the surface fmt.
layout(set = 0, binding = 0) uniform texture2D scene;
layout(set = 0, binding = 1) uniform Post {
    float exposure;      // RenderExposure, clamp[0.5,4]
    float exp_scale;     // auto-exposure meter (1.0 = fixed/faithful subset, D2)
    float tonemap_mix;   // 0 = curve bypassed (legacy classic), else RenderTonemapMix
    float gamma;         // sky gamma, drives the legacyGamma soft-clip
    int   tonemap_type;  // 0 = PBRNeutral, 1 = ACES Hill
    int   legacy_gamma;  // 1 = apply the classic-WL soft shoulder (legacy skies)
} post;
// Metered auto-exposure: 1x1 average scene luminance from the measure pass (exposure_measure.frag).
layout(set = 0, binding = 2) uniform texture2D exp_lum;
layout(location = 0) out vec4 frag;

// ---- sRGB transfer (srgbF.glsl) ----
vec3 srgb_to_linear(vec3 c) {
    bvec3 lo = lessThanEqual(c, vec3(0.04045));
    vec3 lin = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lin, vec3(lo));
}
vec3 linear_to_srgb(vec3 c) {
    c = clamp(c, vec3(0.0), vec3(1.0));
    bvec3 lo = lessThan(c, vec3(0.0031308));
    vec3 s = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    vec3 lin = c * 12.92;
    return mix(s, lin, vec3(lo));
}

// ---- PBRNeutral (Khronos) tonemapUtilF.glsl:96-114 ----
vec3 PBRNeutralToneMapping(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3(1.0), g);
}

// ---- ACES Hill (Stephen Hill fit) tonemapUtilF.glsl:37-90 ----
const mat3 ACESInputMat = mat3(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777);
const mat3 ACESOutputMat = mat3(
     1.60475, -0.10208, -0.00327,
    -0.53108,  1.10813, -0.07276,
    -0.07367, -0.00605,  1.07602);
vec3 RRTAndODTFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}
vec3 toneMapACES_Hill(vec3 color) {
    color = ACESInputMat * color;
    color = RRTAndODTFit(color);
    color = ACESOutputMat * color;
    return clamp(color, 0.0, 1.0);
}

// ---- legacyGamma soft-clip (postDeferredGammaCorrect.glsl:38-44) ----
vec3 legacyGamma(vec3 color) {
    vec3 c = 1.0 - clamp(color, vec3(0.0), vec3(1.0));
    c = 1.0 - pow(c, vec3(post.gamma));
    return c;
}

// toneMap() tonemapUtilF.glsl:121-151 -- mixes the EXPOSED-linear input with the tonemapped
// result (tonemap_mix=0 => exposure+clamp only, the legacy path).
vec3 toneMap(vec3 color) {
    // Metered auto-exposure: normalize the HDR scene toward a mid-gray key from the measured
    // average luminance, so HDR radiance (sky ~1-43) doesn't hard-clamp to white. post.exp_scale
    // is a manual multiplier (tuning / FS_ENGINE_EXPOSURE); post.exposure is the RenderExposure clamp.
    float avg_lum = max(texelFetch(exp_lum, ivec2(0, 0), 0).r, 1e-4);
    float auto_exp = clamp(0.18 / avg_lum, 0.001, 8.0);
    float final_exposure = post.exposure * post.exp_scale * auto_exp;
    vec3 exposed = color * final_exposure;
    vec3 tonemapped;
    if (post.tonemap_type == 1) tonemapped = toneMapACES_Hill(exposed);
    else tonemapped = PBRNeutralToneMapping(exposed);
    color = mix(exposed, tonemapped, post.tonemap_mix);
    return clamp(color, 0.0, 1.0);
}

void main() {
    vec3 hdr = texelFetch(scene, ivec2(gl_FragCoord.xy), 0).rgb;
    vec3 disp = linear_to_srgb(toneMap(hdr));
    if (post.legacy_gamma != 0) disp = legacyGamma(disp);
    frag = vec4(srgb_to_linear(disp), 1.0);
}
