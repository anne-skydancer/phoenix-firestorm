#version 450
// skyF.glsl tier-1: haze color *2, clamp to 5 (no rainbow/halo overlays yet).
layout(location = 0) in vec3 v_haze;
layout(location = 0) out vec4 frag;
void main() {
    vec3 c = min(v_haze * 2.0, vec3(5.0));
    frag = vec4(c, 1.0);
}
