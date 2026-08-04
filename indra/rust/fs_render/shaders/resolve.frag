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
layout(location = 0) out vec4 frag;

const float M_PI = 3.14159265;
const float GBUFFER_FLAG_HAS_ATMOS = 0.34;
const float GBUFFER_FLAG_HAS_PBR = 0.67;
const float SPEC_EXPONENT = 368.0; // RenderSpecularExponent (settings.xml default)

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

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 a = texelFetch(g_albedo, p, 0);
    bool is_atmos = abs(a.a - GBUFFER_FLAG_HAS_ATMOS) < 0.1;
    bool is_pbr   = abs(a.a - GBUFFER_FLAG_HAS_PBR) < 0.1;
    if (!is_atmos && !is_pbr) discard;

    int classic_mode = int(sky.moondir_classic.w + 0.5);
    vec3 n = normalize(texelFetch(g_normal, p, 0).xyz);      // world-space
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
        // pbrBaseLight tail: additive linear emissive (colorEmissive = RT3.rgb).
        color += texelFetch(g_emissive, p, 0).rgb;
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

        // Direct Blinn-Phong specular (softenLightF legacy else, spec.a>0 path). PROBE-INDEPENDENT
        // (applyGlossEnv/applyLegacyEnv reflections need probes -> probe stratum). spec.a = glossiness.
        vec4 spec = texelFetch(g_spec, p, 0);               // RT2: (spec color sRGB, exponent/glossiness)
        float glossiness = spec.a;
        vec3 spec_lin = srgb_to_linear(spec.rgb);
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
                color += lit * sc * sunlit_spec * spec_lin;
            }
        }
        // Fullbright/emissive (softenLightF:270 mix(color, baseColor, baseColor.a)). OGL packs the
        // fullbright factor in RT0.a (diffuse alpha); our RT0.a is the FLAG, so it lives in RT3.a.
        color = mix(color, baseColor, texelFetch(g_emissive, p, 0).a);
        // Still deferred: material-alpha blend (our .a = flag) + env-intensity reflection (probe stratum).
    }

    // softenLightF main tail: classic gets a ×1.1 final scale.
    float final_scale = (classic_mode > 0) ? 1.1 : 1.0;
    frag = vec4(clampHDRRange(color * final_scale), 1.0);
}
