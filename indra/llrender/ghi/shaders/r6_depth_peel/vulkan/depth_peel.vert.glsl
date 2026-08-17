#version 450
#extension GL_GOOGLE_include_directive : require

#include "ghi_vulkan_clip.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 vertexColor;

void main()
{
    gl_Position = ghiToVulkanClip(vec4(inPosition, 1.0));
    vertexColor = inColor;
}
