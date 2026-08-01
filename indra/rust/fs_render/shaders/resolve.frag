#version 450
#extension GL_EXT_samplerless_texture_functions : require
// #4: deferred lighting resolve (minimal). Reads the G-buffer; lights HAS_ATMOS pixels
// with sun N.L + ambient; DISCARDS all other pixels so the forward background already in
// scene_hdr (sky) shows through. Writes linear HDR; the tonemap pass displays it.
//
// This is the minimal viable subset of stock's softenLightF (amblit + sunlit*N.L*albedo,
// scol=1, ambocc=1). Shadows/probes/SSAO/atmospherics layer on later.
layout(set = 0, binding = 0) uniform Env {
    vec4 sun_dir;   // xyz = eye-space direction TOWARD the sun (normalized)
    vec4 sunlight;  // rgb sunlight colour (linear)
    vec4 ambient;   // rgb ambient/sky colour (linear)
} env;
layout(set = 0, binding = 1) uniform texture2D g_albedo;
layout(set = 0, binding = 2) uniform texture2D g_normal;
layout(location = 0) out vec4 frag;

const float HAS_ATMOS = 0.34;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 a = texelFetch(g_albedo, p, 0);
    // Not lit world geometry (cleared background, sky pass-through): keep scene_hdr.
    if (abs(a.a - HAS_ATMOS) > 0.1) {
        discard;
    }
    vec3 n = normalize(texelFetch(g_normal, p, 0).xyz);
    float ndl = max(dot(n, normalize(env.sun_dir.xyz)), 0.0);
    vec3 lit = env.ambient.rgb + ndl * env.sunlight.rgb;
    frag = vec4(a.rgb * lit, 1.0);
}
