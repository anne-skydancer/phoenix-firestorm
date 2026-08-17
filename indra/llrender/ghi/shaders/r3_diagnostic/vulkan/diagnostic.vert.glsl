#version 450
#extension GL_GOOGLE_include_directive : require

#include "ghi_vulkan_clip.glsl"

layout(set = 0, binding = 0, std140) uniform FrameData
{
    mat4 transform;
} frameData;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 texCoord;

void main()
{
    gl_Position = ghiToVulkanClip(frameData.transform * vec4(inPosition, 1.0));
    texCoord = inTexCoord;
}
