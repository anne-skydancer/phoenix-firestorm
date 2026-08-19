#version 450

layout(set = 0, binding = 0, std140) uniform WaterData
{
    mat4 mvp; mat4 modelView; vec4 cameraRoute; vec4 waveDirections;
    vec4 waterHeightPhaseBlendRefScale; vec4 fresnelBlurFogDensityExposure;
    vec4 normalScaleSunUp; vec4 lightDirection; vec4 lightColor;
    vec4 fogColorTonemap;
} W;
layout(set = 0, binding = 1) uniform sampler2D normalTexture;
layout(set = 0, binding = 2) uniform sampler2D nextNormalTexture;
layout(set = 0, binding = 3) uniform sampler2D refractionTexture;
layout(set = 0, binding = 4) uniform sampler2D reflectionTexture;
layout(set = 0, binding = 5) uniform sampler2D exclusionTexture;
layout(set = 0, binding = 6) uniform sampler2D depthTexture;
layout(location = 0) in vec2 screenUv;
layout(location = 1) in vec3 viewPosition;
layout(location = 2) in vec3 viewNormal;
layout(location = 3) in vec4 waveUv;
layout(location = 4) in vec2 bigWaveUv;
layout(location = 0) out vec4 outColor;

vec3 waveNormal(sampler2D source)
{
    vec3 a = texture(source, bigWaveUv).xyz * 2.0 - 1.0;
    vec3 b = texture(source, waveUv.xy).xyz * 2.0 - 1.0;
    vec3 c = texture(source, waveUv.zw).xyz * 2.0 - 1.0;
    return normalize(a + b * 0.4 + c * 0.6);
}
void main()
{
    if (W.cameraRoute.w < 0.5) { outColor = texture(refractionTexture, screenUv); return; }
    vec2 uv = clamp(screenUv, vec2(0.0), vec2(0.999));
    float exclusion = texture(exclusionTexture, uv).r;
    if (exclusion < 0.001) discard;
    vec3 normal = normalize(mix(waveNormal(normalTexture), waveNormal(nextNormalTexture),
                                W.waterHeightPhaseBlendRefScale.z));
    normal.xy *= W.normalScaleSunUp.xy; normal = normalize(normal);
    float distanceToEye = max(length(viewPosition), 1.0);
    vec2 distorted = clamp(uv + normal.xy * W.waterHeightPhaseBlendRefScale.w /
                           sqrt(distanceToEye), vec2(0.0), vec2(0.999));
    float sceneDepth = texture(depthTexture, distorted).r;
    if (gl_FragCoord.z < sceneDepth) discard;
    float shoreline = smoothstep(0.0, 0.0025, abs(gl_FragCoord.z - sceneDepth));
    distorted = mix(uv, distorted, shoreline * exclusion);
    vec3 refraction = texture(refractionTexture, distorted).rgb;
    vec3 reflection = texture(reflectionTexture,
        clamp(uv + normal.xy * 0.02, vec2(0.0), vec2(0.999))).rgb;
    vec3 viewDirection = normalize(-viewPosition);
    float facing = clamp(abs(dot(normalize(viewNormal + normal), viewDirection)), 0.0, 1.0);
    float fresnel = clamp(W.fresnelBlurFogDensityExposure.x +
        (1.0 - facing) * W.waterHeightPhaseBlendRefScale.w, 0.0, 1.0);
    vec3 halfVector = normalize(normalize(W.lightDirection.xyz) + viewDirection);
    float gloss = max(2.0, 256.0 *
        (1.0 - clamp(W.fresnelBlurFogDensityExposure.y, 0.0, 1.0)));
    float specular = pow(max(dot(normal, halfVector), 0.0), gloss);
    vec3 color = mix(refraction, reflection, fresnel) +
                 W.lightColor.rgb * specular * exclusion;
    if (W.cameraRoute.w > 1.5)
    {
        float fog = 1.0 - exp(-max(W.fresnelBlurFogDensityExposure.z, 0.0) * distanceToEye);
        color = mix(color, W.fogColorTonemap.rgb, clamp(fog, 0.0, 1.0));
    }
    outColor = vec4(max(color, vec3(0.0)), 1.0);
}
