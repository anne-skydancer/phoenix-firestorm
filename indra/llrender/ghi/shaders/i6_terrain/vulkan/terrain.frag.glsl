#version 450

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
layout(set = 0, binding = 1) uniform sampler2D compositionMap;
layout(set = 0, binding = 2) uniform sampler2D layer0Map;
layout(set = 0, binding = 3) uniform sampler2D layer1Map;
layout(set = 0, binding = 4) uniform sampler2D layer2Map;
layout(set = 0, binding = 5) uniform sampler2D layer3Map;
layout(location = 0) in vec3 localPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 compositionCoord;
layout(location = 0) out vec4 outBaseColor;
layout(location = 1) out vec4 outOrm;
layout(location = 2) out vec4 outNormal;
layout(location = 3) out vec4 outEmissive;

vec2 transformUv(uint layer, vec2 uv)
{
    vec2 scaled = uv * terrainData.uvOffsetScale[layer].zw;
    vec2 rotation = terrainData.uvRotation[layer].xy;
    return terrainData.uvOffsetScale[layer].xy + vec2(
        rotation.x * scaled.x - rotation.y * scaled.y,
        rotation.y * scaled.x + rotation.x * scaled.y);
}
vec4 sampleLayer(uint layer, sampler2D source)
{
    if (terrainData.terrainParams.w < 2.0)
        return texture(source, transformUv(layer, localPosition.xy));
    vec3 weight = pow(abs(normalize(worldNormal)), vec3(4.0));
    weight /= max(weight.x + weight.y + weight.z, 0.000001);
    vec4 xy = texture(source, transformUv(layer, localPosition.xy));
    vec4 yz = texture(source, transformUv(layer, localPosition.yz));
    vec4 xz = texture(source, transformUv(layer, vec2(-localPosition.x, localPosition.z)));
    return xy * weight.z + yz * weight.x + xz * weight.y;
}
vec4 compositionWeights()
{
    if (terrainData.terrainParams.z > 0.5)
    {
        vec3 other = max(texture(compositionMap,
            localPosition.xy / terrainData.terrainParams.x).rgb, vec3(0.0));
        other /= max(1.0, other.x + other.y + other.z);
        return vec4(1.0 - other.x - other.y - other.z, other);
    }
    float a1 = texture(compositionMap, compositionCoord).a;
    float a2 = texture(compositionMap, compositionCoord - vec2(2.0, 0.0)).a;
    float af = texture(compositionMap, compositionCoord - vec2(1.0, 0.0)).a;
    return mix(mix(vec4(0,0,0,1), vec4(0,0,1,0), a2),
               mix(vec4(0,1,0,0), vec4(1,0,0,0), a1), af);
}
void main()
{
    vec4 weight = max(compositionWeights(), vec4(0.0));
    weight /= max(dot(weight, vec4(1.0)), 0.000001);
    vec4 samples[4] = vec4[4](sampleLayer(0u, layer0Map),
        sampleLayer(1u, layer1Map), sampleLayer(2u, layer2Map),
        sampleLayer(3u, layer3Map));
    vec4 base = vec4(0.0);
    vec3 emissive = vec3(0.0);
    float metallic = 0.0;
    float roughness = 0.0;
    for (uint layer = 0u; layer < 4u; ++layer)
    {
        base += samples[layer] * terrainData.baseColorFactors[layer] * weight[layer];
        emissive += terrainData.emissiveMetallic[layer].rgb * weight[layer];
        metallic += terrainData.emissiveMetallic[layer].a * weight[layer];
        roughness += terrainData.roughnessAlpha[layer].x * weight[layer];
    }
    outBaseColor = base;
    outOrm = vec4(1.0, roughness, metallic, 1.0);
    outNormal = vec4(normalize(worldNormal) * 0.5 + 0.5, 1.0);
    outEmissive = vec4(emissive, 1.0);
}
