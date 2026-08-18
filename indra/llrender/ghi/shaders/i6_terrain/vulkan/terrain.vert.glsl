#version 450
#extension GL_GOOGLE_include_directive : require
#include "ghi_vulkan_clip.glsl"

layout(set = 0, binding = 0, std140) uniform TerrainData
{
    mat4 viewProjection;
    mat4 modelTransform;
    mat4 normalTransform;
    vec4 uvOffsetScale[4];
    vec4 uvRotation[4];
    vec4 baseColorFactors[4];
    vec4 emissiveMetallic[4];
    vec4 roughnessAlpha[4];
    vec4 terrainParams;
} terrainData;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 3) in vec2 inCompositionCoord;
layout(location = 0) out vec3 localPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec2 compositionCoord;

void main()
{
    gl_Position = ghiToVulkanClip(terrainData.viewProjection *
        terrainData.modelTransform * vec4(inPosition, 1.0));
    localPosition = inPosition;
    worldNormal = normalize(mat3(terrainData.normalTransform) * inNormal);
    compositionCoord = inCompositionCoord;
}
