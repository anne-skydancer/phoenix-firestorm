#version 450
// Faithful port of class1/interface/reflectionmipF.glsl -- a single bilinear tap that reduces the (gaussian
// pre-blurred) capture into each successively-smaller mip target of the scratch chain (gReflectionMipProgram).
// Stock passes a resScale uniform but the shader IGNORES it (kept out here); the 4:1 mip-0 reduction relies
// on the preceding gaussian to avoid aliasing. wgpu-shaped: sampler2D diffuseRect -> texture2D + sampler.
layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform texture2D diffuseRect;
layout(set = 0, binding = 1) uniform sampler smp;

void main() {
    vec3 col = texture(sampler2D(diffuseRect, smp), vary_texcoord0.xy).rgb;
    frag_color = vec4(col, 0.0);
}
