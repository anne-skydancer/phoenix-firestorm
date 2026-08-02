#version 450
// Metered auto-exposure: average the linear-HDR scene luminance into a 1x1 target. Mirrors stock
// luminanceF/exposureF's intent (meter the HDR scene, then normalize before the tonemap clamp).
// A coarse grid sampled through a linear sampler is a good-enough average for exposure; the 1x1
// result is read by post_tonemap.frag to derive the exposure multiplier.
layout(set = 0, binding = 0) uniform texture2D scene;
layout(set = 0, binding = 1) uniform sampler samp;
layout(location = 0) out vec4 frag;

float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    float acc = 0.0;
    const int N = 24;
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            vec2 uv = (vec2(float(x), float(y)) + 0.5) / float(N);
            acc += lum(textureLod(sampler2D(scene, samp), uv, 0.0).rgb);
        }
    }
    frag = vec4(acc / float(N * N), 0.0, 0.0, 1.0);
}
