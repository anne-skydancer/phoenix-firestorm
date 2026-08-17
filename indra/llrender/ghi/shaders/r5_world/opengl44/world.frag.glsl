#version 440 core

layout(std140, binding = 0) uniform WorldData
{
    mat4 viewProjection;
    vec4 terrainTransform[4];
    vec4 terrainParams;
    vec4 sunDirectionIntensity;
    vec4 sunColor;
    vec4 moonDirectionIntensity;
    vec4 moonColor;
    vec4 localPositionRadius;
    vec4 localColorFalloff;
    vec4 projectorDirectionCutoff;
    vec4 projectorColorShadowBias;
    vec4 environmentParams;
    vec4 skyZenith;
    vec4 skyHorizon;
    vec4 waterColorTime;
    vec4 waveDirections;
};

layout(binding = 0) uniform sampler2D terrainWeights;
layout(binding = 1) uniform sampler2D terrainLayer0;
layout(binding = 2) uniform sampler2D terrainLayer1;
layout(binding = 3) uniform sampler2D terrainLayer2;
layout(binding = 4) uniform sampler2D terrainLayer3;
layout(binding = 5) uniform sampler2D shadowProjector;

layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec3 worldPosition;
layout(location = 2) in vec3 worldNormal;
layout(location = 0) out vec4 outTerrain;
layout(location = 1) out vec4 outLighting;
layout(location = 2) out vec4 outEnvironment;

vec2 transformedUv(vec2 uv, vec4 transform)
{
    return uv * transform.xy + transform.zw;
}

void main()
{
    vec4 weights = texture(terrainWeights, texCoord);
    vec4 layerHeight = vec4(
        texture(terrainLayer0, transformedUv(texCoord, terrainTransform[0])).a,
        texture(terrainLayer1, transformedUv(texCoord, terrainTransform[1])).a,
        texture(terrainLayer2, transformedUv(texCoord, terrainTransform[2])).a,
        texture(terrainLayer3, transformedUv(texCoord, terrainTransform[3])).a);
    weights *= mix(vec4(1.0), vec4(0.5) + layerHeight, terrainParams.y);
    weights /= max(dot(weights, vec4(1.0)), 0.000001);
    vec3 terrain =
        texture(terrainLayer0, transformedUv(texCoord, terrainTransform[0])).rgb * weights.x +
        texture(terrainLayer1, transformedUv(texCoord, terrainTransform[1])).rgb * weights.y +
        texture(terrainLayer2, transformedUv(texCoord, terrainTransform[2])).rgb * weights.z +
        texture(terrainLayer3, transformedUv(texCoord, terrainTransform[3])).rgb * weights.w;
    vec3 terrainNormal = normalize(vec3(
        (layerHeight.y - layerHeight.x) * terrainParams.w,
        (layerHeight.w - layerHeight.z) * terrainParams.w, 1.0));
    vec3 pbrTerrain = terrain * (vec3(0.75) + terrainNormal * vec3(0.125));
    outTerrain = vec4(mix(terrain, pbrTerrain, terrainParams.z), 1.0);

    vec3 normal = normalize(worldNormal + terrainNormal * 0.25);
    float sun = max(dot(normal, -sunDirectionIntensity.xyz), 0.0) * sunDirectionIntensity.w;
    float moon = max(dot(normal, -moonDirectionIntensity.xyz), 0.0) * moonDirectionIntensity.w;
    vec3 localDelta = localPositionRadius.xyz - worldPosition;
    float localAttenuation = clamp(1.0 - dot(localDelta, localDelta) /
        max(localPositionRadius.w * localPositionRadius.w, 0.000001), 0.0, 1.0);
    localAttenuation *= mix(1.0, localAttenuation, localColorFalloff.w);
    float projectorFacing = step(projectorDirectionCutoff.w,
        dot(normalize(localDelta), -projectorDirectionCutoff.xyz));
    float projectedShadow = step(projectorColorShadowBias.w,
        texture(shadowProjector, texCoord).r);
    vec3 light = sunColor.rgb * sun + moonColor.rgb * moon +
        localColorFalloff.rgb * localAttenuation +
        projectorColorShadowBias.rgb * projectorFacing * projectedShadow;
    outLighting = vec4(terrain * light, 1.0);

    vec3 sky = mix(skyHorizon.rgb, skyZenith.rgb,
        clamp((texCoord.y + environmentParams.x) / (1.0 + environmentParams.x), 0.0, 1.0));
    vec2 waveUv = texCoord + waveDirections.xy * waterColorTime.w +
        waveDirections.zw * (waterColorTime.w * 0.5);
    vec3 waterNormal = normalize(texture(terrainLayer3, waveUv).xyz * 2.0 - 1.0);
    float facing = clamp(waterNormal.z, 0.0, 1.0);
    float fresnel = environmentParams.z + (1.0 - environmentParams.z) *
        (1.0 - facing) * (1.0 - facing);
    vec3 water = mix(waterColorTime.rgb, sky, fresnel);
    vec3 environment = texCoord.y < environmentParams.y ? water : sky;
    environment = mix(environment, waterColorTime.rgb * 0.625,
        clamp(environmentParams.w, 0.0, 1.0));
    outEnvironment = vec4(environment, 1.0);
}
