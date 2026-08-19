#version 410 core

layout(std140) uniform EnvironmentData
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
};

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
    return clip;
}

void main()
{
    int route = int(cameraRoute.w + 0.5);
    vec4 clip = mvp * vec4(inPosition, 1.0);
    float depth = 0.00001;
    if (route == 1)
    {
        clip = mvp * vec4(inPosition - vec3(0.0, 0.0, 50.0), 1.0);
        depth = 0.000001;
    }
    else if (route == 2) depth = 0.000009;
    else if (route == 3) depth = 0.0;
    gl_Position = atDepth(clip, depth);

    texCoord0 = inTexCoord;
    texCoord1 = inTexCoord;
    texCoord2 = inTexCoord;
    texCoord3 = inTexCoord;
    vertexColor = inColor;
    screenPosition = inPosition.xy * vec2(mod(starPhaseEmissiveMoistureDroplet.x, 1.25));
    relativePosition = inPosition - cameraRoute.xyz + vec3(0.0, 50.0, 0.0);
    hazeColor = vec3(0.0);
    lightDot = 0.0;
    cloudSun = vec3(0.0);
    cloudAmbient = vec3(0.0);
    cloudDensity = 0.0;
    altitudeBlend = 0.0;
    if (route != 0 && route != 4 && route != 5) return;

    vec3 rel = relativePosition;
    if (route == 4)
    {
        texCoord0 = vec2(-inTexCoord.x, inTexCoord.y);
        texCoord0 = (texCoord0 - 0.5) / cloudColorScale.w + 0.5;
        texCoord1 = texCoord0 + lightSunUp.xz * 0.0125;
        texCoord2 = texCoord0 * 16.0;
        texCoord3 = texCoord1 * 16.0;
        altitudeBlend = clamp((rel.y + 512.0) / moonlightMaxAltitude.w, 0.0, 1.0);
    }
    if (rel.y > 0.0) rel *= moonlightMaxAltitude.w / rel.y;
    if (rel.y < 0.0)
    {
        if (route == 4) altitudeBlend = 0.0;
        rel *= -32000.0 / rel.y;
    }
    vec3 relNorm = normalize(rel);
    float relLength = length(rel);
    lightDot = dot(relNorm, lightSunUp.xyz);
    vec3 sunlight = lightSunUp.w > 0.5 ? sunlightCloudShadow.rgb :
        moonlightMaxAltitude.rgb * 0.7;
    vec3 attenuation = (blueDensityHazeDensity.rgb +
        vec3(blueDensityHazeDensity.w * 0.25)) *
        (ambientDensityMultiplier.w * moonlightMaxAltitude.w);
    vec3 combined = max(abs(blueDensityHazeDensity.rgb) +
        vec3(abs(blueDensityHazeDensity.w)), vec3(1e-6));
    vec3 blueWeight = blueDensityHazeDensity.rgb / combined;
    vec3 hazeWeight = blueDensityHazeDensity.w / combined;
    float offAxis = 1.0 / max(1e-6, max(0.0, relNorm.y) + lightSunUp.y);
    sunlight *= exp(-attenuation * offAxis);
    combined = exp(-combined * (relLength * ambientDensityMultiplier.w));
    float hazeGlow = max(1.0 - lightDot, 0.001) * glowSunMoonFactor.x;
    hazeGlow = pow(hazeGlow, glowSunMoonFactor.z);
    hazeGlow = glowSunMoonFactor.w < 1.0 ? 0.0 :
        glowSunMoonFactor.w * (hazeGlow + 0.25);
    vec3 ambient = ambientDensityMultiplier.rgb +
        max(vec3(0.0), vec3(1.0) - ambientDensityMultiplier.rgb) *
        sunlightCloudShadow.w * 0.5;
    vec3 above = blueHorizonHazeHorizon.rgb * blueWeight *
        (sunlight + ambientDensityMultiplier.rgb) +
        blueHorizonHazeHorizon.w * hazeWeight *
        (sunlight * hazeGlow + ambientDensityMultiplier.rgb);
    above *= 1.0 - combined;
    sunlight *= max(0.0, 1.0 - sunlightCloudShadow.w);
    vec3 below = blueHorizonHazeHorizon.rgb * blueWeight * (sunlight + ambient) +
        blueHorizonHazeHorizon.w * hazeWeight * (sunlight * hazeGlow + ambient);
    combined = sqrt(combined);
    hazeColor = above + (below - above) * (1.0 - sqrt(combined));
    if (route == 4)
    {
        sunlight = sunlightCloudShadow.rgb;
        offAxis = 1.0 / max(1e-6, lightSunUp.y * 2.0);
        sunlight *= exp(-attenuation * offAxis);
        hazeGlow = max(1.0 - dot(relNorm, lightSunUp.xyz), 0.001) *
            glowSunMoonFactor.x;
        hazeGlow = pow(hazeGlow, glowSunMoonFactor.z) * glowSunMoonFactor.w;
        hazeGlow = glowSunMoonFactor.w < 1.0 ? 0.0 : hazeGlow + 0.25;
        cloudSun = sunlight * hazeGlow * cloudColorScale.rgb * combined;
        cloudAmbient = ambient * cloudColorScale.rgb * combined +
            below * (1.0 - combined);
        cloudDensity = 2.0 * (sunlightCloudShadow.w - 0.25);
    }
}
