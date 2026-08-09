/**
 * @file alphaOITResolveF.glsl
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#extension GL_ARB_shader_storage_buffer_object : enable
#extension GL_ARB_shader_image_load_store : enable

/*[EXTRA_CODE_HERE]*/

// Alpha OIT resolve: walk each pixel's per-pixel linked list (built by the capture pass),
// depth-sort it, and composite it "over" the current screen back-to-front. Output is
// premultiplied color + coverage; the composite blend is ONE, ONE_MINUS_SRC_ALPHA with the
// alpha channel masked off so emissive glow already in screen.a survives.

out vec4 frag_color;

layout(binding = 0, r32ui) uniform readonly uimage2D oit_head;   // per-pixel list head (0xFFFFFFFF = empty)
layout(std430, binding = 0) buffer OITNodePool { uint oit_nodes[]; };  // flat: [3*i]=rgba8, [+1]=depth, [+2]=next

// Nodes up to this cap are exact-sorted + composited. Anything beyond spills to an
// order-independent weighted-average tail (below), so deep stacks degrade instead of dropping.
#define OIT_MAX_LAYERS 32

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    uint idx = imageLoad(oit_head, coord).r;
    if (idx == 0xFFFFFFFFu)
    {
        discard;   // no transparent fragments at this pixel
    }

    uint  ncol[OIT_MAX_LAYERS];
    float ndep[OIT_MAX_LAYERS];
    int cnt = 0;
    while (idx != 0xFFFFFFFFu && cnt < OIT_MAX_LAYERS)
    {
        uint base = idx * 3u;
        ncol[cnt] = oit_nodes[base + 0u];
        ndep[cnt] = uintBitsToFloat(oit_nodes[base + 1u]);
        idx = oit_nodes[base + 2u];
        cnt++;
    }

    // tail: any nodes beyond the exact-sort cap are blended order-independently (coverage-
    // weighted average) so very deep stacks (dense hair/lace) degrade gracefully rather than
    // dropping. Coverage (1 - prod(1-a)) is order-independent; the average colour is too.
    vec3  tail_wsum  = vec3(0.0);   // sum(c.rgb * c.a)
    float tail_asum  = 0.0;         // sum(c.a)
    float tail_trans = 1.0;         // prod(1 - c.a) -> tail coverage = 1 - tail_trans
    while (idx != 0xFFFFFFFFu)
    {
        vec4 c = unpackUnorm4x8(oit_nodes[idx * 3u]);
        tail_wsum  += c.rgb * c.a;
        tail_asum  += c.a;
        tail_trans *= (1.0 - c.a);
        idx = oit_nodes[idx * 3u + 2u];
    }

    // insertion sort DESCENDING by window depth. Standard depth (near=0, far=1): largest z is
    // farthest, so descending order = back-to-front for the "over" composite below.
    for (int i = 1; i < cnt; i++)
    {
        uint  c = ncol[i];
        float d = ndep[i];
        int j = i;
        while (j > 0 && ndep[j - 1] < d)
        {
            ncol[j] = ncol[j - 1];
            ndep[j] = ndep[j - 1];
            j--;
        }
        ncol[j] = c;
        ndep[j] = d;
    }

    // start the accumulator with the tail (treated as the farthest contribution), then
    // over-composite the exact-sorted set on top, far -> near. accum stays premultiplied.
    vec3  accum = vec3(0.0);
    float cov   = 0.0;
    if (tail_asum > 0.0)
    {
        vec3  tail_col = tail_wsum / tail_asum;   // coverage-weighted average colour
        float tail_cov = 1.0 - tail_trans;
        accum = tail_col * tail_cov;              // premultiplied
        cov   = tail_cov;
    }
    for (int i = 0; i < cnt; i++)
    {
        vec4 c = unpackUnorm4x8(ncol[i]);   // straight rgba as stored by oit_append
        accum = c.rgb * c.a + accum * (1.0 - c.a);
        cov   = c.a       + cov   * (1.0 - c.a);
    }

    frag_color = vec4(accum, cov);
}
