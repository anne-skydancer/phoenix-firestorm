#version 450
// Phase A.3: native-VK UI fragment. Modulate the vertex color by the bound texture. Solid
// draws (no texture) bind the engine's 1x1 white -> color * 1 = color. Textured images and
// text (font-atlas glyph quads) both just modulate; the alpha blend does the compositing.
layout(set = 0, binding = 1) uniform texture2D t_tex;
layout(set = 0, binding = 2) uniform sampler   s_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = v_color * texture(sampler2D(t_tex, s_tex), v_uv);
}
