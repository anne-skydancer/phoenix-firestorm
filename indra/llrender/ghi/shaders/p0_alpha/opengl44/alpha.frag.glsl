#version 440 core

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
layout(r32ui) uniform uimage2D ppllHead;
layout(std430) buffer PPLLNodes { uvec4 nodes[]; };
layout(std430) buffer PPLLCounter { uint nextNode; uint overflowCount; };
layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in vec3 worldNormal;
layout(location = 0) out vec4 outColor;
const uint INVALID_NODE = 0xffffffffu;
const uint MAX_EXACT_LAYERS = 32u;
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
    if (ppllConfig.x == 2u)
    {
        vec4 colors[MAX_EXACT_LAYERS]; float depths[MAX_EXACT_LAYERS];
        uint exactCount = 0u; vec3 weightedRgb = vec3(0.0);
        float weightedAlpha = 0.0; float revealage = 1.0;
        uint node = imageLoad(ppllHead, ivec2(gl_FragCoord.xy)).r;
        uint traversal = 0u; uint exactLimit = clamp(ppllConfig.z, 1u, MAX_EXACT_LAYERS);
        while (node != INVALID_NODE && node < ppllConfig.y && traversal < 256u)
        {
            uvec4 packed = nodes[node];
            vec4 color = vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
            float depth = uintBitsToFloat(packed.z);
            if (depth >= opaqueDepth.x)
            {
                if (exactCount < exactLimit) { colors[exactCount] = color; depths[exactCount++] = depth; }
                else { weightedRgb += color.rgb * color.a; weightedAlpha += color.a; revealage *= 1.0 - color.a; }
            }
            node = packed.w; ++traversal;
        }
        for (uint i = 1u; i < exactCount; ++i)
        {
            vec4 color = colors[i]; float depth = depths[i]; uint j = i;
            while (j > 0u && depths[j - 1u] > depth)
            { colors[j] = colors[j - 1u]; depths[j] = depths[j - 1u]; --j; }
            colors[j] = color; depths[j] = depth;
        }
        float tailAlpha = 1.0 - revealage;
        vec3 tailColor = weightedAlpha > 0.0 ? weightedRgb / weightedAlpha : vec3(0.0);
        vec4 result = vec4(tailColor * tailAlpha, tailAlpha);
        for (uint i = 0u; i < exactCount; ++i)
        { float alpha = colors[i].a; result.rgb = colors[i].rgb * alpha + result.rgb * (1.0 - alpha); result.a = alpha + result.a * (1.0 - alpha); }
        outColor = result; return;
    }
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
    if (ppllConfig.x == 1u)
    {
        uint node = atomicAdd(nextNode, 1u);
        if (node >= ppllConfig.y) { atomicAdd(overflowCount, 1u); outColor = vec4(color, base.a); return; }
        ivec2 pixel = ivec2(gl_FragCoord.xy);
        uint previous = imageAtomicExchange(ppllHead, pixel, node);
        nodes[node] = uvec4(packHalf2x16(color.rg), packHalf2x16(vec2(color.b, base.a)), floatBitsToUint(gl_FragCoord.z), previous);
        outColor = vec4(0.0); return;
    }
    outColor = vec4(color, base.a);
}