#version 450
// Fullscreen triangle (no vertex buffer bound) emitting a 0..1 texcoord, for the 2D probe-convolution
// resamples (gaussian pre-blur + reflection-mip downsample). Mirrors post.vert's index trick but adds a
// varying UV since those passes need FILTERED sampling at offset/rescaled coordinates (not texelFetch).
// Vulkan top-left framebuffer origin; the same convention writes and reads the capture, so the resample
// preserves orientation (the final copy into the cube face matches S2's direct copy exactly).
layout(location = 0) out vec2 vary_texcoord0;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vary_texcoord0 = p;                       // {(0,0),(2,0),(0,2)} -> visible region interpolates 0..1
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
