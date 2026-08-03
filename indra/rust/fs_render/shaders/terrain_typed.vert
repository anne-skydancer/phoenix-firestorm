#version 450
// Ground Phase 1 (forward): typed terrain, mesh built engine-side from the region heightmap. Rendered
// straight into scene_hdr after the sky (depth-tested), lit in-shader with ambient + N.L*sunlight so
// the relief reads. Phase 2 moves this to the deferred G-buffer + resolve. Vertices/normals are
// world/agent-space (SL Z-up); lighting is done in world space so no view transform of the normal.
layout(location = 0) in vec3 pos;      // world/agent-space terrain vertex
layout(location = 1) in vec3 normal;   // world/agent-space normal
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj;   // proj * view : world -> clip
    vec4 sun_dir;     // world-space direction TOWARD the sun (xyz), _
    vec4 sunlight;    // rgb linear sunlight
    vec4 ambient;     // rgb linear ambient
    vec4 misc;        // .x = sky_hdr_scale (match the sky's HDR scale), _
} u;
layout(location = 0) out vec3 v_normal; // world-space (normalized in the fragment)
void main() {
    v_normal = normal;
    gl_Position = u.view_proj * vec4(pos, 1.0);
}
