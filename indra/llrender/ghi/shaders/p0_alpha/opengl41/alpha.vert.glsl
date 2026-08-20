#version 410 core

layout(std140) uniform FrameData { mat4 viewProjection; };
layout(std140) uniform AlphaData
{
    vec4 lightDirectionMode; vec4 ambientMinimumAlpha;
    vec4 directionalModel; uvec4 ppllConfig; vec4 opaqueDepth;
};
layout(std140) uniform ObjectData { mat4 modelTransform; mat4 normalTransform; };
layout(std140) uniform SkinData { mat3x4 jointTransforms[110]; uvec4 skinMeta; };

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec4 inColor;
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4 inWeights;

layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec3 worldNormal;

void main()
{
    if (ppllConfig.x == 2u)
    {
        gl_Position = vec4(inPosition, 1.0);
        texCoord = inTexCoord; vertexColor = inColor; worldNormal = inNormal;
        return;
    }
    vec4 weights = max(inWeights, vec4(0.0));
    weights /= max(dot(weights, vec4(1.0)), 0.000001);
    uint lastJoint = max(skinMeta.x, 1u) - 1u;
    uvec4 joints = min(inJoints, uvec4(lastJoint));
    mat3x4 packedSkin = jointTransforms[joints.x] * weights.x
                      + jointTransforms[joints.y] * weights.y
                      + jointTransforms[joints.z] * weights.z
                      + jointTransforms[joints.w] * weights.w;
    mat3 skinLinear = mat3(packedSkin);
    vec3 skinTranslation = vec3(packedSkin[0].w, packedSkin[1].w,
                                packedSkin[2].w);
    mat4 skin = mat4(vec4(skinLinear[0], 0.0), vec4(skinLinear[1], 0.0),
                     vec4(skinLinear[2], 0.0), vec4(skinTranslation, 1.0));
    gl_Position = viewProjection * modelTransform * skin * vec4(inPosition, 1.0);
    texCoord = inTexCoord;
    vertexColor = inColor;
    worldNormal = normalize(mat3(normalTransform) * skinLinear * inNormal);
}