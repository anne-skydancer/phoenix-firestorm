#version 450
// Tier-1 water: fog-integral base (waterFogF.glsl getWaterFogViewNoClip transcribed)
// + fresnel ambient tint + normal-map waves + sun glint. Opaque, no screen refraction
// (tier-2 needs offscreen passes).
layout(location = 0) in vec2 v_bigWave;
layout(location = 1) in vec4 v_littleWave;
layout(location = 2) in vec3 v_view;
layout(set = 0, binding = 0) uniform U {
    mat4 mvp;
    vec4 a0; vec4 a1; vec4 a2; vec4 a3; vec4 a4; vec4 a5; vec4 a6; vec4 a7;
    vec4 a8; vec4 a9; vec4 a10; vec4 a11;
} u;
layout(set = 0, binding = 1) uniform texture2D bump0;
layout(set = 0, binding = 2) uniform texture2D bump1;
layout(set = 0, binding = 3) uniform sampler smp;
layout(location = 0) out vec4 frag;

vec3 srgb_to_linear(vec3 c) {
    return pow(c, vec3(2.2));
}

void main() {
    float fresnelScale = u.a2.w;
    float fresnelOffset = u.a3.x;
    float waterHeight = u.a3.z;
    float fogDensity = u.a3.w;
    vec3 fogColor = u.a4.xyz;
    float fogKS = u.a4.w;
    vec3 lightDir = u.a5.xyz;
    float underwater = u.a5.w;
    vec3 lightDiffuse = u.a6.xyz;
    float bf = u.a6.w;
    vec3 ambient = u.a7.xyz;
    float eyeZ = u.a0.z;

    vec3 w1 = mix(texture(sampler2D(bump0, smp), v_bigWave).xyz,       texture(sampler2D(bump1, smp), v_bigWave).xyz,       bf) * 2.0 - 1.0;
    vec3 w2 = mix(texture(sampler2D(bump0, smp), v_littleWave.xy).xyz, texture(sampler2D(bump1, smp), v_littleWave.xy).xyz, bf) * 2.0 - 1.0;
    vec3 w3 = mix(texture(sampler2D(bump0, smp), v_littleWave.zw).xyz, texture(sampler2D(bump1, smp), v_littleWave.zw).xyz, bf) * 2.0 - 1.0;
    vec3 wavef = normalize((w1 + w2 * 0.4 + w3 * 0.6) * 0.5 + vec3(0.0, 0.0, 1.0));

    vec3 V = normalize(v_view);

    // fresnel (waterF.glsl:164-181 structure)
    float df = max(0.0, dot(-V, wavef) * fresnelScale + fresnelOffset);
    df *= df;

    // fog integral, camera-relative frame (waterFogF.glsl getWaterFogViewNoClip)
    vec3 plane_n = vec3(0.0, 0.0, 1.0);
    float plane_w = eyeZ - waterHeight;
    vec3 fpos = V * 2048.0;
    float es = -dot(V, plane_n);
    float e0 = max(-plane_w, 0.0);
    vec3 int_v = (plane_w > 0.0) ? V * plane_w / max(es, 1e-4) : vec3(0.0);
    float l = max(length(fpos - int_v), 0.1);
    float t1 = -fogDensity * pow(0.98, fogKS * e0);
    float t2 = max(fogDensity + fogKS * es, 1e-4);
    float t3 = pow(0.98, t2 * l) - 1.0;
    float L = pow(clamp(t1 / t2 * t3, 0.0, 1.0), 1.0 / 1.7);
    vec3 base = srgb_to_linear(fogColor) * L;

    // fresnel sky/ambient tint + sun glint
    vec3 refl_tint = ambient + lightDiffuse * 0.25;
    vec3 color = mix(base, refl_tint, clamp(df, 0.0, 0.6));
    vec3 R = reflect(V, wavef);
    float glint = pow(max(dot(R, lightDir), 0.0), 128.0);
    color += lightDiffuse * glint;

    if (underwater > 0.5) {
        color = srgb_to_linear(fogColor) * 0.6 + color * 0.2;
    }

    frag = vec4(color, 1.0);
}
