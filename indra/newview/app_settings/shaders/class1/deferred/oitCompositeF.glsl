/**
 * @file oitCompositeF.glsl
 * WBOIT composite pass - resolves the accum + revealage buffers into a single
 * translucent colour and blends it over the lit scene. Runs AFTER the residual
 * (particle/fullbright/emissive) pass so it can also attenuate the glow channel
 * (screen.a) by the layer transmittance: the caller sets a separate colour/alpha
 * blend where RGB = over-blend (coverage = 1-reveal) and A = dst.a * reveal, which
 * restores the glow occlusion the sorted path gets from lit alpha's alpha blend.
 * Fullscreen; paired with postDeferredNoTCV.glsl. <FS>
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$ ... $/LicenseInfo$
 */

out vec4 frag_color;

uniform sampler2D accum;     // RGBA32F: sum of weighted premultiplied colour + weighted alpha
uniform sampler2D revealage; // R16F: product of (1 - alpha)

void main()
{
    ivec2 tc = ivec2(gl_FragCoord.xy);

    float reveal = texelFetch(revealage, tc, 0).r;
    // reveal >= 1 (or NaN) -> nothing was drawn here; keep background + glow untouched.
    if (!(reveal < 1.0))
    {
        discard;
    }

    vec4 a = texelFetch(accum, tc, 0);
    vec3 avg_color = a.rgb / max(a.a, 1e-4);

    // Guard against non-finite accum (unrecoverable) -> drop the pixel rather than
    // flash white. FP32 accum makes this path rare; kept as a safety net.
    if (any(isnan(avg_color)) || any(isinf(avg_color)))
    {
        discard;
    }

    // rgb = resolved translucent colour; a = coverage (1-reveal). The caller's blend
    // uses coverage for the RGB over-blend AND, via ONE_MINUS_SRC_ALPHA on the alpha
    // channel, multiplies screen.a (glow) by reveal.
    frag_color = vec4(avg_color, 1.0 - reveal);
}
