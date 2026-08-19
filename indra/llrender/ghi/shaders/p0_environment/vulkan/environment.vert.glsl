#version 450
#extension GL_GOOGLE_include_directive : require
#include "ghi_vulkan_clip.glsl"

layout(set = 0, binding = 0, std140) uniform EnvironmentData
{
    mat4 mvp;
    vec4 cameraRoute;
    vec4 lightSunUp;
    vec4 sunlightCloudShadow;
    vec4 moonlightMaxAltitude;
    vec4 ambientDensityMultiplier;
    vec4 blueHorizonHazeHorizon;
    vec4 blueDensityHazeDensity;
    vec4 glowSunMoonFactor;
    vec4 cloudColorScale;
    vec4 cloudPositionDensity1;
    vec4 cloudPositionDensity2;
    vec4 cloudVarianceBlendMoonStar;
    vec4 starPhaseEmissiveMoistureDroplet;
    vec4 iceHdriSplitExposureRotation;
} environment;

#define E environment
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec4 inColor;
layout(location = 0) out vec3 hazeColor;
layout(location = 1) out float lightDot;
layout(location = 2) out vec3 cloudSun;
layout(location = 3) out vec3 cloudAmbient;
layout(location = 4) out float cloudDensity;
layout(location = 5) out vec2 texCoord0;
layout(location = 6) out vec2 texCoord1;
layout(location = 7) out vec2 texCoord2;
layout(location = 8) out vec2 texCoord3;
layout(location = 9) out float altitudeBlend;
layout(location = 10) out vec4 vertexColor;
layout(location = 11) out vec2 screenPosition;
layout(location = 12) out vec3 relativePosition;

vec4 atDepth(vec4 clip, float depth)
{
    clip.z = (depth * 2.0 - 1.0) * clip.w;
    return ghiToVulkanClip(clip);
}

void main()
{
    int route = int(E.cameraRoute.w + 0.5);
    vec4 clip = E.mvp * vec4(inPosition, 1.0);
    float depth = 0.00001;
    if (route == 1)
    {
        clip = E.mvp * vec4(inPosition - vec3(0.0, 0.0, 50.0), 1.0);
        depth = 0.000001;
    }
    else if (route == 2) depth = 0.000009;
    else if (route == 3) depth = 0.0;
    gl_Position = atDepth(clip, depth);
    texCoord0 = inTexCoord; texCoord1 = inTexCoord;
    texCoord2 = inTexCoord; texCoord3 = inTexCoord;
    vertexColor = inColor;
    screenPosition = inPosition.xy * vec2(mod(E.starPhaseEmissiveMoistureDroplet.x, 1.25));
    relativePosition = inPosition - E.cameraRoute.xyz + vec3(0.0, 50.0, 0.0);
    hazeColor = vec3(0.0); lightDot = 0.0;
    cloudSun = vec3(0.0); cloudAmbient = vec3(0.0);
    cloudDensity = 0.0; altitudeBlend = 0.0;
    if (route != 0 && route != 4 && route != 5) return;

    vec3 rel = relativePosition;
    if (route == 4)
    {
        texCoord0 = vec2(-inTexCoord.x, inTexCoord.y);
        texCoord0 = (texCoord0 - 0.5) / E.cloudColorScale.w + 0.5;
        texCoord1 = texCoord0 + E.lightSunUp.xz * 0.0125;
        texCoord2 = texCoord0 * 16.0; texCoord3 = texCoord1 * 16.0;
        altitudeBlend = clamp((rel.y + 512.0) / E.moonlightMaxAltitude.w, 0.0, 1.0);
    }
    if (rel.y > 0.0) rel *= E.moonlightMaxAltitude.w / rel.y;
    if (rel.y < 0.0)
    {
        if (route == 4) altitudeBlend = 0.0;
        rel *= -32000.0 / rel.y;
    }
    vec3 relNorm = normalize(rel);
    float relLength = length(rel);
    lightDot = dot(relNorm, E.lightSunUp.xyz);
    vec3 sunlight = E.lightSunUp.w > 0.5 ? E.sunlightCloudShadow.rgb :
        E.moonlightMaxAltitude.rgb * 0.7;
    vec3 attenuation = (E.blueDensityHazeDensity.rgb +
        vec3(E.blueDensityHazeDensity.w * 0.25)) *
        (E.ambientDensityMultiplier.w * E.moonlightMaxAltitude.w);
    vec3 combined = max(abs(E.blueDensityHazeDensity.rgb) +
        vec3(abs(E.blueDensityHazeDensity.w)), vec3(1e-6));
    vec3 blueWeight = E.blueDensityHazeDensity.rgb / combined;
    vec3 hazeWeight = E.blueDensityHazeDensity.w / combined;
    float offAxis = 1.0 / max(1e-6, max(0.0, relNorm.y) + E.lightSunUp.y);
    sunlight *= exp(-attenuation * offAxis);
    combined = exp(-combined * (relLength * E.ambientDensityMultiplier.w));
    float hazeGlow = max(1.0 - lightDot, 0.001) * E.glowSunMoonFactor.x;
    hazeGlow = pow(hazeGlow, E.glowSunMoonFactor.z);
    hazeGlow = E.glowSunMoonFactor.w < 1.0 ? 0.0 :
        E.glowSunMoonFactor.w * (hazeGlow + 0.25);
    vec3 ambient = E.ambientDensityMultiplier.rgb +
        max(vec3(0.0), vec3(1.0) - E.ambientDensityMultiplier.rgb) *
        E.sunlightCloudShadow.w * 0.5;
    vec3 above = E.blueHorizonHazeHorizon.rgb * blueWeight *
        (sunlight + E.ambientDensityMultiplier.rgb) +
        E.blueHorizonHazeHorizon.w * hazeWeight *
        (sunlight * hazeGlow + E.ambientDensityMultiplier.rgb);
    above *= 1.0 - combined;
    sunlight *= max(0.0, 1.0 - E.sunlightCloudShadow.w);
    vec3 below = E.blueHorizonHazeHorizon.rgb * blueWeight * (sunlight + ambient) +
        E.blueHorizonHazeHorizon.w * hazeWeight * (sunlight * hazeGlow + ambient);
    combined = sqrt(combined);
    hazeColor = above + (below - above) * (1.0 - sqrt(combined));
    if (route == 4)
    {
        sunlight = E.sunlightCloudShadow.rgb;
        offAxis = 1.0 / max(1e-6, E.lightSunUp.y * 2.0);
        sunlight *= exp(-attenuation * offAxis);
        hazeGlow = max(1.0 - dot(relNorm, E.lightSunUp.xyz), 0.001) *
            E.glowSunMoonFactor.x;
        hazeGlow = pow(hazeGlow, E.glowSunMoonFactor.z) * E.glowSunMoonFactor.w;
        hazeGlow = E.glowSunMoonFactor.w < 1.0 ? 0.0 : hazeGlow + 0.25;
        cloudSun = sunlight * hazeGlow * E.cloudColorScale.rgb * combined;
        cloudAmbient = ambient * E.cloudColorScale.rgb * combined +
            below * (1.0 - combined);
        cloudDensity = 2.0 * (E.sunlightCloudShadow.w - 0.25);
    }
}
