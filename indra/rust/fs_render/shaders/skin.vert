#version 450
// Faithful port of class1/avatar/objectSkinV.glsl getObjectSkinnedTransform() for the
// live bridge. weight4 packs joint index + weight per component (index = floor,
// weight = fract, renormalized). The palette is the viewer's mGLMp layout verbatim
// (llvoavatar.cpp:11262-11288): joint j = 3 vec4s, [j*3+k] = (col_k.xyz, trans[k]),
// mapping bind-shape space directly to WORLD space (avatar placement included) --
// which composes with our camera-only MVP exactly as stock's modelview * skin.
layout(location = 0) in vec4 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(location = 3) in vec4 weight4;
layout(set = 0, binding = 0) uniform U {
    mat4 mvp; vec4 color_u; vec4 plane_s; vec4 plane_t; mat4 texmat;
    vec4 khr_sr;   // KHR scale.xy, rotation, _
    vec4 khr_off;  // KHR offset.xy
    vec4 flags;    // .x = blending enabled, .y = pos.w is a tex index, .z = MASK cutoff
} u;
layout(set = 0, binding = 6) uniform P {
    vec4 palette[330]; // 110 joints x 3 vec4 (LL_MAX_JOINTS_PER_MESH_OBJECT)
} p;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out int v_texidx;

vec2 apply_uv_transforms(vec2 tc) {
    tc = (u.texmat * vec4(tc, 0.0, 1.0)).xy;
    tc.y = 1.0 - tc.y;
    float c = cos(u.khr_sr.z);
    float s = sin(u.khr_sr.z);
    mat3 scale_mat = mat3(u.khr_sr.x,0,0, 0,u.khr_sr.y,0, 0,0,1);
    mat3 offset_mat = mat3(1,0,0, 0,1,0, u.khr_off.x, u.khr_off.y, 1);
    mat3 rotation_mat = mat3(c,-s,0, s,c,0, 0,0,1);
    tc = (offset_mat * rotation_mat * scale_mat * vec3(tc, 1.0)).xy;
    tc.y = 1.0 - tc.y;
    return tc;
}

void main() {
    vec4 w = fract(weight4);
    vec4 index = clamp(floor(weight4), vec4(0.0), vec4(109.0));
    w *= 1.0 / (w.x + w.y + w.z + w.w);

    int i1 = int(index.x) * 3;
    int i2 = int(index.y) * 3;
    int i3 = int(index.z) * 3;
    int i4 = int(index.w) * 3;

    mat3 m = mat3(p.palette[i1].xyz, p.palette[i1 + 1].xyz, p.palette[i1 + 2].xyz) * w.x
           + mat3(p.palette[i2].xyz, p.palette[i2 + 1].xyz, p.palette[i2 + 2].xyz) * w.y
           + mat3(p.palette[i3].xyz, p.palette[i3 + 1].xyz, p.palette[i3 + 2].xyz) * w.z
           + mat3(p.palette[i4].xyz, p.palette[i4 + 1].xyz, p.palette[i4 + 2].xyz) * w.w;
    vec3 t = vec3(p.palette[i1].w, p.palette[i1 + 1].w, p.palette[i1 + 2].w) * w.x
           + vec3(p.palette[i2].w, p.palette[i2 + 1].w, p.palette[i2 + 2].w) * w.y
           + vec3(p.palette[i3].w, p.palette[i3 + 1].w, p.palette[i3 + 2].w) * w.z
           + vec3(p.palette[i4].w, p.palette[i4 + 1].w, p.palette[i4 + 2].w) * w.w;

    vec3 pos_world = m * pos.xyz + t;

    v_uv = apply_uv_transforms(uv);
    v_color = color;
    v_texidx = (u.flags.y > 0.5) ? clamp(floatBitsToInt(pos.w), 0, 3) : 0;
    gl_Position = u.mvp * vec4(pos_world, 1.0);
}
