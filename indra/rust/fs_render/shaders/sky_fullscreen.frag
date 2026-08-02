#version 450
// Phase A.1: fullscreen procedural WL sky, driven ENTIRELY by the typed camera + EepSkyBlock
// (parallel-read, tap-independent). Renders with post.vert (fullscreen triangle). Per-fragment
// reproduction of the skyV.glsl dome integral (verbatim from sky.vert), with the dome vertex
// position replaced by a point along the reconstructed world view-ray. Then the skyF/SKIP_ATMOS
// chain (min(haze*2,5) -> srgb_to_linear -> * sky_hdr_scale) -> LINEAR HDR; the S1 global
// tonemap bounds it, exactly as S2's sky.frag did.
//
// This is the first "parallel reader drives the engine" proof: the sky is now a pure function
// of (camera, EepSkyBlock), both already in the typed SceneFrame -- no CLASS_SKY_DOME draw.
layout(set = 0, binding = 0) uniform U {
    mat4 inv_view_proj;   // inverse(proj * view): clip -> world (ray reconstruction)
    vec4 a0;  // camPosLocal.xyz, max_y
    vec4 a1;  // lightnorm.xyz, sun_up_factor
    vec4 a2;  // sunlight_color.xyz, density_multiplier
    vec4 a3;  // moonlight_color.xyz, sun_moon_glow_factor
    vec4 a4;  // ambient_color.xyz, haze_density
    vec4 a5;  // blue_horizon.xyz, haze_horizon
    vec4 a6;  // blue_density.xyz, cloud_shadow
    vec4 a7;  // glow.xyz, _
    vec4 a8;  // sky_hdr_scale, _, viewport_w, viewport_h
} u;
layout(location = 0) out vec4 frag;

vec3 srgb_to_linear(vec3 c) {
    bvec3 lo = lessThanEqual(c, vec3(0.04045));
    vec3 a = c / 12.92;
    vec3 b = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(b, a, vec3(lo));
}

void main() {
    vec3  camPosLocal        = u.a0.xyz;
    float max_y              = u.a0.w;
    vec3  lightnorm          = u.a1.xyz;
    float sun_up_factor      = u.a1.w;
    vec3  sunlight_color     = u.a2.xyz;
    float density_multiplier = u.a2.w;
    vec3  moonlight_color    = u.a3.xyz;
    float sun_moon_glow_factor = u.a3.w;
    vec3  ambient_color      = u.a4.xyz;
    float haze_density_f     = u.a4.w;
    vec3  blue_horizon       = u.a5.xyz;
    float haze_horizon       = u.a5.w;
    vec3  blue_density       = u.a6.xyz;
    float cloud_shadow       = u.a6.w;
    vec3  glow               = u.a7.xyz;
    float sky_hdr_scale      = u.a8.x > 0.0 ? u.a8.x : 1.0;
    vec2  viewport           = u.a8.zw;

    // --- reconstruct the world view-ray through this pixel ---
    // NDC (wgpu: y-down framebuffer -> y-up NDC). Any valid clip-z gives the same ray direction
    // for a perspective camera, so pick mid-range and take the direction from the eye.
    vec2 ndc = vec2((gl_FragCoord.x + 0.5) / viewport.x * 2.0 - 1.0,
                    1.0 - (gl_FragCoord.y + 0.5) / viewport.y * 2.0);
    vec4 world_h = u.inv_view_proj * vec4(ndc, 0.5, 1.0);
    vec3 world_pt = world_h.xyz / world_h.w;
    vec3 dir = normalize(world_pt - camPosLocal);
    // A far point along the ray so the +50 dome offset is negligible and the altitude clamp
    // (below) reduces to the pure ray direction, matching the dome's color-vs-direction.
    vec3 world_pos = camPosLocal + dir * 1.0e6;

    // ==== skyV.glsl integral, verbatim (sky.vert:43-80), pos -> world_pos ====
    vec3 rel_pos = world_pos - camPosLocal + vec3(0.0, 50.0, 0.0);
    if (rel_pos.y > 0.0) { rel_pos *= (max_y / rel_pos.y); }
    if (rel_pos.y < 0.0) { rel_pos *= (-32000.0 / rel_pos.y); }

    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);
    float rel_dot      = dot(rel_pos_norm, lightnorm);

    vec3 sunlight = (sun_up_factor == 1.0) ? sunlight_color : moonlight_color * 0.7;

    vec3 light_atten   = (blue_density + vec3(haze_density_f * 0.25)) * (density_multiplier * max_y);
    vec3 combined_haze = max(abs(blue_density) + vec3(abs(haze_density_f)), vec3(1e-6));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = vec3(haze_density_f) / combined_haze;

    float off_axis = 1.0 / max(1e-6, max(0.0, rel_pos_norm.y) + lightnorm.y);
    sunlight *= exp(-light_atten * off_axis);

    float density_dist = rel_pos_len * density_multiplier;
    combined_haze = exp(-combined_haze * density_dist);

    float haze_glow = max(1.0 - rel_dot, 0.001);
    haze_glow *= glow.x;
    haze_glow = pow(haze_glow, glow.z);
    haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (sun_moon_glow_factor * (haze_glow + 0.25));

    vec3 color = (blue_horizon * blue_weight * (sunlight + ambient_color)
               + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient_color));
    color *= (1.0 - combined_haze);

    vec3 ambient = ambient_color + max(vec3(0.0), (1.0 - ambient_color)) * cloud_shadow * 0.5;
    sunlight *= max(0.0, (1.0 - cloud_shadow));
    vec3 add_below_cloud = (blue_horizon * blue_weight * (sunlight + ambient)
                         + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient));
    combined_haze = sqrt(combined_haze);
    color += (add_below_cloud - color) * (1.0 - sqrt(combined_haze));
    // ==== end skyV ====

    // skyF.glsl + softenLightF SKIP_ATMOS branch (sky.frag): haze*2 clamp[0,5] -> linearize
    // -> * sky_hdr_scale. LINEAR HDR out; the S1 global tonemap bounds it.
    vec3 c = min(color * 2.0, vec3(5.0));
    c = srgb_to_linear(c);
    c *= sky_hdr_scale;
    frag = vec4(c, 1.0);
}
