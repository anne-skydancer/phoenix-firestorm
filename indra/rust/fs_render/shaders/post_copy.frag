#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Increment 1a: exact pixel copy of the linear-HDR scene buffer to the swapchain.
//
// texelFetch by gl_FragCoord makes this a GUARANTEED identity: scene_hdr and the
// swapchain target are the same size and orientation (both Vulkan top-left origin), so
// pixel (x,y) of scene_hdr maps 1:1 to pixel (x,y) of the target -- no sampler, no
// UV/flip ambiguity, no filtering. The linear value stored in the Rgba16Float scene
// buffer is written to the sRGB swapchain, whose ROP re-encodes it: byte-identical to
// today's single-pass output. The tonemap operator + exposure move in here in 1b.
layout(set = 0, binding = 0) uniform texture2D scene;
layout(location = 0) out vec4 frag;
void main() {
    frag = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
}
