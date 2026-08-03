#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Ground P2b: deferred resolve -- FAITHFUL port of stock softenLightF.glsl's legacy/atmos branch
// (class3/deferred/softenLightF.glsl:206-284) + calcAtmosphericVars (class1/windlight/
// atmosphericsFuncs.glsl:51-165) for sunlit/amblit. Lights HAS_ATMOS G-buffer pixels; DISCARDS the
// rest so the sky already in scene_hdr (drawn first, LoadOp::Load) shows through.
//
// Done in WORLD space (the deliberate divergence from stock's eye space): world normal from RT1,
// world sun_dir fed, dots are frame-invariant -> identical result, and it dodges the reverse-Z
// inv_proj depth-reconstruction hazard. Haze (additive/atten, which DO need world pos) = P2c.
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
    vec4 sundir_distmul;   // u52-55: sun_dir.xyz (world, SL Z-up), distance_multiplier
    vec4 extra;            // u56-59
} sky;
layout(set = 0, binding = 1) uniform texture2D g_albedo;  // RT0 albedo(sRGB) + flag.a
layout(set = 0, binding = 2) uniform texture2D g_normal;  // RT1 world normal.xyz + env.w
layout(location = 0) out vec4 frag;

const float GBUFFER_FLAG_HAS_ATMOS = 0.34;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// stock srgbF.glsl srgb_to_linear
vec3 srgb_to_linear(vec3 cs) {
    vec3 lo = cs / 12.92;
    vec3 hi = pow((cs + 0.055) / 1.055, vec3(2.4));
    return mix(lo, hi, vec3(greaterThan(cs, vec3(0.04045))));
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
    // Not lit terrain: keep the sky (scene_hdr LoadOp::Load already holds it).
    if (abs(a.a - GBUFFER_FLAG_HAS_ATMOS) > 0.1) discard;

    vec3 n = normalize(texelFetch(g_normal, p, 0).xyz);      // world-space
    vec3 sun_dir = normalize(sky.sundir_distmul.xyz);         // world, SL Z-up

    // --- calcAtmosphericVars (rel_pos-independent parts): sunlit + amblit ---
    vec3  blue_density = sky.bluedens_clouds.rgb;
    float haze_density = sky.ambient_hazed.w;
    float density_mult = sky.suncol_densmul.w;
    float max_y        = sky.cam_maxy.w;
    float above_horizon = 1.0 / max(1e-6, sky.lightnorm_sunup.y);

    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_mult * max_y);
    vec3 sunlight    = sky.suncol_densmul.rgb * exp(-light_atten * above_horizon);

    vec3 ambient   = sky.ambient_hazed.rgb;
    float cloud_sh = sky.bluedens_clouds.w;
    vec3 tmpAmbient = ambient + (vec3(1.0) - ambient) * cloud_sh * 0.5;
    vec3 amblit_srgb = pow(tmpAmbient, vec3(0.9)) * 0.57;
    amblit_srgb *= ambientLighting(n, sun_dir);              // per-fragment back-fill

    // Linear-space (non-classic): sunlit linearized, ambient reduced to GREYSCALE luminance.
    vec3 sunlit = srgb_to_linear(sunlight) * sky.glow_ssc.w;              // * sky_sunlight_scale
    vec3 amblit = vec3(luma(srgb_to_linear(amblit_srgb))) * sky.hdr_sas_vp.y; // * sky_ambient_scale

    // --- softenLight legacy/atmos combine ---
    vec3 albedo = srgb_to_linear(a.rgb);
    float da = clamp(dot(n, sun_dir), 0.0, 1.0);
    float scol = 1.0;                                        // no shadows yet (P4)
    vec3 color = amblit + min(da, scol) * sunlit;            // irradiance + sun_contrib
    color *= albedo;
    frag = vec4(clampHDRRange(color), 1.0);
}
