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
}

#endif
