#version 450

layout(set = 2, binding = 0, std140) uniform ShadowMaterialData
{
    vec4 alphaData;
    vec4 uvOffsetScale;
    vec4 uvRotationFlags;
} shadowMaterial;
layout(set = 2, binding = 1) uniform sampler2D baseColorMap;
layout(location = 0) in vec2 texCoord;
layout(location = 1) in float vertexAlpha;

void main()
{
    float outputDepth = gl_FragCoord.z;
    if (shadowMaterial.alphaData.z > 0.5)
    {
        vec2 scaled = texCoord * shadowMaterial.uvOffsetScale.zw;
        vec2 rotation = shadowMaterial.uvRotationFlags.xy;
        vec2 uv = shadowMaterial.uvOffsetScale.xy + vec2(
            rotation.x * scaled.x - rotation.y * scaled.y,
            rotation.y * scaled.x + rotation.x * scaled.y);
        float alpha = texture(baseColorMap, uv).a *
                      shadowMaterial.alphaData.y * vertexAlpha;
        if (alpha < shadowMaterial.alphaData.x) outputDepth = 1.0;
    }
    // Strict Less against a clear value of 1.0 rejects masked fragments
    // without requiring the optional demote-to-helper Vulkan feature.
    gl_FragDepth = outputDepth;
}
