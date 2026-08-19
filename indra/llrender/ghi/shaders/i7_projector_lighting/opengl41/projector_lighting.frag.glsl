#version 410 core

layout(std140) uniform ProjectorData
{
    mat4 viewMatrix;
    mat4 projectionMatrix;
    vec4 cameraOrigin;
    vec4 lightPositionRadius;
    vec4 lightColorFalloff;
    vec4 lightRotation;
    vec4 lightScale;
    vec4 projectorParamsLevels;
    mat4 shadowMatrix;
    vec4 shadowMeta;
};
uniform sampler2D baseColorMap;
uniform sampler2D ormMap;
uniform sampler2D normalMap;
uniform sampler2D depthMap;
uniform sampler2D projectionMap;
uniform sampler2D projectorShadowMap;
in vec2 texCoord;
layout(location = 0) out vec4 outLighting;

vec3 rotateVector(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}
vec3 inverseRotateVector(vec4 q, vec3 v)
{
    return rotateVector(vec4(-q.xyz, q.w), v);
}
float attenuation(float distance, float falloff)
{
    float value = 1.0 - clamp((distance + falloff) /
        (1.0 + falloff), 0.0, 1.0);
    return value * value * 2.0;
}
float projectorShadow(vec3 worldPosition)
{
    if (shadowMeta.w < 0.5) return 1.0;
    vec4 viewPosition = viewMatrix * vec4(worldPosition, 1.0);
    vec4 projected = shadowMatrix * viewPosition;
    if (abs(projected.w) < 0.000001) return 1.0;
    vec3 coord = projected.xyz / projected.w;
    if (any(lessThan(coord, vec3(0.0))) ||
        any(greaterThan(coord, vec3(1.0)))) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(projectorShadowMap, 0));
    float visible = 0.0;
    float bias = shadowMeta.x + shadowMeta.y;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            visible += coord.z - bias <= texture(projectorShadowMap,
                coord.xy + (vec2(x, y) - vec2(0.5)) * texel).r ? 1.0 : 0.0;
    return clamp(visible * 0.25 + shadowMeta.z, 0.0, 1.0);
}
void main()
{
    float depth = texture(depthMap, texCoord).r;
    if (depth <= 0.0) { outLighting = vec4(0.0); return; }
    vec3 ndc = vec3(texCoord * 2.0 - 1.0, depth * 2.0 - 1.0);
    vec4 worldValue = inverse(projectionMatrix * viewMatrix) * vec4(ndc, 1.0);
    vec3 world = worldValue.xyz /
        max(abs(worldValue.w), 0.000001) * sign(worldValue.w);
    vec4 rotation = normalize(lightRotation);
    float fov = clamp(projectorParamsLevels.x, 0.001, 3.13);
    float planeDistance = max(abs(lightScale.y) * 0.5, 0.0001) /
                          tan(fov * 0.5);
    vec3 forward = rotateVector(rotation, vec3(0.0, 0.0, -1.0));
    vec3 origin = lightPositionRadius.xyz + forward * lightScale.z * 0.5 -
                  forward * planeDistance;
    vec3 local = inverseRotateVector(rotation, world - origin);
    float axial = -local.z;
    float farDistance = lightPositionRadius.w + planeDistance - lightScale.z;
    float aspect = max(abs(lightScale.x /
        max(abs(lightScale.y), 0.0001)), 0.0001);
    vec2 uv = vec2(local.x /
        max(axial * tan(fov * 0.5) * aspect, 0.0001),
        local.y / max(axial * tan(fov * 0.5), 0.0001)) * 0.5 + 0.5;
    if (axial <= planeDistance || axial >= farDistance ||
        any(lessThanEqual(uv, vec2(0.0))) ||
        any(greaterThanEqual(uv, vec2(1.0))))
    { outLighting = vec4(0.0); return; }
    vec3 delta = lightPositionRadius.xyz - world;
    float distance = length(delta);
    float radius = max(lightPositionRadius.w, 0.000001);
    if (distance >= radius) { outLighting = vec4(0.0); return; }
    float range = max(farDistance - planeDistance, 0.0001);
    float lod = clamp((axial - planeDistance - projectorParamsLevels.y) /
        range, 0.0, 1.0) * max(projectorParamsLevels.w - 1.0, 0.0);
    vec4 projected = textureLod(projectionMap, uv, lod);
    vec3 base = texture(baseColorMap, texCoord).rgb;
    vec4 material = texture(ormMap, texCoord);
    vec4 normalSample = texture(normalMap, texCoord);
    vec3 normal = normalize(normalSample.xyz * 2.0 - 1.0);
    bool legacy = normalSample.a < 0.5;
    float ndotl = max(dot(normal, normalize(delta)), 0.0);
    float energy = attenuation(distance / radius, lightColorFalloff.w) *
                   (ndotl + max(projectorParamsLevels.z, 0.0)) * 3.25 *
                   projectorShadow(world);
    vec3 litSurface = base;
    if (legacy)
    {
        vec3 viewDirection = normalize(cameraOrigin.xyz - world);
        vec3 halfDirection = normalize(normalize(delta) + viewDirection);
        float exponent = mix(1.0, 256.0,
            clamp(material.a * material.a, 0.0, 1.0));
        litSurface += material.rgb * pow(
            max(dot(normal, halfDirection), 0.0), exponent);
    }
    else
    {
        litSurface *= mix(1.0, 0.5, material.b) *
                      mix(0.7, 1.0, 1.0 - material.g);
    }
    vec3 color = litSurface * lightColorFalloff.rgb * projected.rgb *
                 projected.a * energy;
    outLighting = vec4(max(color, vec3(0.0)), 0.0);
}
