#version 440 core

layout(std140, binding = 0) uniform PeelData
{
    uvec4 peelConfig;
    vec4 opaqueDepth;
};
layout(binding = 0) uniform sampler2D priorDepth;

layout(location = 0) in vec4 vertexColor;
layout(location = 0) out vec4 outColor;

void main()
{
    float depth = gl_FragCoord.z;
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(priorDepth, 0));
    float previous = texture(priorDepth, uv).r;
    bool behindPrevious = peelConfig.x == 0u || depth < previous - 0.00001;
    bool eligible = depth >= opaqueDepth.x && behindPrevious;
    vec4 premultiplied = vec4(vertexColor.rgb * vertexColor.a, vertexColor.a);
    outColor = eligible ? premultiplied : vec4(0.0);
    gl_FragDepth = eligible ? depth : 0.0;
}
