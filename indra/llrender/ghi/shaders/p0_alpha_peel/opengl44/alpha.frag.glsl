#version 440 core
layout(std140) uniform AlphaData { vec4 lightDirectionMode; vec4 ambientMinimumAlpha; vec4 directionalModel; };
layout(std140) uniform MaterialData { vec4 baseColorFactor; vec4 emissiveMetallic; vec4 roughnessNormalScale; vec4 materialParams; vec4 uvOffsetScale[4]; vec4 uvRotation[4]; };
layout(std140) uniform PeelData { uvec4 peelConfig; };
uniform sampler2D baseColorMap;
uniform sampler2D emissiveMap;
uniform sampler2D priorDepth;
uniform sampler2D opaqueDepth;
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in vec3 worldNormal;
layout(location = 0) out vec4 outColor;
vec2 transformedUv(uint index)
{
    vec2 scaled = texCoord * uvOffsetScale[index].zw;
    vec2 rotation = uvRotation[index].xy;
    return uvOffsetScale[index].xy + vec2(rotation.x * scaled.x - rotation.y * scaled.y,
                                          rotation.y * scaled.x + rotation.x * scaled.y);
}
void main()
{
    vec4 base = texture(baseColorMap, transformedUv(0u)) * baseColorFactor * vertexColor;
    if (base.a <= ambientMinimumAlpha.a) discard;
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(opaqueDepth, 0));
    float depth = gl_FragCoord.z;
    float opaque = texture(opaqueDepth, uv).r;
    float previous = texture(priorDepth, uv).r;
    bool select = peelConfig.y == 0u;
    bool replay = peelConfig.y == 1u;
    bool tail = peelConfig.y == 2u;
    bool eligible = depth >= opaque && (
        (select && (peelConfig.x == 0u || depth < previous - 0.00001)) ||
        (replay && abs(depth - previous) <= 0.00001) ||
        (tail && depth < previous - 0.00001) || peelConfig.y == 3u);
    bool legacy = directionalModel.a > 0.5;
    if (lightDirectionMode.a > 0.5)
    {
        float glow = legacy ? base.a : max(max(emissiveMetallic.r,
            emissiveMetallic.g), emissiveMetallic.b);
        outColor = eligible ? vec4(0.0, 0.0, 0.0, glow) : vec4(0.0);
        gl_FragDepth = eligible ? depth : 0.0; return;
    }
    bool fullbright = materialParams.y > 0.5;
    float diffuse = max(dot(normalize(worldNormal), normalize(-lightDirectionMode.xyz)), 0.0);
    vec3 color = fullbright ? base.rgb : base.rgb *
        (ambientMinimumAlpha.rgb + directionalModel.rgb * diffuse);
    if (!legacy) color += texture(emissiveMap, transformedUv(3u)).rgb * emissiveMetallic.rgb;
    outColor = eligible && !select ? vec4(color, base.a) : vec4(0.0);
    gl_FragDepth = eligible ? depth : 0.0;
}
