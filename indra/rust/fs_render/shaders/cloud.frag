#version 450
// HALF-RESOLUTION volumetric cloud pass. Raymarches the 3D fbm density slab (moved out of
// sky_fullscreen.frag) into a half-size RGBA16F target: rgb = cloud radiance already in scene_hdr
// space, a = coverage. A later composite pass bilinearly upscales this and alpha-blends it over the
// sky in scene_hdr. Half-res quarters the raymarch cost, and the bilinear upscale dissolves the
// per-pixel jitter grain (worst at the horizon, where the ray path is longest). Same U block as
// sky_fullscreen.frag so it shares the sky UBO bind group.
layout(set = 0, binding = 0) uniform U {
    mat4 inv_view_proj;
    vec4 a0;  // camPosLocal.xyz, max_y
    vec4 a1;  // lightnorm.xyz, sun_up_factor
    vec4 a2;  // sunlight_color.xyz, density_multiplier
    vec4 a3;  // moonlight_color.xyz, sun_moon_glow_factor
    vec4 a4;  // ambient_color.xyz, haze_density
    vec4 a5;  // blue_horizon.xyz, haze_horizon
    vec4 a6;  // blue_density.xyz, cloud_shadow
    vec4 a7;  // glow.xyz, cloud_time
    vec4 a8;  // sky_hdr_scale, _, viewport_w, viewport_h (FULL-res; this pass = half)
    vec4 a9;  // sun_dir.xyz, _
    vec4 a10; // moon_dir.xyz, _
} u;
layout(location = 0) out vec4 frag;

vec3 srgb_to_linear(vec3 c) {
    bvec3 lo = lessThanEqual(c, vec3(0.04045));
    vec3 a = c / 12.92;
    vec3 b = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(b, a, vec3(lo));
}

// ---- procedural 3D noise (fbm) ----
float hash13(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float vnoise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i + vec3(0.0, 0.0, 0.0));
    float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash13(i + vec3(1.0, 1.0, 1.0));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}
float fbm3(vec3 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; i++) { v += a * vnoise3(p); p = p * 2.03 + vec3(1.3, 7.1, 2.9); a *= 0.5; }
    return v;
}
const float CLOUD_BOT = 250.0;
const float CLOUD_TOP = 700.0;
float cloudDensity(vec3 p, float ct) {
    float h = clamp((p.z - CLOUD_BOT) / (CLOUD_TOP - CLOUD_BOT), 0.0, 1.0);
    float hgrad = smoothstep(0.0, 0.25, h) * smoothstep(1.0, 0.55, h);
    vec3 wp = p * 0.0026 + vec3(ct * 0.020, ct * 0.010, 0.0);
    float shape = fbm3(wp);
    float d = (shape - 0.56) * hgrad;
    d -= 0.09 * fbm3(wp * 3.0 + 5.0);
    return clamp(d * 1.9, 0.0, 1.0);
}

void main() {
    vec3  camPosLocal    = u.a0.xyz;
    vec3  sunlight_color = u.a2.xyz;
    vec3  moonlight_color = u.a3.xyz;
    vec3  ambient_color  = u.a4.xyz;
    float sun_up_factor  = u.a1.w;
    float sky_hdr_scale  = u.a8.x > 0.0 ? u.a8.x : 1.0;
    vec3  sun_dir_w      = u.a9.xyz;
    vec3  moon_dir_w     = u.a10.xyz;
    float cloud_time     = u.a7.w;

    // Ray reconstruction. This pass renders to a half-size target, so its extent = full viewport * 0.5.
    vec2 half_res = u.a8.zw * 0.5;
    vec2 ndc = vec2((gl_FragCoord.x + 0.5) / half_res.x * 2.0 - 1.0,
                    1.0 - (gl_FragCoord.y + 0.5) / half_res.y * 2.0);
    vec4 world_h = u.inv_view_proj * vec4(ndc, 0.5, 1.0);
    vec3 world_pt = world_h.xyz / world_h.w;
    vec3 dir = normalize(world_pt - camPosLocal);

    frag = vec4(0.0);
    if (dir.z <= 0.02) return;                          // below the slab / at the horizon
    float t0 = (CLOUD_BOT - camPosLocal.z) / dir.z;     // ray enters the slab
    float t1 = (CLOUD_TOP - camPosLocal.z) / dir.z;     // ray exits the slab
    if (t1 <= 0.0) return;
    t0 = max(t0, 0.0);
    // Cap the march length: at grazing (horizon) angles the slab crossing is kilometres, which makes
    // dt huge -> coarse jitter grain. distfade fades those far clouds anyway, so bound it.
    t1 = min(t1, t0 + 3000.0);

    // Half-res affords a much finer march -> low jitter variance between neighbouring rays, so the
    // bilinear upscale resolves to smooth clouds instead of coarse speckle.
    const int STEPS = 48;
    float dt = (t1 - t0) / float(STEPS);
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float trans = 1.0;
    vec3  scatter = vec3(0.0);
    vec3  ldir = normalize((sun_up_factor == 1.0) ? sun_dir_w : moon_dir_w);
    vec3  lcol = (sun_up_factor == 1.0) ? sunlight_color : moonlight_color * 0.35;
    for (int i = 0; i < STEPS; i++) {
        if (trans < 0.02) break;
        vec3 p = camPosLocal + dir * (t0 + dt * (float(i) + jitter));
        float dens = cloudDensity(p, cloud_time);
        if (dens > 0.003) {
            float lsum = cloudDensity(p + ldir * 40.0,  cloud_time)
                       + cloudDensity(p + ldir * 100.0, cloud_time)
                       + cloudDensity(p + ldir * 190.0, cloud_time);
            float lightT = exp(-lsum * 2.5);
            vec3  lum = lcol * lightT * 0.5 + ambient_color * 0.2;
            float dtrans = exp(-dens * dt * 0.05);
            scatter += trans * (1.0 - dtrans) * lum;
            trans   *= dtrans;
        }
    }
    float distfade = exp(-max(t0 - 1200.0, 0.0) * 0.00018);
    float alpha = (1.0 - trans) * smoothstep(0.02, 0.14, dir.z) * distfade;
    // Cloud radiance in scene_hdr space (same transform sky_fullscreen applies), so the composite
    // blends in the same space as the sky already written to scene_hdr.
    vec3 cloud_hdr = srgb_to_linear(min(scatter * 2.0, vec3(5.0))) * sky_hdr_scale;
    frag = vec4(cloud_hdr, alpha);
}
