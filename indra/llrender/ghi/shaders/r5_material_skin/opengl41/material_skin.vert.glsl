#version 410 core

layout(std140) uniform FrameData
{
    mat4 viewProjection;
};

layout(std140) uniform SkinData
{
    mat4 jointTransforms[4];
};

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
    mat4 skin = jointTransforms[min(inJoints.x, 3u)] * weights.x
              + jointTransforms[min(inJoints.y, 3u)] * weights.y
              + jointTransforms[min(inJoints.z, 3u)] * weights.z
              + jointTransforms[min(inJoints.w, 3u)] * weights.w;
    gl_Position = viewProjection * skin * vec4(inPosition, 1.0);
    mat3 skinNormal = mat3(skin);
    texCoord = inTexCoord;
    vertexColor = inColor;
    worldNormal = normalize(skinNormal * inNormal);
    worldTangent = vec4(normalize(skinNormal * inTangent.xyz), inTangent.w);
}
