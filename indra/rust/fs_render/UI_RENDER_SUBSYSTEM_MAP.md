# Complete UI / Render Subsystem Map — engine-mode (the FULL trace)

> The prior UI docs (`UI_STRATIGRAPHY.md`, `UI_SKINNING_STRATIGRAPHY.md`) covered only ONE path — the 2D
> immediate-mode widget tree. That was incomplete. This maps the ENTIRE subsystem a user sees: every
> on-screen UI pass AND every offscreen/render-to-texture producer, and how each behaves under the null-GL
> stub + fs_render engine. Traced by three read-only passes over `c:/fs/firestorm-upstream` + engine reads.
> Citations `file:line`.

The subsystem splits cleanly into **two layers**, and the split is the key insight:

- **Layer 1 — the one on-screen composite** (`render_ui()`), tapped via `LLRender::flush` → `fsr_ui_submit`
  and composited by the engine over the swapchain. Mostly reachable today.
- **Layer 2 — offscreen render-to-texture (RTT)**, which in engine mode is **a no-op**: the null-GL stub
  makes every FBO call inert, so the viewer's offscreen renders produce ZERO pixels. Everything that
  depends on rendering-to-a-texture-then-reading-it is broken unless the engine does it natively.

---

## Layer 1 — the on-screen composite: `render_ui()` (`newview/llviewerdisplay.cpp:1854`)

Runs AFTER `renderFinalize()` presents the post-processed world to the swapchain (a = the engine's job).
Then, in order, all drawing directly onto the swapchain:

| # | Pass | file:line | Program(s) | Depth | Engine status |
|---|------|-----------|-----------|-------|---------------|
| a | `renderFinalize()` — world present | `:1880` | (engine) | — | ✅ engine renders the world+post to the swapchain |
| b | `render_hud_elements()` — selections, parcel lines, trackers | `:1887`→`pipeline.cpp:4112` | gUIProgram | **test ON**, write off | ⚠ tapped, but engine UI pass is depth-OFF → 3D overlays don't occlude |
| c | `render_hud_attachments()` — HUD attachments | `:1895`→`:1640` | geometry pipeline, **separate ortho** `hud_cam` | own pass | ⚠ separate ortho geometry pass, not the 2D UI tap |
| d | `render_ui_3d()` — axes, beacons, chat-range spheres | `:1912`→`:2080` | gUIProgram + **gDebugProgram** | test ON | ⚠ gDebugProgram NOT reproduced; depth off in engine |
| e | `LLHUDObject::renderAll()` — name tags, floating text, hud icons | `:1929`→`llhudobject.cpp:294` | gUIProgram | off | ✅ tapped (gUIProgram + font) — works |
| f | `render_ui_2d()` — the 2D widget tree | `:1930`→`:2138` | gUIProgram + gSolidColorProgram | off | ✅ **U0-U6 covered this** |

**What U0-U6 actually fixed applies to ALL of b-f** (they all flow through the same `LLRender::flush` tap):
color-space, scissor, blend, per-surface filter, and the gSolidColorProgram solid path. So the 2D widgets
AND name tags AND beacons' gUIProgram draws are covered. **Residual on-screen gaps:**
1. **gDebugProgram** (d) — chat-range spheres, some beacons/debug. A THIRD program (after gUIProgram +
   gSolidColorProgram) not yet reproduced. `interface/debugV/F.glsl`.
2. **Depth-testing for the 3D-world overlays** (b, d) — stock depth-tests them against the scene so they
   occlude behind geometry; the engine's UI pass is depth-off, so selection outlines / beacons / parcel
   lines draw on top of everything. Needs a depth-aware UI sub-pass (the tap already carries per-vertex z).
3. **HUD attachments** (c) — a separate orthographic *geometry* pass (`renderGeomPostDeferred(hud_cam)`),
   not the 2D UI tap. Goes through the 3D geometry bridge with a HUD projection; separate concern.
`mUIScreen` optional UI-FBO path (`RenderUIBuffer`) renders the widget tree to an FBO then blits — offscreen,
so it would break in engine mode; but it's off by default (the direct-to-swapchain path is used).

---

## Layer 2 — offscreen / RTT: **no-op in engine mode** (the architectural root)

The null-GL stub (`newview/nullgl/opengl32.c`) makes offscreen rendering inert:
`glGenFramebuffers` mints a fake id; `glBindFramebuffer`/`glFramebufferTexture2D`/`glDrawBuffers`/
`glBlitFramebuffer`/`glCopyTexImage2D` are `ng_zero` no-ops; `glCheckFramebufferStatus` lies "COMPLETE";
`glDrawArrays`/`glDrawElements` are no-ops. **Any render into an FBO produces zero pixels.** The ONLY CPU
readback is `glReadPixels` → `fsr_read_pixels`, which **ignores the bound FBO** and returns the engine's
**last presented on-screen frame** (`opengl32.c:709`; engine `read_cache` = last swapchain present,
`fs_render/src/lib.rs:48-52`) — window-res, previous camera, **UI composited in**.

Every RTT producer, and its status:

| Producer | file:line | Engine status |
|----------|-----------|---------------|
| Reflection env probes | `llreflectionmapmanager.cpp` | ✅ engine-native (fs_render probe system) |
| Hero / mirror probe | `llheroprobemanager.cpp` | ✅ engine-native |
| Sun / shadow maps | `pipeline.cpp generateSunShadow` | ✅ engine-native (CSM work) |
| Post-process ping-pong FBOs | `pipeline.cpp` | ✅ engine-native (tonemap/exposure/glow) |
| **Snapshots (regular)** | `llviewerwindow.cpp:6165 rawSnapshot` | ❌ **broken** — offscreen no-op + reads stale UI-composited present |
| **Snapshot (simple) / thumbnail** | `:6529 simpleSnapshot` / `:6157` | ❌ broken (same) |
| **360° capture** | `llfloater360capture.cpp` | ❌ broken (6× simpleSnapshot) |
| **Avatar impostors** | `pipeline.cpp:12031 generateImpostor` | ❌ broken — impostored avatars render nothing to their FBO |
| **Avatar profiler ("Complexity")** | `pipeline.cpp:11961 profileAvatar` | ❌ broken — GPU-timed render is a no-op (complexity numbers unreliable) |
| **LLViewerDynamicTexture previews** (BVH/image/sculpt/GLTF/model/param) | `lldynamictexture.cpp:185` | ❌ broken — upload/mesh/appearance preview swatches blank |
| **Avatar LOCAL bake** (`LLTexLayerSetBuffer`) | `llviewertexlayer.cpp:62` | ❌ broken IF client-side baking is used (server bakes may mask it) |

**CPU→GPU uploads (NOT FBO — these work):** media/web textures (`setSubImage`, `llviewermedia.cpp:3126`);
minimap object/parcel dot layers (CPU-rasterized → `setSubImage`, `llnetmap.cpp:573`). The minimap's boxing
is Layer-1 scissor (already fixed).

---

## The snapshot, precisely (the reported bug)

`rawSnapshot` (`llviewerwindow.cpp:6165`): clears `RENDER_DEBUG_FEATURE_UI` (`:6196`), hides HUD
(`sShowHUDAttachments=false`, `:6202`) and L$ (`:6226`); re-renders per tile via `display(...,for_snapshot=
true)` (`:6357`); reads scanlines with `glReadPixels(...GL_RGB...)` (`:6387`). In engine mode:
1. The UI feed tap (`LLRender::flush`→`uiSubmit`) is gated **only** on `uiActive()` (`fsscenedump.cpp:278`),
   **NOT** on `RENDER_DEBUG_FEATURE_UI`/`gSnapshot`/`sShowHUDAttachments` — so the engine ALWAYS gets the UI.
2. The snapshot's `display()` opens a new engine frame (`fsr_begin_frame`/`fsr_ui_begin` clear the lists) but
   **never presents it** — `fsr_end_frame` runs only in `swapBuffers`, suppressed during snapshot
   (`llwindowwin32.cpp:4100`). So the snapshot frame is never rendered by the engine.
3. `glReadPixels`→`fsr_read_pixels` returns the **previous presented normal frame** — UI composited in,
   window-res, stale camera. → **(a) UI leaks in; (b) frame incomplete/stale.**

### What a faithful engine-mode snapshot needs
1. **UI-suppressible engine frame:** the tap must respect the snapshot UI flags so the engine can render a
   frame WITHOUT UI/HUD/L$ when `show_ui`/`show_hud` are off. (Gate `uiActive()`/`uiSubmit` on
   `RENDER_DEBUG_FEATURE_UI` + `gSnapshot` + `sShowHUDAttachments`.)
2. **Actually render+present the snapshot frame:** drive `fsr_end_frame` (or a dedicated offscreen render)
   for the snapshot `display()` so the engine produces the requested camera's frame; today it never does.
3. **Offscreen at requested resolution** for high-res/tiled snapshots, since the swapchain is window-sized —
   a new engine offscreen-render+readback API (the general capability that also unblocks 360/previews).

---

## The plan (whole subsystem, prioritized)

The offscreen/RTT gap (Layer 2) is ONE architectural capability the engine lacks: **render the scene at a
given camera + resolution + UI-flag to an offscreen target and read it back.** Building it once serves
snapshots, 360, and (with per-producer cameras) impostors/previews/profiler. Ordered:

- **R1 — engine offscreen render + readback API** (`fsr_render_offscreen(camera, w, h, flags) → readback`),
  and **gate the UI tap** on the snapshot flags so an offscreen frame can omit UI/HUD/L$. Wire `rawSnapshot`
  to it. Fixes the reported snapshot bugs (a)+(b) faithfully, including high-res. Verify in-world.
- **R2 — on-screen residuals:** gDebugProgram (beacons/debug), and depth-testing for the 3D-world overlays
  (b,d) so selections/beacons occlude correctly (the tap already carries z). HUD-attachment ortho pass (c).
- **R3 — the rest of Layer 2 on the R1 mechanism:** avatar impostors, profiler/Complexity, dynamic-texture
  previews, local bake, 360 — each is "render this camera/scene offscreen, read back," per-producer.

R1 is the concrete fix for the reported issue AND the general capability; R2/R3 fall out of the same map.
Confirm scope before code.
