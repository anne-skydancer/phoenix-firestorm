/**
 * @file fsscenedump.h
 * <FS:VkBridge> P2b -- one-frame scene dump (indra/rust/vkharness PHASE2_PLAN.md).
 *
 * Dev-only, env-gated capture of a single frame's draw stream at the LLVertexBuffer
 * draw sites: vertex/index blobs (hash-deduped), typemask + draw params, current
 * program, bound diffuse texture, modelview/projection, minimal GL state, render
 * target. Consumed by `vkharness replay` (P2c) to re-render the frame and diff it
 * against the GL screenshot -- the bridge parity oracle.
 *
 * Gate: FS_SCENE_DUMP=<output dir> (created if needed). Optional
 * FS_SCENE_DUMP_FRAME=N = in-world frame to capture (default 300, ~5s in-world).
 * Env unset = zero effect; render path untouched.
 */
#ifndef FS_SCENEDUMP_H
#define FS_SCENEDUMP_H

#include "stdtypes.h"

class LLVertexBuffer;

namespace FSSceneDump
{
    // Called once per frame from display(); in_world = startup fully complete.
    void onFrame(bool in_world);
    // Called from the LLVertexBuffer draw entry points; no-op unless capturing.
    void recordDraw(const LLVertexBuffer* vb, U32 mode, U32 count, U32 indices_offset, bool indexed);
    bool active();
    // <FS:VkBridge> P3c live bridge
    bool liveActive();
    void endFrame();
    void textureUploaded(U32 id, U32 w, U32 h, const U8* rgba);
    void bufferDirty(const void* ptr);
    // <FS:VkBridge> F2 (gap G13): passes whose draws must NOT reach the engine's single
    // screen pass (shadow cascades, probe faces, water copy/exclusion/haze, bake targets).
    // <FS:VkBridge> F7/F8: draw-class tagging + per-class aux payload. Set by the draw
    // pool that OWNS the pass (clean layering: pools know what they draw; the tap does
    // not reach into newview). CLASS_TERRAIN ships detail0-3+ramp ids and the texgen
    // planes; recordDraw copies whatever is current into each DrawDesc.
    const U32 DRAWCLASS_GENERIC = 0;
    const U32 DRAWCLASS_TERRAIN = 1;
    void setDrawClass(U32 c);
    void setAuxTex(U32 slot, U32 tex_id);        // slot < 8
    void setAuxF4(U32 slot, const F32* v4);      // slot < 2 (vec4 each)
    // KHR_texture_transform (base color) for GLTF/PBR draws: planar-mapped mesh
    // repeats live HERE, not in TEXCOORD0 (llface.cpp:1545 skips CPU baking for gltf)
    // and not in texture_matrix0. packed8 = [sx,sy,rot,_, ox,oy,_,_].
    // A5: authoritative per-batch Blinn-Phong material data (one-shot, consumed by
    // the next recordDraw). Staged by LLDrawPoolMaterials right before its drawRange.
    void setMaterialBatch(U32 diffuse_id, U32 normal_id, U32 spec_id, F32 alpha_cutoff);
    void setKhrTexTransform(const F32* packed8);
    void clearKhrTexTransform();
    void suppressPush();
    void suppressPop();
    struct SuppressScope
    {
        SuppressScope() { suppressPush(); }
        ~SuppressScope() { suppressPop(); }
    };
}

#endif
