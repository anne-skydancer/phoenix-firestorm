#version 450
// Faithful port of class1/deferred/terrainF.glsl:
//   out = mix( mix(d3, d2, alpha2), mix(d1, d0, alpha1), alphaFinal )
// with alpha1 = ramp(tc).a, alpha2 = ramp(tc-(2,0)).a, alphaFinal = ramp(tc-(1,0)).a.
// ONE deliberate difference (gap G6): their gbuffer writes alpha 0 for downstream
// atmospherics; we composite directly, so alpha must be 1 or terrain vanishes.
layout(location = 0) in vec2 v_detail;
layout(location = 1) in vec4 v_ramp01;
layout(location = 2) in vec2 v_rampF;
layout(set = 0, binding = 0) uniform U {
    mat4 mvp;
    vec4 color_u;
    vec4 plane_s;
    vec4 plane_t;
    mat4 texmat; // unused by terrain (identity in the main pass)
    vec4 khr_sr;
    vec4 khr_off;
    vec4 flags;
} u;
layout(set = 0, binding = 1) uniform texture2D detail0;
layout(set = 0, binding = 2) uniform texture2D detail1;
layout(set = 0, binding = 3) uniform texture2D detail2;
layout(set = 0, binding = 4) uniform texture2D detail3;
layout(set = 0, binding = 5) uniform texture2D ramp;
layout(set = 0, binding = 6) uniform sampler smpWrap;
layout(set = 0, binding = 7) uniform sampler smpClamp;
layout(location = 0) out vec4 frag;
void main() {
    vec4 c0 = texture(sampler2D(detail0, smpWrap), v_detail);
    vec4 c1 = texture(sampler2D(detail1, smpWrap), v_detail);
    vec4 c2 = texture(sampler2D(detail2, smpWrap), v_detail);
    vec4 c3 = texture(sampler2D(detail3, smpWrap), v_detail);
    float alpha1 = texture(sampler2D(ramp, smpClamp), v_ramp01.xy).a;
    float alpha2 = texture(sampler2D(ramp, smpClamp), v_ramp01.zw).a;
    float alphaF = texture(sampler2D(ramp, smpClamp), v_rampF).a;
    vec4 c = mix(mix(c3, c2, alpha2), mix(c1, c0, alpha1), alphaF);
    frag = vec4(max(c.rgb, vec3(0.0)) * u.color_u.rgb, 1.0);
}
