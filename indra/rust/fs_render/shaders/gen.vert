#version 450
// Shared vertex stage for the reflection-probe convolution generators (radiance prefilter + irradiance
// convolution). Faithful port of radianceGenV.glsl / irradianceGenV.glsl: a clip-space quad (position.z
// = -1, drawn as a fullscreen tri-strip) whose per-vertex direction is the modelview-rotated position ->
// interpolated to a per-pixel cube-sampling ray. The modelview is a per-cube-face rotation (the C++
// driver's lookAt(sClipToCubeLookVecs/UpVecs) -> getOpenGLRotation).
layout(location = 0) in vec3 position;
layout(location = 0) out vec3 vary_dir;

layout(set = 0, binding = 2) uniform GenParams {
    mat4 modelview;        // per-face rotation
    float mipLevel;        // radiance: output mip index; irradiance: unused
    float max_probe_lod;   // log2(res)-1
    float probe_strength;  // radiance alpha scale
    int sourceIdx;         // source cube-array layer
    int u_width;           // radiance: probe resolution; irradiance: hardcoded 64 in-shader
} gp;

void main() {
    gl_Position = vec4(position, 1.0);
    vary_dir = (gp.modelview * vec4(position, 1.0)).xyz;
}
