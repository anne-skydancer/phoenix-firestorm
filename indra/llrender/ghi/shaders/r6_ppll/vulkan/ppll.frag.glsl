#version 450

layout(set = 0, binding = 0, std140) uniform AlphaData
{
    uvec4 alphaConfig;
    vec4 opaqueDepth;
} alphaData;
layout(set = 0, binding = 1, r32ui) uniform uimage2D ppllHead;
layout(set = 0, binding = 2, std430) buffer PPLLNodes
{
    uvec4 nodes[];
} nodeData;
layout(set = 0, binding = 3, std430) buffer PPLLCounter
{
    uint nextNode;
    uint overflowCount;
} counterData;

layout(location = 0) in vec4 vertexColor;
layout(location = 0) out vec4 outColor;

const uint INVALID_NODE = 0xffffffffu;
const uint MAX_EXACT_LAYERS = 32u;

void captureFragment()
{
    uint node = atomicAdd(counterData.nextNode, 1u);
    if (node >= alphaData.alphaConfig.y)
    {
        atomicAdd(counterData.overflowCount, 1u);
        outColor = vertexColor;
        return;
    }
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint previous = imageAtomicExchange(ppllHead, pixel, node);
    nodeData.nodes[node] = uvec4(packHalf2x16(vertexColor.rg),
                                packHalf2x16(vertexColor.ba),
                                floatBitsToUint(gl_FragCoord.z), previous);
    outColor = vec4(0.0);
}

void resolveFragment()
{
    vec4 colors[MAX_EXACT_LAYERS];
    float depths[MAX_EXACT_LAYERS];
    uint exactCount = 0u;
    vec3 weightedRgb = vec3(0.0);
    float weightedAlpha = 0.0;
    float revealage = 1.0;
    uint node = imageLoad(ppllHead, ivec2(gl_FragCoord.xy)).r;
    uint traversal = 0u;
    uint exactLimit = clamp(alphaData.alphaConfig.z, 1u, MAX_EXACT_LAYERS);
    while (node != INVALID_NODE && node < alphaData.alphaConfig.y && traversal < 256u)
    {
        uvec4 packed = nodeData.nodes[node];
        vec4 color = vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
        float depth = uintBitsToFloat(packed.z);
        if (depth >= alphaData.opaqueDepth.x)
        {
            if (exactCount < exactLimit)
            {
                colors[exactCount] = color;
                depths[exactCount] = depth;
                ++exactCount;
            }
            else
            {
                weightedRgb += color.rgb * color.a;
                weightedAlpha += color.a;
                revealage *= 1.0 - color.a;
            }
        }
        node = packed.w;
        ++traversal;
    }
    for (uint i = 1u; i < exactCount; ++i)
    {
        vec4 color = colors[i];
        float depth = depths[i];
        uint j = i;
        while (j > 0u && depths[j - 1u] > depth)
        {
            colors[j] = colors[j - 1u];
            depths[j] = depths[j - 1u];
            --j;
        }
        colors[j] = color;
        depths[j] = depth;
    }
    float tailAlpha = 1.0 - revealage;
    vec3 tailColor = weightedAlpha > 0.0 ? weightedRgb / weightedAlpha : vec3(0.0);
    vec4 result = vec4(tailColor * tailAlpha, tailAlpha);
    for (uint i = 0u; i < exactCount; ++i)
    {
        float alpha = colors[i].a;
        result.rgb = colors[i].rgb * alpha + result.rgb * (1.0 - alpha);
        result.a = alpha + result.a * (1.0 - alpha);
    }
    outColor = result;
}

void main()
{
    if (alphaData.alphaConfig.x == 0u) captureFragment();
    else resolveFragment();
}
