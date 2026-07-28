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
    // reveal >= 1 (or NaN) -> nothing usable here; keep the background untouched.
    if (!(reveal < 1.0))
    {
        discard;
    }

    vec4 a = texelFetch(accum, tc, 0);
    vec3 avg_color = a.rgb / max(a.a, 1e-4);

    // Heavy overdraw in bright light (e.g. many lit hair-strand layers) can overflow
    // the fp16 accum to +Inf/NaN. That pixel's colour is unrecoverable, so DROP it
    // (show the lit scene behind) instead of flashing white -- propagating the Inf, as
    // the old max() guard did, is exactly what produced the white blowout. The weight
    // is also scaled down (see the accum shaders) so this path is rarely taken.
    if (any(isnan(avg_color)) || any(isinf(avg_color)))
    {
        discard;
    }

    // Coverage of the translucent layers = 1 - reveal; blend that over the scene.
    frag_color = vec4(avg_color, 1.0 - reveal);
}
