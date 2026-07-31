#version 450
layout(location = 0) in vec4 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(set = 0, binding = 0) uniform U { mat4 mvp; vec4 color_u; } u;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
    v_uv = uv;
    v_color = color;
    gl_Position = u.mvp * vec4(pos.xyz, 1.0);
}
