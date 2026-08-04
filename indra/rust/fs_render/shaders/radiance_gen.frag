#version 450
// Reflection-probe RADIANCE prefilter -- faithful port of class1/interface/radianceGenF.glsl (Sascha
// Willems GGX importance-sample prefilter). Produces one glossy mip of the radiance cube from the source
// environment's prefiltered mip chain. wgpu-shaped: samplerCubeArray -> separate textureCubeArray+sampler;
// loose uniforms -> the shared GenParams UBO (binding 2). GOTCHA reproduced: roughness is derived from
// mipLevel/max_probe_lod (the C++ `roughness` uniform is IGNORED by stock), and sample count scales with
// roughness (mip 0 -> 1 sample -> passthrough of the base env). Per-sample source LOD is the solid-angle
// ratio mip bias (+1.0), sampling the prefiltered mip chain in the scratch layer (sourceIdx).
layout(location = 0) in vec3 vary_dir;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform textureCubeArray reflectionProbes_tex;
layout(set = 0, binding = 1) uniform sampler probe_smp;
layout(set = 0, binding = 2) uniform GenParams {
    mat4 modelview;
    float mipLevel;
    float max_probe_lod;
    float probe_strength;
    int sourceIdx;
    int u_width;
} gp;

#define PROBE_FILTER_SAMPLES 32
const float PI = 3.1415926536;

float random(vec2 co) {
    float a = 12.9898;
    float b = 78.233;
    float c = 43758.5453;
    float dt = dot(co.xy, vec2(a, b));
    float sn = mod(dt, 3.14);
    return fract(sin(sn) * c);
}

vec2 hammersley2d(uint i, uint N) {
    uint bits = (i << 16u) | (i >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float rdi = float(bits) * 2.3283064365386963e-10;
    return vec2(float(i) / float(N), rdi);
}

vec3 importanceSample_GGX(vec2 Xi, float roughness, vec3 normal) {
    float alpha = roughness * roughness;
    float phi = 2.0 * PI * Xi.x + random(normal.xz) * 0.1;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (alpha * alpha - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangentX = normalize(cross(up, normal));
    vec3 tangentY = normalize(cross(normal, tangentX));

    return normalize(tangentX * H.x + tangentY * H.y + normal * H.z);
}

float D_GGX(float dotNH, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = dotNH * dotNH * (alpha2 - 1.0) + 1.0;
    return (alpha2) / (PI * denom * denom);
}

vec4 prefilterEnvMap(vec3 R) {
    vec3 N = R;
    vec3 V = R;
    vec4 color = vec4(0.0);
    float totalWeight = 0.0;
    float envMapDim = float(textureSize(samplerCubeArray(reflectionProbes_tex, probe_smp), 0).s);
    float roughness = gp.mipLevel / gp.max_probe_lod;
    uint numSamples = uint(max(PROBE_FILTER_SAMPLES * roughness, 1));

    for (uint i = 0u; i < numSamples; i++) {
        vec2 Xi = hammersley2d(i, numSamples);
        vec3 H = importanceSample_GGX(Xi, roughness, N);
        vec3 L = 2.0 * dot(V, H) * H - V;
        float dotNL = clamp(dot(N, L), 0.0, 1.0);
        if (dotNL > 0.0) {
            float dotNH = clamp(dot(N, H), 0.0, 1.0);
            float dotVH = clamp(dot(V, H), 0.0, 1.0);

            float pdf = D_GGX(dotNH, roughness) * dotNH / (4.0 * dotVH) + 0.0001;
            float omegaS = 1.0 / (float(numSamples) * pdf);
            float omegaP = 4.0 * PI / (6.0 * envMapDim * envMapDim);
            float mipLevel = roughness == 0.0 ? 0.0 : clamp(0.5 * log2(omegaS / omegaP) + 1.0, 0.0, gp.max_probe_lod);
            color += textureLod(samplerCubeArray(reflectionProbes_tex, probe_smp), vec4(L, gp.sourceIdx), mipLevel) * dotNL;
            totalWeight += dotNL;
        }
    }
    return (color / totalWeight);
}

void main() {
    vec3 N = normalize(vary_dir);
    frag_color = max(prefilterEnvMap(N), vec4(0));
    frag_color.a *= gp.probe_strength;
}
