#version 450
#extension GL_EXT_samplerless_texture_functions : require
// #4: final composite -- tonemap the linear-HDR scene to the swapchain. Replaces the 1a
// identity copy once the deferred resolve produces real HDR radiance. The sRGB swapchain
// ROP performs the final encode (single encode site; do NOT linear_to_srgb here).
// Operator = PBRNeutral (lifted from sky.frag, which stops tonemapping privately once this
// global pass exists). Exposure is fixed at 1.0 for now; auto-exposure lands in the post tier.
layout(set = 0, binding = 0) uniform texture2D scene;
layout(location = 0) out vec4 frag;

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
    vec3 c = texelFetch(scene, ivec2(gl_FragCoord.xy), 0).rgb;
    c = PBRNeutralToneMapping(c);
    frag = vec4(c, 1.0);
}
