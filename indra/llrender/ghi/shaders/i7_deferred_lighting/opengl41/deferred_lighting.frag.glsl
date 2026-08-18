#version 410 core

layout(std140) uniform LightingData
{
    mat4 viewMatrix;
    mat4 projectionMatrix;
    vec4 cameraPointCount;
    vec4 ambientColor;
    vec4 sunDirectionActive;
    vec4 sunColorIntensity;
    vec4 moonDirectionActive;
    vec4 moonColorIntensity;
    vec4 pointPositionRadius[64];
    vec4 pointColorFalloff[64];
    mat4 shadowMatrix[4];
    vec4 shadowClipPlanes;
    vec4 shadowMeta;
};
uniform sampler2D baseColorMap;
uniform sampler2D ormMap;
uniform sampler2D normalMap;
uniform sampler2D emissiveMap;
uniform sampler2D depthMap;
uniform sampler2D shadowMap0;
uniform sampler2D shadowMap1;
uniform sampler2D shadowMap2;
uniform sampler2D shadowMap3;
in vec2 texCoord;
layout(location = 0) out vec4 outLighting;

vec3 reconstructWorldPosition(float depth)
{
    vec3 glNdc = vec3(texCoord * 2.0 - 1.0, depth * 2.0 - 1.0);
    vec4 world = inverse(projectionMatrix * viewMatrix) * vec4(glNdc, 1.0);
    return world.xyz / max(abs(world.w), 0.000001) * sign(world.w);
}
vec3 evaluateLight(vec3 direction, vec3 radiance, vec3 base, vec3 normal,
                   vec3 viewDirection, float metallic, float roughness)
{
    const float pi = 3.14159265358979323846;
    roughness = max(roughness, 8.0 / 255.0);
    float alphaRoughness = roughness * roughness;
    vec3 diffuseColor = base * (vec3(1.0) - vec3(0.04)) * (1.0 - metallic);
    vec3 reflectance0 = mix(vec3(0.04), base, metallic);
    float reflectance = max(max(reflectance0.r, reflectance0.g), reflectance0.b);
    vec3 reflectance90 = vec3(clamp(reflectance * 25.0, 0.0, 1.0));
    vec3 halfDirection = normalize(direction + viewDirection);
    float ndotl = clamp(dot(normal, direction), 0.001, 1.0);
    float ndotv = clamp(abs(dot(normal, viewDirection)), 0.001, 1.0);
    float ndoth = clamp(dot(normal, halfDirection), 0.0, 1.0);
    float vdoth = clamp(dot(viewDirection, halfDirection), 0.0, 1.0);
    vec3 fresnel = reflectance0 + (reflectance90 - reflectance0) *
        pow(clamp(1.0 - vdoth, 0.0, 1.0), 5.0);
    float r2 = alphaRoughness * alphaRoughness;
    float attenuationL = 2.0 * ndotl /
        (ndotl + sqrt(r2 + (1.0 - r2) * ndotl * ndotl));
    float attenuationV = 2.0 * ndotv /
        (ndotv + sqrt(r2 + (1.0 - r2) * ndotv * ndotv));
    float denominator = (ndoth * r2 - ndoth) * ndoth + 1.0;
    float distribution = r2 / (pi * denominator * denominator);
    vec3 diffuse = (1.0 - fresnel) * diffuseColor / pi;
    vec3 specular = fresnel * attenuationL * attenuationV * distribution /
        (4.0 * ndotl * ndotv);
    return radiance * ndotl * (diffuse + specular);
}
float legacyAttenuation(float distance, float falloff)
{
    float attenuation = 1.0 - clamp((distance + falloff) /
        (1.0 + falloff), 0.0, 1.0);
    return attenuation * attenuation * 2.0;
}
float shadowDepth(uint cascade, vec2 uv)
{
    if (cascade == 0u) return texture(shadowMap0, uv).r;
    if (cascade == 1u) return texture(shadowMap1, uv).r;
    if (cascade == 2u) return texture(shadowMap2, uv).r;
    return texture(shadowMap3, uv).r;
}
float directionalShadow(vec3 worldPosition)
{
    if (shadowMeta.y < 0.5) return 1.0;
    vec4 viewPosition = viewMatrix * vec4(worldPosition, 1.0);
    float distance = -viewPosition.z;
    uint count = clamp(uint(shadowMeta.z + 0.5), 1u, 4u);
    uint cascade = 0u;
    while (cascade + 1u < count && distance > shadowClipPlanes[cascade])
        ++cascade;
    vec4 projected = shadowMatrix[cascade] * viewPosition;
    if (abs(projected.w) < 0.000001) return 1.0;
    vec3 coord = projected.xyz / projected.w;
    if (any(lessThan(coord, vec3(0.0))) ||
        any(greaterThan(coord, vec3(1.0)))) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(shadowMap0, 0));
    float visible = 0.0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            visible += coord.z - shadowMeta.x <= shadowDepth(cascade,
                coord.xy + (vec2(x, y) - vec2(0.5)) * texel) ? 1.0 : 0.0;
    return visible * 0.25;
}
void main()
{
    float depth = texture(depthMap, texCoord).r;
    if (depth <= 0.0) { outLighting = vec4(0.0); return; }
    vec3 base = texture(baseColorMap, texCoord).rgb;
    vec3 orm = texture(ormMap, texCoord).rgb;
    vec3 normal = normalize(texture(normalMap, texCoord).xyz * 2.0 - 1.0);
    vec3 result = texture(emissiveMap, texCoord).rgb + base * ambientColor.rgb * orm.r;
    vec3 worldPosition = reconstructWorldPosition(depth);
    vec3 viewDirection = normalize(cameraPointCount.xyz - worldPosition);
    if (sunDirectionActive.w > 0.5)
        result += evaluateLight(normalize(-sunDirectionActive.xyz),
            sunColorIntensity.rgb * sunColorIntensity.w, base, normal,
            viewDirection, orm.b, orm.g) * directionalShadow(worldPosition);
    if (moonDirectionActive.w > 0.5)
        result += evaluateLight(normalize(-moonDirectionActive.xyz),
            moonColorIntensity.rgb * moonColorIntensity.w, base, normal,
            viewDirection, orm.b, orm.g) * directionalShadow(worldPosition);
    int pointCount = min(int(cameraPointCount.w + 0.5), 64);
    for (int index = 0; index < pointCount; ++index)
    {
        vec3 delta = pointPositionRadius[index].xyz - worldPosition;
        float radius = max(pointPositionRadius[index].w, 0.000001);
        float distanceSquared = dot(delta, delta);
        if (distanceSquared >= radius * radius) continue;
        float attenuation = legacyAttenuation(sqrt(distanceSquared) / radius,
            pointColorFalloff[index].w);
        result += evaluateLight(normalize(delta), pointColorFalloff[index].rgb * attenuation * 3.25,
            base, normal, viewDirection, orm.b, orm.g);
    }
    outLighting = vec4(result, 1.0);
}
