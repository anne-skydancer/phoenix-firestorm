#version 450
// Faithful port of class1/interface/gaussianF.glsl -- the separable 9-tap gaussian pre-blur stock applies
// to the supersampled probe capture (gGaussianProgram) BEFORE the 4:1 reflection-mip reduction, so the
// downsample doesn't alias. Run horizontally then vertically. wgpu-shaped: loose uniforms -> a single vec4
// (xy = direction {1,0} or {0,1}, z = resScale = 1/(probeRes*2)); sampler2D diffuseRect -> separate
// texture2D + sampler. Same weights, same +/-4 tap offsets stepping direction*resScale in UV.
layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform texture2D diffuseRect;
layout(set = 0, binding = 1) uniform sampler smp;
layout(set = 0, binding = 2) uniform GaussParams { vec4 dir_res; } gp; // xy=direction, z=resScale

void main() {
    vec3 col = vec3(0.0);
    float w[9] = float[9]( 0.0002, 0.0060, 0.0606, 0.2417, 0.3829, 0.2417, 0.0606, 0.0060, 0.0002 );
    vec2 direction = gp.dir_res.xy;
    float resScale = gp.dir_res.z;
    for (int i = 0; i < 9; ++i) {
        vec2 tc = vary_texcoord0 + float(i - 4) * direction * resScale;
        col += texture(sampler2D(diffuseRect, smp), tc).rgb * w[i];
    }
    frag_color = max(vec4(col, 0.0), vec4(0));
}
