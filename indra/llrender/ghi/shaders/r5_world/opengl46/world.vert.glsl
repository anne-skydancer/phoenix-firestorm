#version 460 core

layout(std140, binding = 0) uniform WorldData
{
    mat4 viewProjection;
    vec4 terrainTransform[4];
    vec4 terrainParams;
    vec4 sunDirectionIntensity;
    vec4 sunColor;
    vec4 moonDirectionIntensity;
    vec4 moonColor;
    vec4 localPositionRadius;
    vec4 localColorFalloff;
    vec4 projectorDirectionCutoff;
    vec4 projectorColorShadowBias;
    vec4 environmentParams;
    vec4 skyZenith;
    vec4 skyHorizon;
    vec4 waterColorTime;
    vec4 waveDirections;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec3 worldPosition;
layout(location = 2) out vec3 worldNormal;

void main()
{
    gl_Position = viewProjection * vec4(inPosition, 1.0);
    texCoord = inTexCoord;
    worldPosition = inPosition;
    worldNormal = inNormal;
}
