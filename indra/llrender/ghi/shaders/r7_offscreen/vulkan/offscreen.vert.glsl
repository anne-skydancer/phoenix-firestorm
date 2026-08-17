#version 450
#extension GL_GOOGLE_include_directive : require
#include "ghi_vulkan_clip.glsl"
void main()
{
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = ghiToVulkanClip(vec4(corner * 2.0 - 1.0, 0.0, 1.0));
}
