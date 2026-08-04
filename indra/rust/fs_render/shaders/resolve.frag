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
// sun_dir fed, dots are frame-invariant -> identical result, and it dodges the reverse-Z inv_proj
// depth-reconstruction hazard. Consequences of no view vector yet (arrives with world-pos in P2c):
//   - punctual Fresnel F uses the normal-incidence value f0=0.04 (grazing rim = P2c),
//   - punctual GGX specular is omitted (rough matte ground; real ORM+specular = P2c),
//   - reflection-probe IBL is the sky-ambient stub (real probes = later).
// Haze (additive/atten, which DO need world pos) is the separate hazeF pass = P2c.
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
layout(location = 0) out vec4 frag;

const float M_PI = 3.14159265;
const float GBUFFER_FLAG_HAS_ATMOS = 0.34;
const float GBUFFER_FLAG_HAS_PBR = 0.67;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

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

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 a = texelFetch(g_albedo, p, 0);
    bool is_atmos = abs(a.a - GBUFFER_FLAG_HAS_ATMOS) < 0.1;
    bool is_pbr   = abs(a.a - GBUFFER_FLAG_HAS_PBR) < 0.1;
    if (!is_atmos && !is_pbr) discard;

    int classic_mode = int(sky.moondir_classic.w + 0.5);
    vec3 n = normalize(texelFetch(g_normal, p, 0).xyz);      // world-space
    vec3 sun_dir = normalize(sky.sundir_distmul.xyz);         // world, SL Z-up

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
        // calcDiffuseSpecular: diffuseColor = base*(1-f0)*(1-metallic); base is LINEAR (PBR fill).
        float metallic = 0.0;                               // default matte (no ORM target yet)
        float ao = 1.0;
        vec3 base = a.rgb;                                   // already linear
        vec3 diffuseColor = base * (1.0 - 0.04) * (1.0 - metallic);
        // pbrPunctual diffuse (deconstructed): diffPunc = (1-F) * diffuseColor / M_PI. Without a view
        // vector, F is the normal-incidence f0=0.04 (grazing rim is P2c). specPunc omitted (rough matte).
        vec3 diffPunc = (1.0 - 0.04) * diffuseColor / M_PI;
        // pbrIbl diffuse: irradiance * diffuseColor * ao. irradiance = sky ambient (probe stub).
        vec3 irradiance = amblit;

        if (classic_mode > 0) {
            // pbrBaseLight classic sub-branch (deconstructed to match blinn-phong under legacy skies).
            irradiance = srgb_to_linear(irradiance * 0.9);
            float da = pow(nl, 1.2);
            vec3 sun_contrib = vec3(min(da, scol));
            // ×M_PI cancels the Lambertian /pi so legacy skies aren't too dark.
            sun_contrib = srgb_to_linear(linear_to_srgb(sun_contrib) * sunlit * 0.7) * M_PI;
            vec3 finalAmbient = irradiance * diffuseColor;
            vec3 finalSun = clamp(sun_contrib * (diffPunc * scol), vec3(0.0), vec3(10.0));
            color = srgb_to_linear(linear_to_srgb(finalAmbient) + linear_to_srgb(finalSun) * 1.1);
        } else {
            // pbrBaseLight linear sub-branch: iblDiff + nl*diffPunc*sunlit*3.0*scol.
            vec3 iblDiff = irradiance * diffuseColor * ao;
            vec3 sunDiff = clamp(nl * diffPunc, vec3(0.0), vec3(10.0)) * sunlit * 3.0 * scol;
            color = iblDiff + sunDiff;
        }
    } else {
        // legacy HAS_ATMOS branch (softenLightF else). Legacy writes sRGB to the gbuffer.
        vec3 baseColor = srgb_to_linear(a.rgb);
        vec3 irradiance = amblit;
        color = irradiance;                                 // lambertian IBL only
        if (classic_mode > 0) {
            float da = pow(nl, 1.2);
            vec3 sun_contrib = vec3(min(da, scol));
            color = srgb_to_linear(color * 0.9 + (linear_to_srgb(sun_contrib) * sunlit * 0.7));
        } else {
            vec3 sun_contrib = min(nl, scol) * sunlit;
            color += sun_contrib;
        }
        color *= baseColor;
        // stock also does mix(color, baseColor, baseColor.a) for material alpha + the spec.a/envIntensity
        // gloss paths; this world-space G-buffer packs the FLAG in .a (no material-alpha channel) and has
        // no spec/env yet -> opaque, non-glossy. Both arrive with the real material G-buffer (later).
    }

    // softenLightF main tail: classic gets a ×1.1 final scale.
    float final_scale = (classic_mode > 0) ? 1.1 : 1.0;
    frag = vec4(clampHDRRange(color * final_scale), 1.0);
}
