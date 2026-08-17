#version 450

layout(set = 2, binding = 0, std140) uniform MaterialData
{
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessNormalScale;
    vec4 uvOffsetScale[4];
    vec4 uvRotation[4];
} materialData;

layout(set = 2, binding = 1) uniform sampler2D baseColorMap;
layout(set = 2, binding = 2) uniform sampler2D normalMap;
layout(set = 2, binding = 3) uniform sampler2D ormMap;
layout(set = 2, binding = 4) uniform sampler2D emissiveMap;

layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in vec3 worldNormal;
layout(location = 3) in vec4 worldTangent;

layout(location = 0) out vec4 outBaseColor;
layout(location = 1) out vec4 outOrm;
layout(location = 2) out vec4 outNormal;
layout(location = 3) out vec4 outEmissive;

vec2 transformedUv(uint index)
{
    vec2 scaled = texCoord * materialData.uvOffsetScale[index].zw;
    vec2 rotation = materialData.uvRotation[index].xy;
    return materialData.uvOffsetScale[index].xy + vec2(
        rotation.x * scaled.x - rotation.y * scaled.y,
        rotation.y * scaled.x + rotation.x * scaled.y);
}

void main()
{
    vec4 base = texture(baseColorMap, transformedUv(0u))
              * materialData.baseColorFactor * vertexColor;
    vec3 tangentNormal = texture(normalMap, transformedUv(1u)).xyz * 2.0 - 1.0;
    tangentNormal.xy *= materialData.roughnessNormalScale.y;
    vec3 tangent = normalize(worldTangent.xyz);
    vec3 normal = normalize(worldNormal);
    vec3 bitangent = worldTangent.w * cross(normal, tangent);
    vec3 mappedNormal = normalize(mat3(tangent, bitangent, normal) * tangentNormal);
    vec3 orm = texture(ormMap, transformedUv(2u)).rgb;
    orm.g *= materialData.roughnessNormalScale.x;
    orm.b *= materialData.emissiveMetallic.w;
    vec3 emissive = texture(emissiveMap, transformedUv(3u)).rgb
                  * materialData.emissiveMetallic.rgb;
    outBaseColor = vec4(base.rgb, base.a);
    outOrm = vec4(orm, 1.0);
    outNormal = vec4(mappedNormal * 0.5 + 0.5, 1.0);
    outEmissive = vec4(emissive, 1.0);
}
