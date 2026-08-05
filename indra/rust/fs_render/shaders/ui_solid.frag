#version 450
// U6: faithful port of stock gSolidColorProgram (interface/solidcolorF.glsl). The texture is used ONLY
// as an ALPHA MASK; the RGB comes entirely from the color (which stock carries in the DIFFUSE_COLOR
// uniform, but the tap stamps into the per-vertex color for gSolidColorProgram batches -- mColorsp is
// stale/white for them). Selected per draw by the solid flag on fsr_ui_submit.
//   stock: frag_color = vec4(color.rgb, texture(tex0, uv).a * color.a);
layout(set = 0, binding = 1) uniform texture2D t_tex;
layout(set = 0, binding = 2) uniform sampler   s_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    float a = texture(sampler2D(t_tex, s_tex), v_uv).a * v_color.a;
    out_color = vec4(v_color.rgb, a);
}
