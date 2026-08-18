#version 450

layout(set = 0, binding = 0, std140) uniform ProjectorData
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
} projector;
layout(set = 0, binding = 1) uniform sampler2D baseColorMap;
layout(set = 0, binding = 2) uniform sampler2D ormMap;
layout(set = 0, binding = 3) uniform sampler2D normalMap;
layout(set = 0, binding = 5) uniform sampler2D depthMap;
layout(set = 0, binding = 6) uniform sampler2D projectionMap;
layout(set = 0, binding = 7) uniform sampler2D projectorShadowMap;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outLighting;

vec3 rotateVector(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}
vec3 inverseRotateVector(vec4 q, vec3 v)
{
    return rotateVector(vec4(-q.xyz, q.w), v);
}
vec3 reconstructWorldPosition(float depth)
{
    vec3 glNdc = vec3(texCoord.x * 2.0 - 1.0,
                      1.0 - texCoord.y * 2.0, depth * 2.0 - 1.0);
    vec4 world = inverse(projector.projectionMatrix * projector.viewMatrix)
               * vec4(glNdc, 1.0);
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
float projectorShadow(vec3 worldPosition)
{
    if (projector.shadowMeta.w < 0.5) return 1.0;
    vec4 viewPosition = projector.viewMatrix * vec4(worldPosition, 1.0);
    vec4 projected = projector.shadowMatrix * viewPosition;
    if (abs(projected.w) < 0.000001) return 1.0;
    vec3 coord = projected.xyz / projected.w;
    if (any(lessThan(coord, vec3(0.0))) ||
        any(greaterThan(coord, vec3(1.0)))) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(projectorShadowMap, 0));
    float visible = 0.0;
    float bias = projector.shadowMeta.x + projector.shadowMeta.y;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            visible += coord.z - bias <= texture(projectorShadowMap,
                coord.xy + (vec2(x, y) - vec2(0.5)) * texel).r ? 1.0 : 0.0;
    return clamp(visible * 0.25 + projector.shadowMeta.z, 0.0, 1.0);
}

void main()
{
    float depth = texture(depthMap, texCoord).r;
    if (depth <= 0.0) { outLighting = vec4(0.0); return; }
    vec3 worldPosition = reconstructWorldPosition(depth);
    vec4 rotation = normalize(projector.lightRotation);
    float fov = clamp(projector.projectorParamsLevels.x, 0.001, 3.13);
    float halfHeight = max(abs(projector.lightScale.y) * 0.5, 0.0001);
    float distanceToPlane = halfHeight / tan(fov * 0.5);
    vec3 forward = rotateVector(rotation, vec3(0.0, 0.0, -1.0));
    vec3 nearCenter = projector.lightPositionRadius.xyz +
        forward * projector.lightScale.z * 0.5;
    vec3 origin = nearCenter - forward * distanceToPlane;
    vec3 local = inverseRotateVector(rotation, worldPosition - origin);
    float axialDistance = -local.z;
    float farDistance = projector.lightPositionRadius.w + distanceToPlane -
                        projector.lightScale.z;
    float aspect = max(abs(projector.lightScale.x /
                           max(abs(projector.lightScale.y), 0.0001)), 0.0001);
    vec2 projected = vec2(local.x /
        max(axialDistance * tan(fov * 0.5) * aspect, 0.0001),
        local.y / max(axialDistance * tan(fov * 0.5), 0.0001));
    vec2 uv = projected * 0.5 + 0.5;
    if (axialDistance <= distanceToPlane || axialDistance >= farDistance ||
        any(lessThanEqual(uv, vec2(0.0))) ||
        any(greaterThanEqual(uv, vec2(1.0))))
    { outLighting = vec4(0.0); return; }
    vec3 delta = projector.lightPositionRadius.xyz - worldPosition;
    float radius = max(projector.lightPositionRadius.w, 0.000001);
    float distance = length(delta);
    if (distance >= radius) { outLighting = vec4(0.0); return; }
    float range = max(farDistance - distanceToPlane, 0.0001);
    float focus = projector.projectorParamsLevels.y;
    float maxLod = max(projector.projectorParamsLevels.w - 1.0, 0.0);
    float lod = clamp((axialDistance - distanceToPlane - focus) / range,
                      0.0, 1.0) * maxLod;
    vec4 projectedColor = textureLod(projectionMap, uv, lod);
    vec2 edgeDistance = vec2(0.5) - abs(uv - vec2(0.5));
    float edge = min(edgeDistance.x, edgeDistance.y);
    projectedColor *= smoothstep(0.0, max(0.002, 0.04 * lod), edge);
    vec3 base = texture(baseColorMap, texCoord).rgb;
    vec3 orm = texture(ormMap, texCoord).rgb;
    vec3 normal = normalize(texture(normalMap, texCoord).xyz * 2.0 - 1.0);
    vec3 viewDirection = normalize(projector.cameraOrigin.xyz - worldPosition);
    float attenuation = legacyAttenuation(
        distance / radius, projector.lightColorFalloff.w);
    vec3 radiance = projector.lightColorFalloff.rgb * projectedColor.rgb *
                    projectedColor.a * attenuation * 3.25 *
                    projectorShadow(worldPosition);
    vec3 result = evaluateLight(normalize(delta), radiance, base, normal,
        viewDirection, orm.b, orm.g);
    float ambiance = max(projector.projectorParamsLevels.z, 0.0);
    result += base * projector.lightColorFalloff.rgb * projectedColor.rgb *
              projectedColor.a * attenuation * ambiance;
    outLighting = vec4(max(result, vec3(0.0)), 0.0);
}
