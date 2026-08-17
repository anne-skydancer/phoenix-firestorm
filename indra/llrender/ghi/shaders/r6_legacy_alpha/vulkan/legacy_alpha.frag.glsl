#version 450
#extension GL_EXT_terminate_invocation : require
layout(set = 0, binding = 0, std140) uniform LegacyData
{
    uvec4 legacyConfig;
    vec4 legacyParams;
} legacyData;
layout(location = 0) in vec4 vertexColor;
layout(location = 0) out vec4 outColor;
void main()
{
    if (legacyData.legacyConfig.x == 1u)
    {
        if (vertexColor.a < legacyData.legacyParams.x) terminateInvocation;
        outColor = vec4(vertexColor.rgb, 1.0);
    }
    else if (legacyData.legacyConfig.x == 2u)
        outColor = vec4(vertexColor.rgb * vertexColor.a, vertexColor.a);
    else if (legacyData.legacyConfig.x == 3u)
        outColor = vec4(vertexColor.rgb * legacyData.legacyParams.y, 0.0);
    else
        outColor = vertexColor;
}
