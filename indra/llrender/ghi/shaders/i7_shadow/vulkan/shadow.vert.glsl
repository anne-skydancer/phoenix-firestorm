#version 450
#extension GL_GOOGLE_include_directive : require

#include "ghi_vulkan_clip.glsl"

layout(set = 0, binding = 0, std140) uniform ShadowFrameData
{
    mat4 viewProjection;
} shadowFrame;
layout(set = 1, binding = 0, std140) uniform ObjectData
{
    mat4 modelTransform;
    mat4 normalTransform;
} objectData;
layout(set = 1, binding = 1, std140) uniform SkinData
{
    mat3x4 jointTransforms[110];
    uvec4 skinMeta;
} skinData;

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec4 inColor;
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4 inWeights;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out float vertexAlpha;

void main()
{
    vec4 weights = max(inWeights, vec4(0.0));
    weights /= max(dot(weights, vec4(1.0)), 0.000001);
    uint lastJoint = max(skinData.skinMeta.x, 1u) - 1u;
    uvec4 joints = min(inJoints, uvec4(lastJoint));
    mat3x4 packedSkin = skinData.jointTransforms[joints.x] * weights.x
                      + skinData.jointTransforms[joints.y] * weights.y
                      + skinData.jointTransforms[joints.z] * weights.z
                      + skinData.jointTransforms[joints.w] * weights.w;
    mat3 skinLinear = mat3(packedSkin);
    vec3 skinTranslation = vec3(packedSkin[0].w, packedSkin[1].w,
                                packedSkin[2].w);
    mat4 skin = mat4(vec4(skinLinear[0], 0.0),
                     vec4(skinLinear[1], 0.0),
                     vec4(skinLinear[2], 0.0),
                     vec4(skinTranslation, 1.0));
    gl_Position = ghiToVulkanClip(shadowFrame.viewProjection *
        objectData.modelTransform * skin * vec4(inPosition, 1.0));
    texCoord = inTexCoord;
    vertexAlpha = inColor.a;
}
