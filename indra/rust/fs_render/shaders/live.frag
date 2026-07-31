#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in int v_texidx;
layout(set = 0, binding = 0) uniform U {
    mat4 mvp; vec4 color_u; vec4 plane_s; vec4 plane_t; mat4 texmat;
    vec4 khr_sr;   // KHR scale.xy, rotation, _
    vec4 khr_off;  // KHR offset.xy
    vec4 flags;    // .x = blending enabled at draw time
} u;
layout(set = 0, binding = 1) uniform texture2D tex0;
layout(set = 0, binding = 2) uniform texture2D tex1;
layout(set = 0, binding = 3) uniform texture2D tex2;
layout(set = 0, binding = 4) uniform texture2D tex3;
layout(set = 0, binding = 5) uniform sampler smp;
layout(location = 0) out vec4 frag;
void main() {
    vec4 t;
    if (v_texidx == 1)      t = texture(sampler2D(tex1, smp), v_uv);
    else if (v_texidx == 2) t = texture(sampler2D(tex2, smp), v_uv);
    else if (v_texidx == 3) t = texture(sampler2D(tex3, smp), v_uv);
    else                    t = texture(sampler2D(tex0, smp), v_uv);
    vec4 c = vec4(t.rgb * v_color.rgb * u.color_u.rgb, t.a * v_color.a * u.color_u.a);
    // MASK-mode cutout first (viewer discards on combined alpha BEFORE any opaque
    // handling, pbropaqueF.glsl:75-80); -1 disables.
    if (u.flags.z >= 0.0 && c.a < u.flags.z) {
        discard;
    }
    // Blending OFF = an opaque pass: stock shaders ignore texture alpha there
    // (diffuse alpha mode NONE). Discarding black/alpha-0 texels in opaque passes
    // erased what stock draws solid (the ghost HUD ring).
    if (u.flags.x < 0.5) {
        c.a = 1.0;
    } else if (c.a < 0.004) {
        discard;
    }
    frag = c;
}
