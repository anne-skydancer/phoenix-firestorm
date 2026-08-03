#version 450
// Metered auto-exposure: arithmetic-mean luminance into a 1x1 target, FAITHFUL to stock's luminanceF
// sampling -- the center 60% of screen, nudged down 0.1 to favor ground over sky (so the metered value
// tracks what you're standing in, not the bright sky/sun). The 1x1 result feeds post_tonemap.frag's
// bounded exposure (stock exposureF: a bright scene pins at exp_max, so the sun no longer causes the
// swing that the old unbounded 0.18/avg turned into flicker) and is temporally smoothed in live.rs.
layout(set = 0, binding = 0) uniform texture2D scene;
layout(set = 0, binding = 1) uniform sampler samp;
layout(location = 0) out vec4 frag;

float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    float acc = 0.0;
    const int N = 24;
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            // stock luminanceF: sample the center 60%, nudged down 0.1 to favor ground over sky
            vec2 uv = (vec2(float(x), float(y)) + 0.5) / float(N);
            uv = uv * 0.6 + 0.2;
            uv.y -= 0.1;
            acc += lum(textureLod(sampler2D(scene, samp), uv, 0.0).rgb);
        }
    }
    frag = vec4(acc / float(N * N), 0.0, 0.0, 1.0);
}
