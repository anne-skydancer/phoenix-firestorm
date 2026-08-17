#version 450

layout(set = 2, binding = 0, std140) uniform MaterialData
{
    vec4 baseColor;
    vec4 orm;
    vec4 normal;
    vec4 emissive;
} materialData;

layout(location = 0) in vec4 vertexColor;
layout(location = 0) out vec4 outBaseColor;
layout(location = 1) out vec4 outOrm;
layout(location = 2) out vec4 outNormal;
layout(location = 3) out vec4 outEmissive;

void main()
{
    outBaseColor = materialData.baseColor * vertexColor;
    outOrm = materialData.orm;
    outNormal = materialData.normal;
    outEmissive = materialData.emissive * vertexColor;
}
