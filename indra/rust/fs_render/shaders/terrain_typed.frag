#version 450
// Ground Phase 1 (forward): light the terrain and write linear HDR into scene_hdr (composited over
// the sky, before the cloud/exposure/tonemap passes). Flat placeholder albedo -- the 4-texture splat
// is Phase 3; proper deferred lighting (shadows, probes, atmosphere) is Phase 2. Scaled by hdr_scale
// so it sits in the same HDR range as the sky pass.
layout(location = 0) in vec3 v_normal;   // world-space, unnormalized
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj;
    vec4 sun_dir;    // world dir toward the sun
    vec4 sunlight;   // rgb linear
    vec4 ambient;    // rgb linear
    vec4 misc;       // .x = sky_hdr_scale
} u;
layout(location = 0) out vec4 frag;
void main() {
    vec3 n = normalize(v_normal);
    float ndl = max(dot(n, normalize(u.sun_dir.xyz)), 0.0);
    vec3 albedo = vec3(0.42, 0.44, 0.36);                 // placeholder (P3 = the splat)
    vec3 lit = u.ambient.rgb + ndl * u.sunlight.rgb;
    frag = vec4(albedo * lit * max(u.misc.x, 1.0), 1.0);
}
