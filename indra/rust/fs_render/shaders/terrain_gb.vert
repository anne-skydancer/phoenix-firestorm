#version 450
// Ground P2a: terrain -> deferred G-buffer fill. World-space mesh (pos + normal); this pass only
// records albedo + normal + depth -- the softenLight resolve does the lighting. Reuses the
// terrain_typed UBO layout (only view_proj is used here).
layout(location = 0) in vec3 pos;      // world/agent-space vertex
layout(location = 1) in vec3 normal;   // world/agent-space normal
layout(set = 0, binding = 0) uniform U {
    mat4 view_proj;   // world -> clip (reverse-Z)
    vec4 sun_dir;     // unused here (shared UBO with the forward path)
    vec4 sunlight;
    vec4 ambient;
    vec4 misc;
} u;
layout(location = 0) out vec3 v_normal; // world-space (normalized in the fragment)
void main() {
    v_normal = normal;
    gl_Position = u.view_proj * vec4(pos, 1.0);
}
