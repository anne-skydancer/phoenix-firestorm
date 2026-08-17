#version 450
#extension GL_GOOGLE_include_directive : require

#include "ghi_vulkan_clip.glsl"

layout(set = 0, binding = 0, std140) uniform FrameData
{
    mat4 viewProjection;
} frameData;

layout(set = 1, binding = 0, std140) uniform ObjectData
{
    mat4 modelTransform;
    mat4 normalTransform;
} objectData;

layout(set = 1, binding = 1, std140) uniform SkinData
{
    mat4 jointTransforms[4];
} skinData;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec4 inColor;
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4 inWeights;

layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec3 worldNormal;
layout(location = 3) out vec4 worldTangent;

void main()
{
    vec4 weights = inWeights / max(dot(inWeights, vec4(1.0)), 0.000001);
    mat4 skin = skinData.jointTransforms[min(inJoints.x, 3u)] * weights.x
              + skinData.jointTransforms[min(inJoints.y, 3u)] * weights.y
              + skinData.jointTransforms[min(inJoints.z, 3u)] * weights.z
              + skinData.jointTransforms[min(inJoints.w, 3u)] * weights.w;
    gl_Position = ghiToVulkanClip(frameData.viewProjection * objectData.modelTransform
                                  * skin * vec4(inPosition, 1.0));
    mat3 skinNormal = mat3(skin);
    vec3 normal = normalize(mat3(objectData.normalTransform) * skinNormal * inNormal);
    vec3 tangent = mat3(objectData.modelTransform) * skinNormal * inTangent.xyz;
    tangent = normalize(tangent - normal * dot(normal, tangent));
    texCoord = inTexCoord;
    vertexColor = inColor;
    worldNormal = normal;
    worldTangent = vec4(tangent, inTangent.w);
}
