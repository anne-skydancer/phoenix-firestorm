#version 450
// waterV.glsl:69-106 transcription (region-space, camera-relative wave UVs).
layout(location = 0) in vec4 pos;
layout(set = 0, binding = 0) uniform U {
    mat4 mvp;
    vec4 a0;  // eyeVec.xyz, time
    vec4 a1;  // waveDir1.xy, waveDir2.xy
    vec4 a2;  // normScale.xyz, fresnelScale
    vec4 a3;  // fresnelOffset, blurMultiplier, waterHeight, fogDensity
    vec4 a4;  // fogColorLinear.rgb, fogKS
    vec4 a5;  // lightDir.xyz, underwater
    vec4 a6;  // lightDiffuse.rgb, blend_factor
    vec4 a7;  // ambient.rgb, sun_up
    vec4 a8; vec4 a9; vec4 a10; vec4 a11;
} u;
layout(location = 0) out vec2 v_bigWave;
layout(location = 1) out vec4 v_littleWave;
layout(location = 2) out vec3 v_view;

void main() {
    vec3 eyeVec = u.a0.xyz;
    float t = u.a0.w;
    vec2 waveDir1 = u.a1.xy;
    vec2 waveDir2 = u.a1.zw;

    vec3 oEyeVec = pos.xyz - eyeVec;
    float d = max(length(oEyeVec.xy), 1e-3);
    float ld = min(d, 2560.0);
    vec2 pxy = eyeVec.xy + oEyeVec.xy / d * ld;
    vec3 v = vec3(pxy, pos.z);
    v.x += (cos(v.x * 0.08) + sin(v.y * 0.02)) * 6.0;

    v_bigWave       = v.xy * vec2(0.04, 0.04) + waveDir1 * t * 0.055;
    v_littleWave.xy = v.xy * vec2(0.45, 0.9)  + waveDir2 * t * 0.13;
    v_littleWave.zw = v.xy * vec2(0.1, 0.2)   + waveDir1 * t * 0.1;
    v_view = oEyeVec;

    gl_Position = u.mvp * vec4(pos.xyz, 1.0);
}
