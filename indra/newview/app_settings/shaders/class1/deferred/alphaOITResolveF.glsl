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
layout(std430, binding = 0) buffer OITNodePool { uint oit_nodes[]; };
uniform sampler2D depthMap;   // opaque scene depth (DEFERRED_DEPTH) -- occlusion reject vs captured nodes
uniform int oit_max_pixel_layers;

// Exact-sort the user-selected number of layers, then collapse the remainder
// into the archived implementation's order-independent weighted tail.  The
// traversal cap prevents corrupt data from creating an unbounded shader loop.
#define OIT_MAX_LAYERS 32
#define OIT_MAX_TRAVERSAL 256

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    // Opaque scene depth at this pixel. The capture shaders have NO early_fragment_tests (it is
    // expensive on Zink and, being compile-time, perturbed the normal alpha path), so fragments
    // behind opaque geometry were still appended -- reject them here. Standard depth (nearer =
    // smaller z): keep nz <= opaque_z, the same LEQUAL test the hardware early test used.
    float opaque_z = texelFetch(depthMap, coord, 0).r;

    uint idx = imageLoad(oit_head, coord).r;
    if (idx == 0xFFFFFFFFu)
    {
        discard;   // no transparent fragments at this pixel
    }

    uint  nrg[OIT_MAX_LAYERS];
    uint  nba[OIT_MAX_LAYERS];
    float ndep[OIT_MAX_LAYERS];
    int cnt = 0;
    int traversed = 0;
    while (idx != 0xFFFFFFFFu &&
           cnt < OIT_MAX_LAYERS &&
           cnt < oit_max_pixel_layers &&
           traversed < OIT_MAX_TRAVERSAL)
    {
        uint  base = idx * 4u;
        float nz   = uintBitsToFloat(oit_nodes[base + 2u]);
        uint  nxt  = oit_nodes[base + 3u];
        traversed++;
        if (nz <= opaque_z)   // visible (in front of opaque) -> keep; else occluded -> skip
        {
            nrg[cnt] = oit_nodes[base + 0u];
            nba[cnt] = oit_nodes[base + 1u];
            ndep[cnt] = nz;
            cnt++;
        }
        idx = nxt;
    }

    // Deep stacks degrade continuously instead of dropping layers. Coverage and
    // the coverage-weighted colour average are independent of list order.
    vec3 tail_wsum = vec3(0.0);
    float tail_asum = 0.0;
    float tail_trans = 1.0;
    while (idx != 0xFFFFFFFFu && traversed < OIT_MAX_TRAVERSAL)
    {
        uint base = idx * 4u;
        float nz = uintBitsToFloat(oit_nodes[base + 2u]);
        uint nxt = oit_nodes[base + 3u];
        traversed++;
        if (nz <= opaque_z)
        {
            vec2 rg = unpackHalf2x16(oit_nodes[base + 0u]);
            vec2 ba = unpackHalf2x16(oit_nodes[base + 1u]);
            vec4 c = vec4(rg, ba);
            tail_wsum += c.rgb * c.a;
            tail_asum += c.a;
            tail_trans *= (1.0 - c.a);
        }
        idx = nxt;
    }

    // insertion sort DESCENDING by window depth. Standard depth (near=0, far=1): largest z is
    // farthest, so descending order = back-to-front for the "over" composite below.
    for (int i = 1; i < cnt; i++)
    {
        uint  rg = nrg[i];
        uint  ba = nba[i];
        float d = ndep[i];
        int j = i;
        while (j > 0 && ndep[j - 1] < d)
        {
            nrg[j] = nrg[j - 1];
            nba[j] = nba[j - 1];
            ndep[j] = ndep[j - 1];
            j--;
        }
        nrg[j] = rg;
        nba[j] = ba;
        ndep[j] = d;
    }

    vec3  accum = vec3(0.0);
    float cov   = 0.0;
    if (tail_asum > 0.0)
    {
        float tail_cov = 1.0 - tail_trans;
        accum = (tail_wsum / tail_asum) * tail_cov;
        cov = tail_cov;
    }
    for (int i = 0; i < cnt; i++)
    {
        vec2 rg = unpackHalf2x16(nrg[i]);
        vec2 ba = unpackHalf2x16(nba[i]);
        vec4 c = vec4(rg, ba);
        accum = c.rgb * c.a + accum * (1.0 - c.a);
        cov   = c.a       + cov   * (1.0 - c.a);
    }

    frag_color = vec4(accum, cov);
}
