#version 450
// Phase A.3: native-VK UI. One 2D pass over the tonemapped swapchain, fed honestly from
// LLRender::flush -- the viewer hands us its OWN modelview*projection (mMatrix, software) and
// the interleaved immediate verts. No glGet, no tap. mvp is a per-draw dynamic-offset UBO.
layout(set = 0, binding = 0) uniform Mvp { mat4 mvp; } u;

layout(location = 0) in vec3 in_pos;   // LLRender vertex.xyz (UI is 2D; z carried for 3D overlays)
layout(location = 1) in vec2 in_uv;    // TEXCOORD0
layout(location = 2) in vec4 in_color; // LLColor4U, normalized (Unorm8x4)

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    gl_Position = u.mvp * vec4(in_pos, 1.0);
    // The viewer's UI ortho uses GL's z-convention (NDC z in [-1,1]); wgpu/Vulkan clip space is
    // [0,1]. The UI is painter's-ordered (no depth test), so pin z to mid-range -> never z-clips,
    // regardless of the incoming ortho. x/y (the only thing that matters for 2D) are untouched.
    gl_Position.z = 0.5 * gl_Position.w;
    v_uv = in_uv;
    v_color = in_color;
}
