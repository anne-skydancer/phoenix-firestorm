/**
 * @file oitCompositeF.glsl
 * WBOIT composite pass - resolves the accum + revealage buffers into a single
 * translucent colour and blends it over the lit scene (with the usual
 * SRC_ALPHA/ONE_MINUS_SRC_ALPHA fixed-function blend). Fullscreen; paired with
 * postDeferredNoTCV.glsl. <FS>
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$ ... $/LicenseInfo$
 */

out vec4 frag_color;

uniform sampler2D accum;     // RGBA16F: weighted premultiplied colour + weighted alpha
uniform sampler2D revealage; // R16F: product of (1 - alpha)

void main()
{
    ivec2 tc = ivec2(gl_FragCoord.xy);

    float reveal = texelFetch(revealage, tc, 0).r;
    // reveal == 1 -> nothing was drawn here; keep the background untouched.
    if (reveal >= 1.0)
    {
        discard;
    }

    vec4 a = texelFetch(accum, tc, 0);
    // Guard against fp16 overflow to +Inf in heavy overdraw.
    if (isinf(a.r) || isinf(a.g) || isinf(a.b) || isinf(a.a))
    {
        a.rgb = vec3(max(max(a.r, a.g), a.b));
    }

    vec3 avg_color = a.rgb / max(a.a, 1e-5);
    // Coverage of the translucent layers = 1 - reveal; blend that over the scene.
    frag_color = vec4(avg_color, 1.0 - reveal);
}
