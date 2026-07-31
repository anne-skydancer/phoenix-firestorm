#version 450
// skyF.glsl writes LINEAR haze*2 (clamped [0,5]) into an HDR gbuffer; the viewer then
// tonemaps+exposes it in postDeferredTonemap. We skipped that stage -> wrong color.
// This is the ACTUAL operator (PBRNeutralToneMapping, tonemapUtilF.glsl:96-114),
// faithful-camera path (exposure 1.0). The sRGB swapchain ROP does linear->sRGB.
layout(location = 0) in vec3 v_haze;
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
    vec3 c = min(v_haze * 2.0, vec3(5.0));
    c = PBRNeutralToneMapping(c);
    frag = vec4(c, 1.0);
}
