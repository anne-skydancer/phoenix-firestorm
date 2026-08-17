#version 410 core

layout(std140) uniform MaterialData
{
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessNormalScale;
};

uniform sampler2D baseColorMap;
uniform sampler2D normalMap;
uniform sampler2D ormMap;
uniform sampler2D emissiveMap;

layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in vec3 worldNormal;
layout(location = 3) in vec4 worldTangent;

layout(location = 0) out vec4 outBaseColor;
layout(location = 1) out vec4 outOrm;
layout(location = 2) out vec4 outNormal;
layout(location = 3) out vec4 outEmissive;

void main()
{
    vec4 base = texture(baseColorMap, texCoord) * baseColorFactor * vertexColor;
    vec3 tangentNormal = texture(normalMap, texCoord).xyz * 2.0 - 1.0;
    tangentNormal.xy *= roughnessNormalScale.y;
    vec3 tangent = normalize(worldTangent.xyz);
    vec3 normal = normalize(worldNormal);
    vec3 bitangent = worldTangent.w * cross(normal, tangent);
    vec3 mappedNormal = normalize(mat3(tangent, bitangent, normal) * tangentNormal);
    vec3 orm = texture(ormMap, texCoord).rgb;
    orm.g *= roughnessNormalScale.x;
    orm.b *= emissiveMetallic.w;
    vec3 emissive = texture(emissiveMap, texCoord).rgb * emissiveMetallic.rgb;
    outBaseColor = vec4(base.rgb, base.a);
    outOrm = vec4(orm, 1.0);
    outNormal = vec4(mappedNormal * 0.5 + 0.5, 1.0);
    outEmissive = vec4(emissive, 1.0);
}
