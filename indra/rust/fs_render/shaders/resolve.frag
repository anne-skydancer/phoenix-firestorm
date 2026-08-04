#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Ground P2b: deferred resolve -- FAITHFUL port of stock softenLightF.glsl (class3/deferred) + its
// callees pbrBaseLight/pbrPunctual/calcDiffuseSpecular (class1/deferred/deferredUtil.glsl) and
// calcAtmosphericVarsLinear (class1/windlight/atmosphericsFuncs.glsl). Lights HAS_ATMOS (legacy) +
// HAS_PBR (metallic-roughness) G-buffer pixels; DISCARDS the rest so the sky already in scene_hdr
// (drawn first, LoadOp::Load) shows through.
//
// STRATUM ORDER (load-bearing): legacy day/night -> WindLight (calcAtmosphericVars) -> EEP/HDR. The
// HDR regime seam is classic_mode (llsettingsvo.cpp:810 canAutoAdjust && !should_auto_adjust): a
// legacy WindLight sky takes the CLASSIC deconstruction (sun ×1.35, srgb round-trips, ×M_PI to cancel
// the Lambertian /pi, ×1.1 final); a PBR/EEP sky takes the linear path (Lambertian /pi, sun ×3.0).
// Getting this wrong is the daytime blowout: the old code always ran the linear branch WITHOUT the /pi,
// so PBR/EEP skies came out ~3.3x too bright (measured, fs_ogl_ref atmos_ab bench) while classic skies
// coincidentally looked right.
//
// Done in WORLD space (the deliberate divergence from stock's eye space): world normal from RT1, world
// sun_dir fed, dots are frame-invariant -> identical result. The view vector v is reconstructed from
// the pixel's view RAY (inv_view_proj) -- depth-independent, so it dodges the reverse-Z hazard.
// MATERIALS stratum (this pass): legacy Blinn-Phong direct specular (RT2 spec color+exponent) is IN.
// Still deferred to later strata:
//   - PBR punctual GGX specular + real ORM (roughness/metallic) = PBR stratum,
//   - reflection-probe IBL + env-reflection (applyGlossEnv/applyLegacyEnv) = probe stratum,
//   - emissive (RT3) = materials M2b, haze (additive/atten) = the separate hazeF pass.
//
// UBO is the shared 60-float sky UBO; indices match scene.rs sky_ubo() / live.rs set_fullscreen_sky.
layout(set = 0, binding = 0) uniform Sky {
    mat4 inv_view_proj;    // u0-15   (P2c)
    vec4 cam_maxy;         // u16-19: cam.xyz (P2c), max_y
    vec4 lightnorm_sunup;  // u20-23: lightnorm.xyz (OGL Y-up), sun_up_factor
    vec4 suncol_densmul;   // u24-27: sun_color.rgb, density_multiplier
    vec4 mooncol_glowf;    // u28-31: moon_color.rgb, sun_moon_glow_factor
    vec4 ambient_hazed;    // u32-35: ambient.rgb, haze_density
    vec4 bluehz_hazeh;     // u36-39: blue_horizon.rgb, haze_horizon
    vec4 bluedens_clouds;  // u40-43: blue_density.rgb, cloud_shadow
    vec4 glow_ssc;         // u44-47: glow.xyz, sky_sunlight_scale
    vec4 hdr_sas_vp;       // u48-51: sky_hdr_scale, sky_ambient_scale, vp_w, vp_h
    vec4 sundir_distmul;   // u52-55: sun_dir.xyz (world, SL Z-up), distance_multiplier (P2c)
    vec4 moondir_classic;  // u56-59: moon_dir.xyz (world), classic_mode (w) -- the HDR-regime seam
} sky;
layout(set = 0, binding = 1) uniform texture2D g_albedo;  // RT0 albedo + flag.a
layout(set = 0, binding = 2) uniform texture2D g_normal;  // RT1 world normal.xyz + env.w
layout(set = 0, binding = 3) uniform texture2D g_spec;    // RT2 legacy: spec color.rgb + exponent.a / PBR: ORM
layout(set = 0, binding = 4) uniform texture2D g_emissive; // RT3 PBR: emissive.rgb (linear, additive) / legacy: fullbright.a

// --- PROBE stratum (P1): reflection-probe IBL. Faithful port of class3/deferred/reflectionProbeF.glsl.
// Stock's ENTIRE probe subsystem (this shader + the C++ updateUniforms packer) is VIEW-space: refSphere
// centers, refBox transforms, and the -pos.z depth bucket are all camera-space; env_mat rotates the
// view-space reflection vector back to WORLD for the (world-oriented) cubemap lookup. So the faithful
// port is view-space too -- and it makes the P3 manager port a direct line-by-line reproduction of
// updateUniforms. The rest of this resolve stays world-space (atmospherics); the probe block below is a
// self-contained view-space island: it reconstructs a view-space position from depth (getPositionWithDepth
// form) and a view-space normal (transpose(env_mat)*world_norm). In the verification fixture view=identity
// so env_mat=identity and inv_proj=inv_view_proj -> the block coincides with world space and the oracle.
layout(set = 0, binding = 5) uniform texture2D g_depth;   // depth buffer (for view-space position reconstruction)
layout(set = 0, binding = 7) uniform textureCubeArray reflectionProbes_tex; // radiance (glossy) probe array
layout(set = 0, binding = 8) uniform textureCubeArray irradianceProbes_tex; // irradiance (diffuse) probe array
layout(set = 0, binding = 9) uniform sampler probe_smp;                      // filtering sampler for both
layout(set = 0, binding = 11) uniform texture2D brdfLut_tex;                 // split-sum env-BRDF LUT (PBR IBL)

layout(location = 0) out vec4 frag;

const float M_PI = 3.14159265;
const float GBUFFER_FLAG_HAS_ATMOS = 0.34;
const float GBUFFER_FLAG_HAS_PBR = 0.67;
const float SPEC_EXPONENT = 368.0; // RenderSpecularExponent (settings.xml default)

#define MAX_REFMAP_COUNT 256   // must match LL_MAX_REFLECTION_PROBE_COUNT
#define REF_SAMPLE_COUNT 32    // stock permutation value
#define REFMAP_LEVEL 3         // enable the full spatial walk in preProbeSample

// The ReflectionProbes std140 block -- BYTE-IDENTICAL to llreflectionmapmanager.h ReflectionProbeData /
// class3/deferred/reflectionProbeF.glsl. The C++ struct is memcpy'd straight into this layout.
layout(std140, set = 0, binding = 6) uniform ReflectionProbes {
    mat4  refBox[MAX_REFMAP_COUNT];     // camera->[-1,1] unit cube (box probes)
    mat4  heroBox;
    vec4  refSphere[MAX_REFMAP_COUNT];  // xyz view-space origin, w radius
    vec4  refParams[MAX_REFMAP_COUNT];  // x irradiance-scale, y radiance-scale, z fadeIn, w znear
    vec4  heroSphere;
    ivec4 refIndex[MAX_REFMAP_COUNT];   // x cube layer, y neighborStart(ivec4 idx), z neighborCount(-1 none), w signed priority (neg=box)
    ivec4 refNeighbor[1024];            // 4096 ints
    ivec4 refBucket[256];               // depth->start-index LUT (only .x used)
    int   refmapCount;
    int   heroShape;
    int   heroMipCount;
    int   heroProbeCount;
};

// Probe-frame scalars stock carries as loose uniforms (env_mat, max_probe_lod, cube_snapshot) + inv_proj.
layout(std140, set = 0, binding = 10) uniform ProbeParams {
    mat4 pp_inv_proj;   // clip->view (fixture: = inv_view_proj since view=identity)
    mat4 pp_env_mat;    // view->world rotation (upper-left 3x3); fixture: identity
    vec4 pp_misc;       // x = max_probe_lod, y = cube_snapshot, z/w unused
} pp;

// stock globals reflectionProbeF relies on (file-scope mutable state), set at the top of main().
mat3 env_mat;
float max_probe_lod;
int cube_snapshot;
int classic_mode;
int probeIndex[REF_SAMPLE_COUNT];
int probeInfluences = 0;
bool sample_automatic = true;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// stock Blinn-Phong specular LUT (pipeline.cpp:1624 createLUTBuffers), the CLOSED FORM -- no texture:
// n = glossiness^2 * RenderSpecularExponent; lightFunc = pow(nh,n) * normalization curve.
float lightFunc(float nh, float glossiness) {
    float n = glossiness * glossiness * SPEC_EXPONENT;
    float s = pow(nh, n);
    s *= ((n + 2.0) * (n + 4.0)) / (8.0 * M_PI * (pow(2.0, -n * 0.5) + n));
    return s;
}

// stock srgbF.glsl srgb_to_linear / linear_to_srgb
vec3 srgb_to_linear(vec3 cs) {
    vec3 lo = cs / 12.92;
    vec3 hi = pow((cs + 0.055) / 1.055, vec3(2.4));
    return mix(lo, hi, vec3(greaterThan(cs, vec3(0.04045))));
}
vec3 linear_to_srgb(vec3 cl) {
    vec3 lo = cl * 12.92;
    vec3 hi = 1.055 * pow(cl, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, vec3(greaterThan(cl, vec3(0.0031308))));
}

// stock deferredUtil.glsl clampHDRRange: scrub nan/inf, clamp to [0, 11.2]
vec3 clampHDRRange(vec3 c) {
    c = mix(c, vec3(0.0), vec3(isnan(c)));
    c = mix(c, vec3(1.0), vec3(isinf(c)));
    return clamp(c, vec3(0.0), vec3(11.2));
}

// stock atmosphericsFuncs.glsl ambientLighting(): soft back-fill so shadowed sides keep detail
float ambientLighting(vec3 n, vec3 light_dir) {
    float a = min(abs(dot(n, light_dir)), 1.0);
    a *= 0.5;
    a *= a;
    return 1.0 - a;
}

// ============================================================================================
// Reflection-probe sampling -- verbatim faithful port of class3/deferred/reflectionProbeF.glsl.
// View-space throughout (pos/dir are camera-space; env_mat rotates to world for the cube lookup).
// Hero probes and SSR are stubbed for P1 (HERO_PROBES / SSR undefined in stock permutations here).
// ============================================================================================

// return true if probe at index i influences position pos
bool shouldSampleProbe(int i, vec3 pos) {
    if (refIndex[i].w < 0) {
        vec4 v = refBox[i] * vec4(pos, 1.0);
        if (abs(v.x) > 1 || abs(v.y) > 1 || abs(v.z) > 1) {
            return false;
        }
        // never allow automatic probes to encroach on box probes
        sample_automatic = false;
    } else {
        if (refIndex[i].w == 0 && !sample_automatic) {
            return false;
        }
        vec3 delta = pos.xyz - refSphere[i].xyz;
        float d = dot(delta, delta);
        float r2 = refSphere[i].w;
        r2 *= r2;
        if (d > r2) { // outside bounding sphere
            return false;
        }
    }
    return true;
}

int getStartIndex(vec3 pos) {
    int idx = clamp(int(floor(-pos.z)), 0, 255);
    return clamp(refBucket[idx].x, 1, refmapCount + 1);
}

// populate probeIndex with the probe indices that influence pos
void preProbeSample(vec3 pos) {
#if REFMAP_LEVEL > 0
    int start = getStartIndex(pos);
    for (int i = start; i < refmapCount; ++i) {
        if (shouldSampleProbe(i, pos)) {
            probeIndex[probeInfluences] = i;
            ++probeInfluences;

            int neighborIdx = refIndex[i].y;
            if (neighborIdx != -1) {
                int neighborCount = refIndex[i].z;
                int count = 0;
                while (count < neighborCount) {
                    int idx = refNeighbor[neighborIdx].x;
                    if (shouldSampleProbe(idx, pos)) {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT) { break; }
                    }
                    count++;
                    if (count == neighborCount) { break; }

                    idx = refNeighbor[neighborIdx].y;
                    if (shouldSampleProbe(idx, pos)) {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT) { break; }
                    }
                    count++;
                    if (count == neighborCount) { break; }

                    idx = refNeighbor[neighborIdx].z;
                    if (shouldSampleProbe(idx, pos)) {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT) { break; }
                    }
                    count++;
                    if (count == neighborCount) { break; }

                    idx = refNeighbor[neighborIdx].w;
                    if (shouldSampleProbe(idx, pos)) {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT) { break; }
                    }
                    count++;
                    ++neighborIdx;
                }
                break;
            }
        }
    }

    if (sample_automatic) { // probe 0 is the special probe for smoothing out automatic probes
        probeIndex[probeInfluences++] = 0;
    }
#else
    probeIndex[probeInfluences++] = 0;
#endif
}

// assume origin is inside sphere, return intersection of ray with edge of sphere
vec3 sphereIntersect(vec3 origin, vec3 dir, vec3 center, float radius2) {
    vec3 L = center - origin;
    float tca = dot(L, dir);
    float d2 = dot(L, L) - tca * tca;
    float thc = sqrt(radius2 - d2);
    float t1 = tca + thc;
    return origin + dir * t1;
}

// get point of intersection with given probe's box influence volume (Lagarde OBB parallax)
vec3 boxIntersect(vec3 origin, vec3 dir, mat4 i, out float d, float scale) {
    mat4 clipToLocal = i;
    vec3 RayLS = mat3(clipToLocal) * dir;
    vec3 PositionLS = (clipToLocal * vec4(origin, 1.0)).xyz;

    d = 1.0 - max(max(abs(PositionLS.x), abs(PositionLS.y)), abs(PositionLS.z));

    vec3 Unitary = vec3(scale);
    vec3 FirstPlaneIntersect  = (Unitary - PositionLS) / RayLS;
    vec3 SecondPlaneIntersect = (-Unitary - PositionLS) / RayLS;
    vec3 FurthestPlane = max(FirstPlaneIntersect, SecondPlaneIntersect);
    float Distance = min(FurthestPlane.x, min(FurthestPlane.y, FurthestPlane.z));

    return origin + dir * Distance;
}

vec3 boxIntersect(vec3 origin, vec3 dir, mat4 i, out float d) {
    return boxIntersect(origin, dir, i, d, 1.0);
}

// weight of a sphere probe
float sphereWeight(vec3 pos, vec3 dir, vec3 origin, float r, vec4 i, out float dw) {
    float r1 = r * 0.5; // 50% of radius (outer sphere to start interpolating down)
    vec3 delta = pos.xyz - origin;
    float d2 = max(length(delta), 0.001);

    float atten = 1.0 - max(d2 - r1, 0.0) / max((r - r1), 0.001);
    float w = 1.0 / d2;
    w *= i.z;
    dw = w * atten * max(r, 1.0) * 4;
    w *= atten;
    return w;
}

// Tap a reflection (radiance) probe
vec3 tapRefMap(vec3 pos, vec3 dir, out float w, out float dw, float lod, vec3 c, int i) {
    vec3 v;
    if (refIndex[i].w < 0) { // box probe
        float d = 0;
        v = boxIntersect(pos, dir, refBox[i], d);
        w = max(d, 0.001);
    } else { // sphere probe
        float r = refSphere[i].w;
        float rr = r * r;
        v = sphereIntersect(pos, dir, c,
            refIndex[i].w < 1 ? 4096.0 * 4096.0 : // disable parallax for automatic probes
            rr);
        w = sphereWeight(pos, dir, refSphere[i].xyz, r, refParams[i], dw);
    }

    v -= c;
    v = env_mat * v;

    vec4 ret = textureLod(samplerCubeArray(reflectionProbes_tex, probe_smp), vec4(v.xyz, refIndex[i].x), lod) * refParams[i].y;
    return ret.rgb;
}

// Tap an irradiance probe
vec3 tapIrradianceMap(vec3 pos, vec3 dir, out float w, out float dw, vec3 c, int i, vec3 amblit) {
    vec3 v;
    if (refIndex[i].w < 0) {
        float d = 0.0;
        v = boxIntersect(pos, dir, refBox[i], d, 3.0);
        w = max(d, 0.001);
    } else {
        float r = refSphere[i].w;
        float rr = r * r;
        v = sphereIntersect(pos, dir, c,
            refIndex[i].w < 1 ? 4096.0 * 4096.0 :
            rr);
        w = sphereWeight(pos, dir, refSphere[i].xyz, r, refParams[i], dw);
    }

    v -= c;
    v = env_mat * v;

    vec3 col = textureLod(samplerCubeArray(irradianceProbes_tex, probe_smp), vec4(v.xyz, refIndex[i].x), 0).rgb * refParams[i].x;
    col = mix(amblit, col, min(refParams[i].x, 1.0));
    return col;
}

vec3 sampleProbes(vec3 pos, vec3 dir, float lod) {
    float wsum[2]; wsum[0] = 0; wsum[1] = 0;
    float dwsum[2]; dwsum[0] = 0; dwsum[1] = 0;
    vec3 col[2]; col[0] = vec3(0); col[1] = vec3(0);

    for (int idx = 0; idx < probeInfluences; ++idx) {
        int i = probeIndex[idx];
        int p = clamp(abs(refIndex[i].w), 0, 1);
        if (p == 0 && !sample_automatic) { continue; }

        float w = 0; float dw = 0;
        vec3 refcol = tapRefMap(pos, dir, w, dw, lod, refSphere[i].xyz, i);
        col[p] += refcol.rgb * w;
        wsum[p] += w;
        dwsum[p] += dw;
    }

    // mix automatic and manual probes
    if (sample_automatic && wsum[0] > 0.0) {
        col[0] *= 1.0 / wsum[0];
        if (wsum[1] > 0.0) {
            col[1] *= 1.0 / wsum[1];
            col[1] = mix(col[0], col[1], min(dwsum[1], 1.0));
            col[0] = vec3(0);
        }
    } else if (wsum[1] > 0.0) {
        col[1] *= 1.0 / wsum[1];
        col[0] = vec3(0);
    }

    return col[1] + col[0];
}

vec3 sampleProbeAmbient(vec3 pos, vec3 dir, vec3 amblit) {
    float wsum[2]; wsum[0] = 0; wsum[1] = 0;
    float dwsum[2]; dwsum[0] = 0; dwsum[1] = 0;
    vec3 col[2]; col[0] = vec3(0); col[1] = vec3(0);

    for (int idx = 0; idx < probeInfluences; ++idx) {
        int i = probeIndex[idx];
        int p = clamp(abs(refIndex[i].w), 0, 1);
        if (p == 0 && !sample_automatic) { continue; }

        float w = 0; float dw = 0;
        vec3 refcol = tapIrradianceMap(pos, dir, w, dw, refSphere[i].xyz, i, amblit);
        col[p] += refcol * w;
        wsum[p] += w;
        dwsum[p] += dw;
    }

    if (sample_automatic && wsum[0] > 0.0) {
        col[0] *= 1.0 / wsum[0];
        if (wsum[1] > 0.0) {
            col[1] *= 1.0 / wsum[1];
            col[1] = mix(col[0], col[1], min(dwsum[1], 1.0));
            col[0] = vec3(0);
        }
    } else if (wsum[1] > 0.0) {
        col[1] *= 1.0 / wsum[1];
        col[0] = vec3(0);
    }

    return col[1] + col[0];
}

// hero (mirror) probes -- stubbed for P1 (HERO_PROBES undefined)
void tapHeroProbe(inout vec3 glossenv, vec3 pos, vec3 norm, float glossiness) {}

// PBR entry: fills ambenv (irradiance) + glossenv (radiance)
void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit) {
    preProbeSample(pos);

    float reflection_lods = max_probe_lod;
    vec3 refnormpersp = reflect(pos.xyz, norm.xyz);

    ambenv = amblit;
    if (classic_mode == 0) {
        ambenv = sampleProbeAmbient(pos, norm, amblit);
    }

    float lod = (1.0 - glossiness) * reflection_lods;
    glossenv = sampleProbes(pos, normalize(refnormpersp), lod);

    tapHeroProbe(glossenv, pos, norm, glossiness);
}

// legacy entry: fills ambenv (irradiance) + glossenv (radiance) + legacyenv (env-intensity reflection)
void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit) {
    float reflection_lods = max_probe_lod;
    preProbeSample(pos);

    vec3 refnormpersp = reflect(pos.xyz, norm.xyz);

    ambenv = amblit;
    if (classic_mode == 0) {
        ambenv = sampleProbeAmbient(pos, norm, amblit);
    }

    if (glossiness > 0.0) {
        float lod = (1.0 - glossiness) * reflection_lods;
        glossenv = sampleProbes(pos, normalize(refnormpersp), lod);
    }

    if (envIntensity > 0.0) {
        legacyenv = sampleProbes(pos, normalize(refnormpersp), 0.0);
    }

    tapHeroProbe(glossenv, pos, norm, glossiness);
    tapHeroProbe(legacyenv, pos, norm, 1.0);

    glossenv = clamp(glossenv, vec3(0), vec3(10));
}

void applyGlossEnv(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm) {
    glossenv *= 0.5; // fudge darker
    float fresnel = clamp(1.0 + dot(normalize(pos.xyz), norm.xyz), 0.3, 1.0);
    fresnel *= fresnel;
    fresnel *= spec.a;
    glossenv *= spec.rgb * fresnel;
    glossenv *= vec3(1.0) - color; // fake energy conservation
    color.rgb += glossenv * 0.5;
}

void applyLegacyEnv(inout vec3 color, vec3 legacyenv, vec4 spec, vec3 pos, vec3 norm, float envIntensity) {
    vec3 reflected_color = legacyenv;
    vec3 lookAt = normalize(pos);
    float fresnel = 1.0 + dot(lookAt, norm.xyz);
    fresnel *= fresnel;
    fresnel = min(fresnel + envIntensity, 1.0);
    reflected_color *= (envIntensity * fresnel);
    color = mix(color.rgb, reflected_color * 0.5, envIntensity);
}

// --- PBR IBL (deferredUtil.glsl pbrIbl): split-sum env BRDF + multiscatter energy compensation. ---
vec2 BRDF(float NoV, float roughness) {
    return textureLod(sampler2D(brdfLut_tex, probe_smp), vec2(NoV, roughness), 0.0).rg;
}

void pbrIbl(vec3 diffuseColor, vec3 specularColor, vec3 radiance, vec3 irradiance, float ao, float nv,
            float perceptualRough, out vec3 diffuseOut, out vec3 specularOut) {
    vec2 brdf = BRDF(clamp(nv, 0.0, 1.0), 1.0 - perceptualRough);
    vec3 diffuseLight = irradiance;
    vec3 specularLight = radiance;

    vec3 diffuse = diffuseLight * diffuseColor;

    // Single-scatter specular (split-sum: F0*A + B).
    vec3 FssEss = specularColor * brdf.x + brdf.y;
    // Multiscatter energy compensation (Fdez-Aguera 2019) -- MULTISCATTER_GGX is default-on in stock.
    float Ess = brdf.x + brdf.y;
    float Ems = 1.0 - Ess;
    vec3 Favg = specularColor + (1.0 - specularColor) / 21.0;
    vec3 Fms = FssEss * Favg / (1.0 - Ems * Favg);
    vec3 specular = specularLight * (FssEss + Fms * Ems);

    diffuseOut = diffuse * ao;
    specularOut = specular * ao;
}

// getPositionWithDepth (deferredUtil.glsl): reconstruct VIEW-space position from a depth sample.
// Takes the SAME flipped ndc.xy the view ray uses (so pos and ray agree), + OGL clip z (2*depth-1) via
// the fixture's OGL-form inv_proj -- matches the oracle's getPositionWithDepth exactly.
// (Engine integration P3 feeds a reverse-Z-appropriate inv_proj + ndc mapping; the decal gate verifies.)
vec3 getViewPositionFromDepth(vec2 ndc_xy, float depth) {
    vec4 ndc = vec4(ndc_xy, 2.0 * depth - 1.0, 1.0);
    vec4 vpos = pp.pp_inv_proj * ndc;
    return vpos.xyz / vpos.w;
}

void main() {
    env_mat = mat3(pp.pp_env_mat);
    max_probe_lod = pp.pp_misc.x;
    cube_snapshot = int(pp.pp_misc.y);

    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 a = texelFetch(g_albedo, p, 0);
    bool is_atmos = abs(a.a - GBUFFER_FLAG_HAS_ATMOS) < 0.1;
    bool is_pbr   = abs(a.a - GBUFFER_FLAG_HAS_PBR) < 0.1;
    if (!is_atmos && !is_pbr) discard;

    classic_mode = int(sky.moondir_classic.w + 0.5);
    vec3 n = normalize(texelFetch(g_normal, p, 0).xyz);      // world-space
    float envIntensity = texelFetch(g_normal, p, 0).w;        // RT1.w = legacy env-intensity
    vec3 sun_dir = normalize(sky.sundir_distmul.xyz);         // world, SL Z-up

    // View vector (surface->camera), stock softenLightF's v = -normalize(eye_pos). The DIRECTION is
    // depth-INDEPENDENT (depth only scales eye_pos, cancels in normalize) -> reconstruct the pixel's
    // view ray from inv_view_proj alone, dodging the reverse-Z depth-reconstruction hazard entirely.
    vec2 uv  = gl_FragCoord.xy / sky.hdr_sas_vp.zw;          // vp_w, vp_h (0 at top, y-down)
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;                                          // framebuffer y-down -> the fixture's GL inv_proj y-up
    vec4 far = sky.inv_view_proj * vec4(ndc, 1.0, 1.0);      // far plane (clip z far); ray direction only
    vec3 world_far = far.xyz / far.w;
    vec3 v = normalize(sky.cam_maxy.xyz - world_far);        // = -ray_dir = surface->camera

    // Probe stratum: a view-space position + view-space normal for the (view-space) probe block. Uses the
    // SAME flipped ndc.xy as the ray + the depth sample. env_mat is view->world, so transpose(env_mat)
    // maps our world normal (RT1) to view space. In the fixture view=identity so both are pass-throughs.
    float depth = texelFetch(g_depth, p, 0).r;
    vec3 view_pos = getViewPositionFromDepth(ndc, depth);
    vec3 view_norm = transpose(env_mat) * n;

    // --- calcAtmosphericVars/calcAtmosphericVarsLinear: sunlit + amblit (rel_pos-independent parts) ---
    vec3  blue_density = sky.bluedens_clouds.rgb;
    float haze_density = sky.ambient_hazed.w;
    float density_mult = sky.suncol_densmul.w;
    float max_y        = sky.cam_maxy.w;
    float above_horizon = 1.0 / max(1e-6, sky.lightnorm_sunup.y);

    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_mult * max_y);
    vec3 sunlight    = sky.suncol_densmul.rgb * exp(-light_atten * above_horizon); // attenuated sun color (sRGB)

    vec3 ambient   = sky.ambient_hazed.rgb;
    float cloud_sh = sky.bluedens_clouds.w;
    vec3 tmpAmbient = ambient + (vec3(1.0) - ambient) * cloud_sh * 0.5;
    vec3 amblit_srgb = pow(tmpAmbient, vec3(0.9)) * 0.57;
    amblit_srgb *= ambientLighting(n, sun_dir);              // per-fragment back-fill (sRGB)

    float ssc = sky.glow_ssc.w;      // sky_sunlight_scale
    float sas = sky.hdr_sas_vp.y;    // sky_ambient_scale

    // calcAtmosphericVarsLinear regime split, then softenLightF main's `if (classic_mode>0) sunlit*=1.35`.
    vec3 sunlit;
    vec3 amblit;
    if (classic_mode > 0) {
        sunlit = sunlight * ssc * 1.35;         // classic keeps sunlit in sRGB, +the ×1.35 sun boost
        amblit = amblit_srgb * sas;             // classic keeps ambient in sRGB, full RGB
    } else {
        sunlit = srgb_to_linear(sunlight) * ssc;               // linear
        amblit = vec3(luma(srgb_to_linear(amblit_srgb))) * sas; // linear, reduced to greyscale luminance
    }

    float nl   = clamp(dot(n, sun_dir), 0.0, 1.0);
    float scol = 1.0;                                        // no shadows yet (P4)
    vec3 color;

    if (is_pbr) {
        // ORM from RT2 (occlusion, roughness, metallic). base is LINEAR (PBR fill).
        vec3 orm = texelFetch(g_spec, p, 0).rgb;
        float ao = orm.r;
        float perceptualRoughness = orm.g;
        float metallic = orm.b;
        vec3 base = a.rgb;
        // calcDiffuseSpecular (deferredUtil): diffuseColor = base*(1-f0)*(1-metallic); specColor = mix(f0, base, metallic).
        vec3 diffuseColor = base * (1.0 - 0.04) * (1.0 - metallic);
        vec3 specularColor = mix(vec3(0.04), base, metallic);

        // PBR IBL: probe ambient (ambenv) + gloss radiance (glossenv). gloss uses the UNCLAMPED roughness
        // (softenLightF:177). radiance feeds iblSpec; irradiance feeds iblDiff (pbrIbl split-sum).
        float gloss_ibl = 1.0 - perceptualRoughness;
        vec3 irradiance = amblit;
        vec3 radiance = vec3(0);
        sampleReflectionProbes(irradiance, radiance, uv, view_pos, view_norm, gloss_ibl, false, amblit);

        float nv_ibl = clamp(abs(dot(n, v)), 0.001, 1.0);
        vec3 iblDiff; vec3 iblSpec;
        pbrIbl(diffuseColor, specularColor, radiance, irradiance, ao, nv_ibl, perceptualRoughness, iblDiff, iblSpec);

        // pbrPunctual GGX (deferredUtil): F (Schlick) * G (Smith) * D (GGX). Uses the reconstructed view vector v.
        perceptualRoughness = max(perceptualRoughness, 8.0 / 255.0);
        float alphaRoughness = perceptualRoughness * perceptualRoughness;
        float ar2 = alphaRoughness * alphaRoughness;
        float reflectance90 = clamp(max(max(specularColor.r, specularColor.g), specularColor.b) * 25.0, 0.0, 1.0);
        vec3 l = normalize(sun_dir);
        vec3 h = normalize(l + v);
        float NdotL = clamp(dot(n, l), 0.001, 1.0);
        float NdotV = clamp(abs(dot(n, v)), 0.001, 1.0);
        float NdotH = clamp(dot(n, h), 0.0, 1.0);
        float VdotH = clamp(dot(v, h), 0.0, 1.0);
        vec3 F = specularColor + (vec3(reflectance90) - specularColor) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
        float aL = 2.0 * NdotL / (NdotL + sqrt(ar2 + (1.0 - ar2) * (NdotL * NdotL)));
        float aV = 2.0 * NdotV / (NdotV + sqrt(ar2 + (1.0 - ar2) * (NdotV * NdotV)));
        float G = aL * aV;
        float fd = (NdotH * ar2 - NdotH) * NdotH + 1.0;
        float D = ar2 / (M_PI * fd * fd);
        vec3 diffPunc = (vec3(1.0) - F) * diffuseColor / M_PI;
        vec3 specPunc = F * G * D / (4.0 * NdotL * NdotV);
        float nl_p = NdotL;

        // pbrBaseLight: color += iblDiff (the split-sum diffuse IBL).
        color = iblDiff;

        // pbrBaseLight combination (classic deconstruction vs linear).
        if (classic_mode > 0) {
            irradiance = srgb_to_linear(irradiance * 0.9);
            float da = pow(nl_p, 1.2);
            vec3 sun_contrib = vec3(min(da, scol));
            sun_contrib = srgb_to_linear(linear_to_srgb(sun_contrib) * sunlit * 0.7) * M_PI;
            vec3 finalAmbient = irradiance * diffuseColor;
            vec3 finalSun = clamp(sun_contrib * ((diffPunc + specPunc) * scol), vec3(0.0), vec3(10.0));
            color = srgb_to_linear(linear_to_srgb(finalAmbient) + linear_to_srgb(finalSun) * 1.1);
        } else {
            color += clamp(nl_p * (diffPunc + specPunc), vec3(0.0), vec3(10.0)) * sunlit * 3.0 * scol;
        }
        // pbrBaseLight tail: iblSpec (radiance split-sum specular) then additive linear emissive (RT3.rgb).
        color += iblSpec;
        color += texelFetch(g_emissive, p, 0).rgb;
    } else {
        // legacy HAS_ATMOS branch (softenLightF else). Legacy writes sRGB to the gbuffer.
        vec3 baseColor = srgb_to_linear(a.rgb);
        vec4 spec = texelFetch(g_spec, p, 0);               // RT2: (spec color sRGB, exponent/glossiness)
        spec.rgb = srgb_to_linear(spec.rgb);                // softenLightF:211 -- spec.rgb linearized once
        float glossiness = spec.a;

        // PROBE IBL: reflection-probe ambient (ambenv), gloss reflection (glossenv), legacy env (legacyenv).
        // Replaces the old amblit-only stub -- closes the non-classic ambient residual + adds env reflection.
        vec3 irradiance = amblit;
        vec3 glossenv = vec3(0);
        vec3 legacyenv = vec3(0);
        sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, uv, view_pos, view_norm,
                                     glossiness, envIntensity, false, amblit);

        color = irradiance;                                 // lambertian IBL (now probe ambient)
        if (classic_mode > 0) {
            float da = pow(nl, 1.2);
            vec3 sun_contrib = vec3(min(da, scol));
            color = srgb_to_linear(color * 0.9 + (linear_to_srgb(sun_contrib) * sunlit * 0.7));
        } else {
            vec3 sun_contrib = min(nl, scol) * sunlit;
            color += sun_contrib;
        }
        color *= baseColor;

        // Direct Blinn-Phong specular (softenLightF legacy else, spec.a>0 path). spec.a = glossiness.
        // classic linearizes sunlit_linear before the specular (softenLightF:232); non-classic already linear.
        vec3 sunlit_spec = (classic_mode > 0) ? srgb_to_linear(sunlit) : sunlit;
        if (glossiness > 0.0) {
            vec3 l = normalize(sun_dir);
            vec3 h = normalize(l + v);
            float eps = 1e-6;
            float nh  = clamp(dot(n, h), eps, 1.0);
            float nl_s = clamp(dot(n, l), eps, 1.0);
            float nv  = clamp(dot(n, v), eps, 1.0);
            float vh  = clamp(dot(v, h), eps, 1.0);
            if (nl_s > 0.0 && nh > 0.0) {
                float lit = min(nl_s * 6.0, 1.0);
                float fres = pow(1.0 - vh, 5.0) * 0.4 + 0.5;
                float gtdenom = 2.0 * nh;
                float gt = max(0.0, min(gtdenom * nv / vh, gtdenom * nl_s / vh));
                float sc = scol * fres * lightFunc(nh, glossiness) * gt / (nh * nl_s);
                color += lit * sc * sunlit_spec * spec.rgb;
            }
            // Radiance (gloss env) reflection -- inside the spec.a>0 gate, like softenLightF:266.
            applyGlossEnv(color, glossenv, spec, view_pos, view_norm);
        }
        // Fullbright/emissive (softenLightF:270 mix(color, baseColor, baseColor.a)). OGL packs the
        // fullbright factor in RT0.a (diffuse alpha); our RT0.a is the FLAG, so it lives in RT3.a.
        color = mix(color, baseColor, texelFetch(g_emissive, p, 0).a);

        // Legacy environment-map reflection (softenLightF:272 envIntensity>0 path).
        if (envIntensity > 0.0) {
            applyLegacyEnv(color, legacyenv, spec, view_pos, view_norm, envIntensity);
        }
    }

    // softenLightF main tail: classic gets a ×1.1 final scale.
    float final_scale = (classic_mode > 0) ? 1.1 : 1.0;
    frag = vec4(clampHDRRange(color * final_scale), 1.0);
}
