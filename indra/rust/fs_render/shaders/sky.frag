#version 450
// EEP sky post-chain, faithful to stock (softenLightF SKIP_ATMOS branch + tonemap).
// The haze COLOR is computed in sky.vert (skyV.glsl port). skyF writes haze*2 clamp[0,5]
// to the gbuffer; the deferred resolve then does srgb_to_linear * sky_hdr_scale, then
// exposure+PBRNeutralToneMapping. We reproduce that whole chain here (single pass; sky
// legitimately opts out of atmospherics via SKIP_ATMOS). Omitting srgb_to_linear +
// sky_hdr_scale fed the tonemapper wrong-colorspace ~2x-bright values -> blown white +
// no apparent EEP response.
layout(location = 0) in vec3 v_haze;
layout(set = 0, binding = 0) uniform U {
    mat4 mvp;
    vec4 a0; vec4 a1; vec4 a2; vec4 a3; vec4 a4; vec4 a5; vec4 a6; vec4 a7;
    vec4 a8; vec4 a9; vec4 a10; vec4 a11; // a8.x = sky_hdr_scale
} u;
layout(location = 0) out vec4 frag;

vec3 srgb_to_linear(vec3 c) {
    bvec3 lo = lessThanEqual(c, vec3(0.04045));
    vec3 a = c / 12.92;
    vec3 b = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(b, a, vec3(lo));
}
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
void main() {
    vec3 c = min(v_haze * 2.0, vec3(5.0));       // skyF.glsl: haze*2, clamp [0,5]
    c = srgb_to_linear(c);                        // softenLightF SKIP_ATMOS: linearize
    float hdr = u.a8.x > 0.0 ? u.a8.x : 1.0;
    c *= hdr;                                      // softenLightF: * sky_hdr_scale (EEP)
    c = PBRNeutralToneMapping(c);                 // tonemap; sRGB ROP encodes
    frag = vec4(c, 1.0);
}
