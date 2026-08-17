#version 440 core
layout(std140, binding = 0) uniform InteractionData
{
    uvec4 interactionConfig;
    vec4 interactionTint;
};
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectId;
layout(location = 2) out float outPickDepth;
void main()
{
    uint x = uint(floor(gl_FragCoord.x));
    uint y = uint(floor(abs(gl_FragCoord.y - 32.0)));
    uint phase = interactionConfig.x;
    if (phase == 0u)
    {
        float stripe = ((x / 8u) & 1u) == 0u ? 0.12 : 0.20;
        float row = ((y / 8u) & 1u) == 0u ? 0.02 : 0.06;
        outColor = vec4(stripe + row, stripe + 0.5 * row, stripe + 0.10, 1.0);
        outObjectId = 0u;
        outPickDepth = 1.0;
    }
    else if (phase == 1u)
    {
        uint cell = (x / 8u) & 3u;
        float glyph = ((x + y) % 5u) == 0u ? 1.0 : 0.45;
        outColor = vec4(interactionTint.rgb * glyph, interactionTint.a);
        outObjectId = interactionConfig.y + cell;
        outPickDepth = 0.42 + float(cell) * 0.01;
    }
    else
    {
        uint slot = (x / 4u) & 7u;
        float pulse = (slot & 1u) == 0u ? 1.0 : 0.65;
        outColor = vec4(interactionTint.rgb * pulse, interactionTint.a);
        outObjectId = interactionConfig.y + slot;
        outPickDepth = 0.80;
    }
}
