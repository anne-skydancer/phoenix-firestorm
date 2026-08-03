#version 450
// Composite the half-res cloud target over scene_hdr. Rendered full-res to scene_hdr with LoadOp::Load
// and alpha blend (src = SrcAlpha, dst = OneMinusSrcAlpha), so the ROP computes
//   out = cloud.rgb * cloud.a + scene_hdr * (1 - cloud.a) = mix(scene_hdr, cloud.rgb, cloud.a).
// Bilinear sampling of the half-res target upscales + smooths the per-pixel jitter grain.
layout(set = 0, binding = 0) uniform texture2D cloud;
layout(set = 0, binding = 1) uniform sampler samp;
layout(location = 0) out vec4 frag;
void main() {
    vec2 full_res = vec2(textureSize(sampler2D(cloud, samp), 0)) * 2.0;   // cloud is half-res
    vec2 uv = gl_FragCoord.xy / full_res;
    frag = texture(sampler2D(cloud, samp), uv);   // rgb=cloud radiance, a=coverage; pipeline blends it
}
