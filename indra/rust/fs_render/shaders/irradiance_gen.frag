#version 450
// Reflection-probe IRRADIANCE convolution -- faithful port of class2/interface/irradianceGenF.glsl
// (Khronos glTF-Sample-Viewer cosine-weighted hemisphere integral). Produces the diffuse irradiance cube
// from the source environment. wgpu-shaped: samplerCubeArray -> separate textureCubeArray+sampler; loose
// uniforms -> the shared GenParams UBO (binding 2). GOTCHAS reproduced verbatim: 32 cosine samples,
// LOD bias +2.0, `u_width` HARDCODED to 64 (not the real probe resolution), and generateTBN uses a FIXED
// bitangent (0,1,0) (the pole-robustness branch is disabled in stock). vary_dir is NOT normalized (stock).
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

#define MATH_PI 3.1415926535897932384626433832795

float u_roughness = 1.0;
int u_sampleCount = 32;
float u_lodBias = 2.0;
int u_width = 64; // stock hardcodes this regardless of actual probe resolution

float radicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley2d(int i, int N) {
    return vec2(float(i) / float(N), radicalInverse_VdC(uint(i)));
}

mat3 generateTBN(vec3 normal) {
    vec3 bitangent = vec3(0.0, 1.0, 0.0);
    // stock: the pole-robustness branch is commented out -> fixed bitangent (0,1,0).
    vec3 tangent = normalize(cross(bitangent, normal));
    bitangent = cross(normal, tangent);
    return mat3(tangent, bitangent, normal);
}

struct MicrofacetDistributionSample {
    float pdf;
    float cosTheta;
    float sinTheta;
    float phi;
};

MicrofacetDistributionSample Lambertian(vec2 xi, float roughness) {
    MicrofacetDistributionSample lambertian;
    lambertian.cosTheta = sqrt(1.0 - xi.y);
    lambertian.sinTheta = sqrt(xi.y);
    lambertian.phi = 2.0 * MATH_PI * xi.x;
    lambertian.pdf = lambertian.cosTheta / MATH_PI;
    return lambertian;
}

vec4 getImportanceSample(int sampleIndex, vec3 N, float roughness) {
    vec2 xi = hammersley2d(sampleIndex, u_sampleCount);
    MicrofacetDistributionSample importanceSample;
    importanceSample = Lambertian(xi, roughness);
    vec3 localSpaceDirection = normalize(vec3(
        importanceSample.sinTheta * cos(importanceSample.phi),
        importanceSample.sinTheta * sin(importanceSample.phi),
        importanceSample.cosTheta
    ));
    mat3 TBN = generateTBN(N);
    vec3 direction = TBN * localSpaceDirection;
    return vec4(direction, importanceSample.pdf);
}

float computeLod(float pdf) {
    float lod = 0.5 * log2(6.0 * float(u_width) * float(u_width) / (float(u_sampleCount) * pdf));
    return lod;
}

vec4 filterColor(vec3 N) {
    vec4 color = vec4(0.0);
    for (int i = 0; i < u_sampleCount; ++i) {
        vec4 importanceSample = getImportanceSample(i, N, 1.0);
        vec3 H = vec3(importanceSample.xyz);
        float pdf = importanceSample.w;
        float lod = computeLod(pdf);
        lod += u_lodBias;
        lod = clamp(lod, 0.0, gp.max_probe_lod);
        vec4 lambertian = textureLod(samplerCubeArray(reflectionProbes_tex, probe_smp), vec4(H, gp.sourceIdx), lod);
        color += lambertian;
    }
    color /= float(u_sampleCount);
    return color;
}

void main() {
    vec4 color = vec4(0);
    color = filterColor(vary_dir);
    frag_color = max(color, vec4(0));
}
