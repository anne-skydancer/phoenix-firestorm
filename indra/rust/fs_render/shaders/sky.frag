#version 450
// S2: EEP sky dome -> LINEAR HDR into scene_hdr (the single global tonemap owns bounding).
// The haze COLOR is computed in sky.vert (skyV.glsl port). This reproduces skyF.glsl
// (haze*2, clamp[0,5]) + the resolve's SKIP_ATMOS branch (srgb_to_linear * sky_hdr_scale)
// and STOPS -- emitting linear HDR. The private PBRNeutral that used to live here is REMOVED:
// with S1's faithful global tonemap wired, keeping it here would double-tonemap the sky.
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
void main() {
    vec3 c = min(v_haze * 2.0, vec3(5.0));       // skyF.glsl: haze*2, clamp [0,5]
    c = srgb_to_linear(c);                        // softenLightF SKIP_ATMOS: linearize
    float hdr = u.a8.x > 0.0 ? u.a8.x : 1.0;
    c *= hdr;                                      // softenLightF: * sky_hdr_scale (EEP)
    frag = vec4(c, 1.0);                           // LINEAR HDR; S1 global tonemap bounds it
}
