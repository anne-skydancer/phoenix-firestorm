#version 450
// Ground Phase 1: fill the G-buffer for typed terrain. Flat placeholder albedo (the 4-texture splat
// is Phase 3) tagged HAS_ATMOS so the deferred resolve lights it (ambient + N.L*sunlight); the relief
// reads purely through that shading. RT layout matches live_gb.frag / resolve.frag.
layout(location = 0) in vec3 v_normal;   // eye-space, unnormalized
layout(location = 0) out vec4 g_albedo;  // rgb albedo, a = gbufferFlag
layout(location = 1) out vec4 g_normal;  // eye-space normal
const float GBUFFER_FLAG_HAS_ATMOS = 0.34;
void main() {
    vec3 albedo = vec3(0.42, 0.44, 0.36);   // earthy grey-green placeholder (P3 replaces with the splat)
    g_albedo = vec4(albedo, GBUFFER_FLAG_HAS_ATMOS);
    g_normal = vec4(normalize(v_normal), 1.0);
}
