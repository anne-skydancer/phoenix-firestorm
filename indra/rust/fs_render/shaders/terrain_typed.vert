#version 450
// Ground Phase 1: typed terrain (mesh built engine-side from the region heightmap). Writes the same
// G-buffer the deferred resolve already reads (RT0 albedo+flag, RT1 eye-space normal). Vertices are
// world/agent-space (region origin + grid*metres + height); the camera view matrix takes them to eye
// space so the normal matches the resolve's eye-space sun_dir.
layout(location = 0) in vec3 pos;      // world/agent-space terrain vertex (SL Z-up)
layout(location = 1) in vec3 normal;   // world/agent-space normal
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj;   // proj * view : world -> clip
    mat4 view;        // world -> eye ; mat3(view) is the eye-space normal transform (rigid, no scale)
} u;
layout(location = 0) out vec3 v_normal; // eye-space (normalized in the fragment)
void main() {
    v_normal = mat3(u.view) * normal;
    gl_Position = u.view_proj * vec4(pos, 1.0);
}
