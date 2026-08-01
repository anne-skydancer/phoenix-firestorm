#version 450
// Fullscreen triangle for the post/composite pass (no vertex buffer bound).
// Indices 0,1,2 -> clip positions (-1,-1),(3,-1),(-1,3), which cover the whole
// viewport. gl_FragCoord in the fragment shader gives the framebuffer pixel, so the
// composite is done by texelFetch (see post_copy.frag) -- no UVs needed here.
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
