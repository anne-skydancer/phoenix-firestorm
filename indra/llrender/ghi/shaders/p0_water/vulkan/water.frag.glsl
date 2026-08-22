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
vec3 srgbToLinear(vec3 color)
{
    return mix(color / 12.92, pow((color + 0.055) / 1.055, vec3(2.4)),
               greaterThan(color, vec3(0.04045)));
}
float packedDepth(vec2 uv)
{
    uvec4 bytes = uvec4(round(texture(depthTexture, uv) * 255.0));
    return uintBitsToFloat(bytes.r | (bytes.g << 8) |
                           (bytes.b << 16) | (bytes.a << 24));
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
    vec2 depthUv = W.cameraRoute.w > 1.5 ? uv : distorted;
    float sceneDepth = packedDepth(depthUv);
    if (gl_FragCoord.z > sceneDepth) discard;
    float shoreline = smoothstep(0.0, 0.0025, abs(gl_FragCoord.z - sceneDepth));
    distorted = mix(uv, distorted, shoreline * exclusion);
    vec3 refraction = texture(refractionTexture, distorted).rgb;
    if (W.cameraRoute.w > 1.5)
    {
        vec3 wave1 = texture(normalTexture, bigWaveUv).xyz * 2.0 - 1.0;
        vec3 wave2 = texture(normalTexture, waveUv.xy).xyz * 2.0 - 1.0;
        vec3 wave3 = texture(normalTexture, waveUv.zw).xyz * 2.0 - 1.0;
        vec3 underwaterNormal = normalize(wave1 + wave2 + wave3);
        vec2 underwaterUv = uv + underwaterNormal.xy *
            W.waterHeightPhaseBlendRefScale.w * exclusion;
        vec3 refraction = texture(refractionTexture, underwaterUv).rgb;
        vec3 planeNormal = normalize(viewNormal);
        float planeOffset = -dot(viewPosition, planeNormal);
        vec3 viewDirection = normalize(viewPosition);
        float eyeSlope = -dot(viewDirection, planeNormal);
        float eyeDepth = max(-planeOffset, 0.0);
        vec3 intersection = planeOffset > 0.0
            ? viewDirection * planeOffset / eyeSlope : vec3(0.0);
        float waterDepth = max(length(viewPosition - intersection), 0.1);
        float density = W.fresnelBlurFogDensityExposure.z;
        float lightScale = 1.0 / max(W.cameraRoute.z, 0.3);
        float extinction = density + lightScale * eyeSlope;
        float scatter = pow(min(
            (-density * pow(0.98, lightScale * eyeDepth) / extinction) *
            (pow(0.98, extinction * waterDepth) - 1.0), 1.0), 1.0 / 1.7);
        float attenuation = pow(0.98, waterDepth * density);
        vec3 color = refraction * attenuation +
                     srgbToLinear(W.fogColorTonemap.rgb) * scatter;
        outColor = max(vec4(color, 1.0), vec4(0.0));
        return;
    }
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
    outColor = vec4(max(color, vec3(0.0)), 1.0);
}
