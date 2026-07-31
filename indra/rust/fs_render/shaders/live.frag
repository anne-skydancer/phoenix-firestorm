#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(set = 0, binding = 0) uniform U { mat4 mvp; vec4 color_u; } u;
layout(set = 0, binding = 1) uniform texture2D tex0;
layout(set = 0, binding = 2) uniform sampler smp;
layout(location = 0) out vec4 frag;
void main() {
    vec4 t = texture(sampler2D(tex0, smp), v_uv);
    vec4 c = vec4(t.rgb * v_color.rgb * u.color_u.rgb, t.a * v_color.a * u.color_u.a);
    if (c.a < 0.004) discard;
    frag = c;
}
