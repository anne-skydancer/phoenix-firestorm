/**
 * @file deferred/fullbrightF.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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
#extension GL_ARB_shader_atomic_counters : enable

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

#if defined(IS_ALPHA) && (defined(ALPHA_OIT) || defined(ALPHA_DEPTH_PEEL))
uniform int oit_mode;
#endif

#if defined(IS_ALPHA) && defined(ALPHA_OIT)
// ---- alpha OIT (per-pixel linked list) capture ----
    // no early_fragment_tests: occlusion is rejected in the resolve (alphaOITResolveF) by depth compare;
layout(binding = 0, r32ui) uniform coherent uimage2D oit_head;
layout(std430, binding = 0) buffer OITNodePool { uint oit_nodes[]; };
layout(binding = 0, offset = 0) uniform atomic_uint oit_counter;
uniform int oit_node_cap;   // node pool capacity; overflow falls through to legacy blending
bool oit_append(vec4 c, float z)
{
    uint idx = atomicCounterIncrement(oit_counter);
    if (idx >= uint(oit_node_cap)) return false;
    uint prev = imageAtomicExchange(oit_head, ivec2(gl_FragCoord.xy), idx);
    uint base = idx * 4u;
    oit_nodes[base + 0u] = packHalf2x16(max(c.rg, vec2(0.0)));
    oit_nodes[base + 1u] = packHalf2x16(vec2(max(c.b, 0.0), clamp(c.a, 0.0, 1.0)));
    oit_nodes[base + 2u] = floatBitsToUint(z);
    oit_nodes[base + 3u] = prev;
    return true;
}
#endif

#if defined(IS_ALPHA) && defined(ALPHA_DEPTH_PEEL)
uniform sampler2D alpha_peel_depth;
uniform int alpha_peel_first;
void alpha_depth_peel(inout vec4 c, float z)
{
    if (oit_mode == 2)
    {
        if (alpha_peel_first == 0)
        {
            float selected = texelFetch(alpha_peel_depth, ivec2(gl_FragCoord.xy), 0).r;
            if (z >= selected) discard;
        }
        c = vec4(z);
    }
    else if (oit_mode == 3)
    {
        float selected = texelFetch(alpha_peel_depth, ivec2(gl_FragCoord.xy), 0).r;
        if (z != selected) discard;
    }
    else if (oit_mode == 4)
    {
        float selected = texelFetch(alpha_peel_depth, ivec2(gl_FragCoord.xy), 0).r;
        if (z >= selected) discard;
    }
}
#endif

#if !defined(HAS_DIFFUSE_LOOKUP)
uniform sampler2D diffuseMap;
#endif

in vec3 vary_position;
in vec4 vertex_color;
in vec2 vary_texcoord0;

vec3 srgb_to_linear(vec3 cs);
vec3 linear_to_srgb(vec3 cl);

#ifdef HAS_ALPHA_MASK
uniform float minimum_alpha;
#endif

#ifdef IS_ALPHA
uniform vec4 waterPlane;
void waterClip(vec3 pos);
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten);
vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
#endif

void mirrorClip(vec3 pos);

void main()
{
    mirrorClip(vary_position);
#ifdef IS_ALPHA
    waterClip(vary_position.xyz);
#endif

#ifdef HAS_DIFFUSE_LOOKUP
    vec4 color = diffuseLookup(vary_texcoord0.xy);
#else
    vec4 color = texture(diffuseMap, vary_texcoord0.xy);
#endif

    float final_alpha = color.a * vertex_color.a;

#ifdef HAS_ALPHA_MASK
    if (color.a < minimum_alpha)
    {
        discard;
    }
#endif

    color.rgb *= vertex_color.rgb;

    vec3 pos = vary_position;

    color.a = final_alpha;
#ifndef IS_HUD
    color.rgb = srgb_to_linear(color.rgb);
#ifdef IS_ALPHA

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVars(pos.xyz, vec3(0), 1.0, sunlit, amblit, additive, atten);

    color.rgb = applySkyAndWaterFog(pos, additive, atten, color).rgb;

#endif

#endif

#if defined(IS_ALPHA) && defined(ALPHA_OIT)
    vec4 oit_out = max(color, vec4(0));
    if (oit_mode == 1 && oit_append(oit_out, gl_FragCoord.z)) { discard; }
#ifdef ALPHA_DEPTH_PEEL
    alpha_depth_peel(oit_out, gl_FragCoord.z);
#endif
    frag_color = oit_out;
#else
    vec4 oit_out = max(color, vec4(0));
#if defined(IS_ALPHA) && defined(ALPHA_DEPTH_PEEL)
    alpha_depth_peel(oit_out, gl_FragCoord.z);
#endif
    frag_color = oit_out;
#endif
}
