#version 450
#extension GL_GOOGLE_include_directive : require
#include "ghi_vulkan_clip.glsl"

layout(set = 0, binding = 0, std140) uniform WaterData
{
    mat4 mvp; mat4 modelView; vec4 cameraRoute; vec4 waveDirections;
    vec4 waterHeightPhaseBlendRefScale; vec4 fresnelBlurFogDensityExposure;
    vec4 normalScaleSunUp; vec4 lightDirection; vec4 lightColor;
    vec4 fogColorTonemap;
} W;
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 0) out vec2 screenUv;
layout(location = 1) out vec3 viewPosition;
layout(location = 2) out vec3 viewNormal;
layout(location = 3) out vec4 waveUv;
layout(location = 4) out vec2 bigWaveUv;

void main()
{
    if (W.cameraRoute.w < 0.5)
    {
        vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
        screenUv = position;
        gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
        viewPosition = vec3(0.0); viewNormal = vec3(0.0, 0.0, 1.0);
        waveUv = vec4(0.0); bigWaveUv = vec2(0.0); return;
    }
    vec4 worldPosition = vec4(inPosition, 1.0);
    vec4 clip = W.mvp * worldPosition;
    gl_Position = ghiVulkanClip(clip);
    screenUv = clip.xy / clip.w * 0.5 + 0.5;
    viewPosition = (W.modelView * worldPosition).xyz;
    viewNormal = normalize(mat3(W.modelView) * inNormal);
    vec2 position = inPosition.xy;
    position.x += (cos(position.x * 0.08) + sin(position.y * 0.02)) * 6.0;
    float phase = W.waterHeightPhaseBlendRefScale.y;
    bigWaveUv = position * 0.04 + W.waveDirections.xy * phase * 0.055;
    waveUv.xy = position * vec2(0.45, 0.9) + W.waveDirections.zw * phase * 0.13;
    waveUv.zw = position * vec2(0.1, 0.2) + W.waveDirections.xy * phase * 0.1;
}
