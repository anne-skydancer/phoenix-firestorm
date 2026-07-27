/**
 * @file oitAccumF.glsl
 * WBOIT accumulation pass - fragment shader. Writes the two OIT buffers:
 *   frag_data[0] (accum, RGBA16F) = weighted premultiplied colour + weighted alpha
 *   frag_data[1] (revealage, R16F) = alpha (blended to the product of 1-alpha)
 * Colour comes from the DIFFUSE_COLOR uniform `color` (set via gGL.diffuseColor).
 * <FS>
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$ ... $/LicenseInfo$
 */

out vec4 frag_data[2];

uniform vec4 color;      // DIFFUSE_COLOR: rgb + alpha of this surface

in float vary_view_z;

void main()
{
    vec4 c = color;

    // McGuire/Bavoil depth weight (eq. 9-ish): nearer + more-opaque fragments
    // dominate, so the weighted average approximates correct back-to-front order
    // without any sort. z is the positive view-space distance.
    float z = abs(vary_view_z);
    float w = c.a * clamp(0.03 / (1e-5 + pow(z / 200.0, 4.0)), 1e-2, 3e3);

    frag_data[0] = vec4(c.rgb * c.a, c.a) * w; // accum
    frag_data[1] = vec4(c.a);                  // revealage (only .r is stored)
}
