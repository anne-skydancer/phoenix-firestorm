#version 440 core

layout(std140) uniform ShadowMaterialData
{
    vec4 alphaData;
    vec4 uvOffsetScale;
    vec4 uvRotationFlags;
};
uniform sampler2D baseColorMap;
layout(location = 0) in vec2 texCoord;
layout(location = 1) in float vertexAlpha;

void main()
{
    if (alphaData.z > 0.5)
    {
        vec2 scaled = texCoord * uvOffsetScale.zw;
        vec2 uv = uvOffsetScale.xy + vec2(
            uvRotationFlags.x * scaled.x - uvRotationFlags.y * scaled.y,
            uvRotationFlags.y * scaled.x + uvRotationFlags.x * scaled.y);
        if (texture(baseColorMap, uv).a * alphaData.y * vertexAlpha <
            alphaData.x) discard;
    }
}
