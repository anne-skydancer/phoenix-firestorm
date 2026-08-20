#version 410 core

layout(std140) uniform AlphaData
{
    vec4 lightDirectionMode;
    vec4 ambientMinimumAlpha;
    vec4 directionalModel;
    uvec4 ppllConfig;
    vec4 opaqueDepth;
};
layout(std140) uniform MaterialData
{
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessNormalScale;
    vec4 materialParams;
    vec4 uvOffsetScale[4];
    vec4 uvRotation[4];
};
uniform sampler2D baseColorMap;
uniform sampler2D emissiveMap;

layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in vec3 worldNormal;
layout(location = 0) out vec4 outColor;

vec2 transformedUv(uint index)
{
    vec2 scaled = texCoord * uvOffsetScale[index].zw;
    vec2 rotation = uvRotation[index].xy;
    return uvOffsetScale[index].xy + vec2(
        rotation.x * scaled.x - rotation.y * scaled.y,
        rotation.y * scaled.x + rotation.x * scaled.y);
}

void main()
{
    vec4 base = texture(baseColorMap, transformedUv(0u))
              * baseColorFactor * vertexColor;
    if (base.a <= ambientMinimumAlpha.a) discard;
    bool legacy = directionalModel.a > 0.5;
    bool replay = ppllConfig.x == 3u;
    if (replay)
    {
        float glow = legacy ? base.a : max(max(emissiveMetallic.r,
            emissiveMetallic.g), emissiveMetallic.b);
        outColor = vec4(0.0, 0.0, 0.0, glow);
        return;
    }
    bool fullbright = materialParams.y > 0.5;
    float diffuse = max(dot(normalize(worldNormal),
                            normalize(-lightDirectionMode.xyz)), 0.0);
    vec3 lighting = ambientMinimumAlpha.rgb + directionalModel.rgb * diffuse;
    vec3 color = fullbright ? base.rgb : base.rgb * lighting;
    if (!legacy)
        color += texture(emissiveMap, transformedUv(3u)).rgb
               * emissiveMetallic.rgb;
    outColor = vec4(color, base.a);
}