#version 450
// #3: deferred G-buffer fill for opaque generic world geometry.
//   RT0 (g_albedo) = albedo.rgb + gbufferFlag in .a
//   RT1 (g_normal) = eye-space normal (Rgba16Float, signed)
// The resolve pass reads these and lights HAS_ATMOS pixels; the flag lets it tell lit
// world geometry from sky/pass-through. This mirrors stock's frag_data[] MRT fill,
// minimal subset (albedo + normal); ORM/emissive are added with the PBR path (#3b).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in int v_texidx;
layout(location = 3) in vec3 v_normal;
layout(set = 0, binding = 0) uniform U {
    mat4 mvp; vec4 color_u; vec4 plane_s; vec4 plane_t; mat4 texmat;
    vec4 khr_sr; vec4 khr_off; vec4 flags; mat3 normal_mat;
} u;
layout(set = 0, binding = 1) uniform texture2D tex0;
layout(set = 0, binding = 2) uniform texture2D tex1;
layout(set = 0, binding = 3) uniform texture2D tex2;
layout(set = 0, binding = 4) uniform texture2D tex3;
layout(set = 0, binding = 5) uniform sampler smp;
layout(location = 0) out vec4 g_albedo; // rgb albedo, a = gbufferFlag
layout(location = 1) out vec4 g_normal; // eye-space normal
layout(location = 2) out vec4 g_spec;   // RT2: legacy spec color.rgb + glossiness.a (no material data -> matte)

const float GBUFFER_FLAG_HAS_ATMOS = 0.34; // stock GBUFFER_FLAG_HAS_ATMOS

void main() {
    vec4 t;
    if (v_texidx == 1)      t = texture(sampler2D(tex1, smp), v_uv);
    else if (v_texidx == 2) t = texture(sampler2D(tex2, smp), v_uv);
    else if (v_texidx == 3) t = texture(sampler2D(tex3, smp), v_uv);
    else                    t = texture(sampler2D(tex0, smp), v_uv);
    vec3 albedo = t.rgb * v_color.rgb * u.color_u.rgb;
    float a = t.a * v_color.a * u.color_u.a;
    // MASK cutout (opaque G-buffer draws reach here only when blending is OFF).
    if (u.flags.z >= 0.0 && a < u.flags.z) discard;
    g_albedo = vec4(albedo, GBUFFER_FLAG_HAS_ATMOS);
    g_normal = vec4(normalize(v_normal), 1.0);
    g_spec = vec4(0.0); // glossiness 0 -> resolve's Blinn-Phong specular is off until real material data lands
}
