#version 410 core
layout(std140) uniform LegacyData
{
    uvec4 legacyConfig;
    vec4 legacyParams;
};
layout(location = 0) in vec4 vertexColor;
layout(location = 0) out vec4 outColor;
void main()
{
    if (legacyConfig.x == 1u)
    {
        if (vertexColor.a < legacyParams.x) discard;
        outColor = vec4(vertexColor.rgb, 1.0);
    }
    else if (legacyConfig.x == 2u)
        outColor = vec4(vertexColor.rgb * vertexColor.a, vertexColor.a);
    else if (legacyConfig.x == 3u)
        outColor = vec4(vertexColor.rgb * legacyParams.y, 0.0);
    else
        outColor = vertexColor;
}
