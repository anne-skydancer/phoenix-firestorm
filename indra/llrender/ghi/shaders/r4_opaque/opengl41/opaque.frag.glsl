#version 410 core

layout(std140) uniform MaterialData
{
    vec4 baseColor;
    vec4 orm;
    vec4 normal;
    vec4 emissive;
};

layout(location = 0) in vec4 vertexColor;
layout(location = 0) out vec4 outBaseColor;
layout(location = 1) out vec4 outOrm;
layout(location = 2) out vec4 outNormal;
layout(location = 3) out vec4 outEmissive;

void main()
{
    outBaseColor = baseColor * vertexColor;
    outOrm = orm;
    outNormal = normal;
    outEmissive = emissive * vertexColor;
}
