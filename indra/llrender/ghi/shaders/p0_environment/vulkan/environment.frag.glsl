#version 450

layout(set = 0, binding = 0, std140) uniform EnvironmentData
{
    mat4 mvp; vec4 cameraRoute; vec4 lightSunUp;
    vec4 sunlightCloudShadow; vec4 moonlightMaxAltitude;
    vec4 ambientDensityMultiplier; vec4 blueHorizonHazeHorizon;
    vec4 blueDensityHazeDensity; vec4 glowSunMoonFactor;
    vec4 cloudColorScale; vec4 cloudPositionDensity1;
    vec4 cloudPositionDensity2; vec4 cloudVarianceBlendMoonStar;
    vec4 starPhaseEmissiveMoistureDroplet;
    vec4 iceHdriSplitExposureRotation;
} environment;
layout(set = 0, binding = 1) uniform sampler2D primaryTexture;
layout(set = 0, binding = 2) uniform sampler2D secondaryTexture;
layout(set = 0, binding = 3) uniform sampler2D rainbowTexture;
layout(set = 0, binding = 4) uniform sampler2D haloTexture;
#define E environment
layout(location = 0) in vec3 hazeColor;
layout(location = 1) in float lightDot;
layout(location = 2) in vec3 cloudSun;
layout(location = 3) in vec3 cloudAmbient;
layout(location = 4) in float cloudDensity;
layout(location = 5) in vec2 texCoord0;
layout(location = 6) in vec2 texCoord1;
layout(location = 7) in vec2 texCoord2;
layout(location = 8) in vec2 texCoord3;
layout(location = 9) in float altitudeBlend;
layout(location = 10) in vec4 vertexColor;
layout(location = 11) in vec2 screenPosition;
layout(location = 12) in vec3 relativePosition;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outSpecular;
layout(location = 2) out vec4 outNormalFlag;
layout(location = 3) out vec4 outEmissive;

vec4 cloudNoise(vec2 uv)
{
    return mix(texture(primaryTexture, uv), texture(secondaryTexture, uv),
               E.cloudVarianceBlendMoonStar.y);
}
void storeColor(vec4 color, float flag)
{
    outColor = E.starPhaseEmissiveMoistureDroplet.y > 0.5 ? vec4(0.0) : color;
    outEmissive = E.starPhaseEmissiveMoistureDroplet.y > 0.5 ? color : vec4(0.0);
    outSpecular = vec4(0.0);
    outNormalFlag = vec4(0.0, 0.0, 0.0, flag);
}
void main()
{
    int route = int(E.cameraRoute.w + 0.5);
    if (route == 1)
    {
        storeColor(mix(texture(primaryTexture, texCoord0),
            texture(secondaryTexture, texCoord0), E.cloudVarianceBlendMoonStar.y), 0.0);
        return;
    }
    if (route == 2)
    {
        vec4 color = texture(primaryTexture, texCoord0);
        if (color.a <= 2.0 / 255.0) discard;
        float fade = E.moonlightMaxAltitude.z > 0.0 ?
            clamp(E.moonlightMaxAltitude.z * E.moonlightMaxAltitude.z * 4.0, 0.0, 1.0) : 1.0;
        color.rgb *= E.cloudVarianceBlendMoonStar.z; color.a *= fade;
        storeColor(color, 0.0); return;
    }
    if (route == 3)
    {
        vec4 color = texture(primaryTexture, texCoord0);
        color.rgb *= vertexColor.rgb;
        color.a *= smoothstep(0.0, 0.9, E.cloudVarianceBlendMoonStar.w) * 32.0;
        color.a *= abs(fract(screenPosition.x + screenPosition.y));
        storeColor(color, 0.0); return;
    }
    if (route == 4)
    {
        if (E.cloudColorScale.w < 0.001) discard;
        vec2 disturbance = vec2(cloudNoise(texCoord0 / 8.0).x,
            cloudNoise((texCoord2 + texCoord0) / 16.0).x) *
            E.cloudVarianceBlendMoonStar.x * (1.0 - E.cloudColorScale.w * 0.25);
        vec2 disturbance2 = vec2(cloudNoise((texCoord0 + texCoord2) / 4.0).x,
            cloudNoise((texCoord3 + texCoord1) / 8.0).x) *
            E.cloudVarianceBlendMoonStar.x * (1.0 - E.cloudColorScale.w * 0.25);
        vec2 uv1 = texCoord0 + E.cloudPositionDensity1.xy + disturbance * 0.2;
        vec2 uv2 = texCoord1 + E.cloudPositionDensity1.xy;
        vec2 uv3 = texCoord2 + E.cloudPositionDensity2.xy;
        float variance = min(1.0, (disturbance.x * 2.0 + disturbance.y * 2.0 +
            disturbance2.x + disturbance2.y) * 4.0);
        float density = cloudDensity * (1.0 - variance * variance);
        float alpha1 = (cloudNoise(uv1).x - 0.5) +
            (cloudNoise(uv3).x - 0.5) * E.cloudPositionDensity2.z;
        alpha1 = min(max(alpha1 + density, 0.0) * 10.0 * E.cloudPositionDensity1.z, 1.0);
        alpha1 = 1.0 - alpha1 * alpha1; alpha1 = 1.0 - alpha1 * alpha1;
        alpha1 = clamp(alpha1 * altitudeBlend, 0.0, 1.0);
        float alpha2 = min(max((cloudNoise(uv2).x - 0.5) + density, 0.0) *
            2.5 * E.cloudPositionDensity1.z, 1.0);
        alpha2 = 1.0 - alpha2; alpha2 = 1.0 - alpha2 * alpha2;
        vec3 color = clamp(cloudSun * (1.0 - alpha2) + cloudAmbient,
                           vec3(0.0), vec3(1.0)) * 2.0;
        storeColor(vec4(color, alpha1), 0.0); return;
    }
    if (route == 5)
    {
        vec3 direction = normalize(relativePosition);
        float angle = E.iceHdriSplitExposureRotation.w;
        direction.xz = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) * direction.xz;
        vec2 uv = vec2(atan(direction.z, direction.x) + 3.14159265, acos(direction.y)) /
            vec2(6.2831853, 3.14159265);
        storeColor(vec4(min(texture(primaryTexture, uv).rgb *
            E.iceHdriSplitExposureRotation.z, vec3(1073741824.0)), 1.0), 1.0);
        return;
    }
    float rainbowCoord = clamp(-0.575 - lightDot, 0.0, 1.0);
    rainbowCoord = clamp(rainbowCoord, 0.0, 0.25) +
        max(0.0, rainbowCoord - 0.25) * 4.2857;
    float radius = (E.starPhaseEmissiveMoistureDroplet.w - 5.0) / 1024.0;
    vec3 rainbow = pow(texture(rainbowTexture, vec2(radius + 0.5, rainbowCoord)).rgb,
        vec3(1.8)) * E.starPhaseEmissiveMoistureDroplet.z;
    float d = clamp(lightDot, 0.1, 1.0);
    vec3 halo = texture(haloTexture,
        vec2(0.0, sqrt(clamp(1.0 - d * d, 0.0, 1.0)))).rgb *
        E.iceHdriSplitExposureRotation.x;
    storeColor(vec4(clamp((hazeColor + rainbow + halo) * 2.0,
        vec3(0.0), vec3(5.0)), 1.0), 0.0);
}
