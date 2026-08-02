# Native Vulkan — a third selectable backend (parallel idiomatic port)

Add **native Vulkan** as a third, restart-selectable render backend beside the two the viewer
already ships. It is an **idiomatic parallel port of the OpenGL renderer** onto the GPU's own
Vulkan driver (via `fs_render`, Rust/wgpu) — **no more, no less**: it reproduces what the GL
renderer produces, reading the same scene, in native Vulkan.

> This supersedes the earlier "retarget the six render classes onto a two-backend RHI" plan.
> That approach fought upstream in `pipeline.cpp` and required a general two-backend abstraction
> (`indra/rhi/llrhi.h`, now set aside). The six code traces it produced are **kept as the
> reference** for *what the VK renderer must reproduce* — they are not wasted; only the retarget
> strategy is.

## HARD INVARIANTS (non-negotiable — learned from the null-GL fiasco)

The null-GL "engine mode" failed because `gpu_benchmark()` ran over no-op GL → Class1 →
medium-low (ALM off, occlusion off), and then real occlusion-query calls hit the stub, no-op'd,
returned garbage, corrupted the UI/textures, and crashed — relogging into medium-low. Native-VK
removes the *mechanism* for both failures. These are requirements, not aspirations:

1. **Real capabilities from the real device.** Native-VK owns the real `VkDevice`. There is NO
   stub and NO no-op-GL benchmark on this path. The graphics class is derived from the real
   `VkPhysicalDevice` (`fsr_query_gpu_info`), reported straight to `LLFeatureManager`, replacing
   the benchmark under `native-vulkan`. The card is reported truthfully because the reporter IS
   the card.
2. **ALM (deferred / advanced lighting) is ALWAYS ON, at every class.** It is fundamental to PBR
   SL, not a toggle. Native-VK's baseline *is* the deferred pipeline; there is no forward/"basic"
   fallback path. Class tunes shadow detail / SSAO / resolution — never whether ALM is on.
3. **The UI can never disappear.** The UI is an isolated final pass with its own attachment;
   scene occlusion is an input to world culling only, with no path to the UI framebuffer;
   occlusion is real `VkQueryPool`/viewer culling, never a garbage-returning no-op. Guarded by an
   oracle. (Additive safety net: `native-gl` stays pristine and restart-selectable, so a broken
   in-progress native-VK can never trap the user in a bad state.)

## The three backends (`RenderGLBackend`, restart to switch)

| Option | What runs | Status |
|--------|-----------|--------|
| `native-gl` | vendor OpenGL driver + the viewer's GL renderer | baseline, **untouched** |
| `zink` (Mesa) | the same GL renderer, GL→Vulkan via Mesa/Zink | baseline, **untouched** |
| `native-vulkan` | the **parallel VK renderer** (fs_render) reading the scene | **new** |

One backend is live per session (like the Zink toggle). Under `native-vulkan` the GL rendering
path **does not run** — the VK renderer *is* the frame.

## Why this is weeks-scoped, not months

- **Additive → zero regression.** GL and Zink stay as fallbacks. The VK backend never has to be
  complete or perfect to ship; you can always fall back. No big-bang, no "don't break anything."
- **No re-architecture.** It is a *faithful reproduction*, not a redesign. It reads the **same
  scene the GL renderer already reads** and reproduces the **same passes** in Vulkan. No general
  render framework, no PSO-cache-as-a-system, no scene-graph abstraction to invent.
- **The engine already exists.** `fs_render` is a working wgpu renderer with the pass shape and
  the S1–S3 atmospherics (tonemap/sky/regime + oracles). This backend *is* that engine, finished
  as a faithful port of the GL passes — a head start, not a from-scratch renderer.
- **GL is a live parity oracle.** Same scene, run it on `native-gl` then on `native-vulkan`,
  diff the frames. That is the "carbon copy" check, continuously.
- **Single active backend** → no dual-path/dual-upload complexity.

## What it is / isn't

- **IS:** a parallel VK renderer that reads the same scene structures the GL pools/`pipeline.cpp`
  read — the culled/batched `LLDrawInfo` lists on spatial groups, geometry, `LLEnvironment`,
  texture data — and reproduces the pass graph in native Vulkan.
- **ISN'T:** a retarget of the shared GL classes; a GL→Vulkan translator (that's Zink); a scene
  re-architecture; a two-backend render API.

## Invasive footprint (minimal — this is the point)

1. **Backend selection:** `RenderGLBackend += "native-vulkan"`; a top-level switch that routes
   the frame to the VK renderer instead of the GL frame. Restart-selected, same as Zink.
2. **Two resource classes made backend-aware** (decided once at init, single active backend):
   - `LLVertexBuffer` — geometry lands in VK buffers.
   - `LLImageGL` / `LLCubeMap` — textures land in VK.
3. **The parallel VK renderer** — the engine + a viewer-side scene reader (see Open Question).
4. **Shaders → SPIR-V** (+ the loose-uniform → std140 UBO reorg; the one large rock).

`pipeline.cpp`, the draw pools, `LLRender`, and the `LLGLSLShader`/`LLRenderTarget` *logic* stay
**GL-pristine** — under `native-vulkan` they simply don't run. Clean upstream merges.

## The reference — what to reproduce (from the six traces)

- **Pass graph:** deferred, 6 phases, ~20 targets — shadows → G-buffer → lighting → alpha/WBOIT →
  post/tonemap → UI. [trace: LLRenderTarget]
- **Scene:** the `LLDrawInfo` batch lists, the closed 14-attribute vertex type-mask, materials.
  [trace: LLVertexBuffer, LLRender]
- **Shaders:** 227 `.glsl`, 876 loose uniforms across 189 files, 220 combined samplers — already
  modern core GLSL, only two transforms needed (UBO + sampler split). [trace: LLGLSLShader]
- **Per-pass state:** the canonical PSO combinations (opaque/alpha/emissive/decal/UI/skybox).
  [trace: LLGLState]

These describe *what* the VK renderer reproduces; they are the port's spec.

## Phases (gated; GL/Zink always shippable throughout)

- **Phase A — walking skeleton.** `native-vulkan` selectable; the parallel reader drives the
  engine to render **clear + sky** from `LLEnvironment` (reuses S1–S3). Proves selection +
  parallel-read + engine, end to end, with nothing else touched.
- **Phase B — opaque world.** Scene reader over the `LLDrawInfo` opaque batches; geometry +
  texture upload to VK (the two resource classes); G-buffer + lighting resolve + sun shadows
  (S1–S3 lighting re-homes here). First recognizable in-world frame on native VK.
- **Phase C — the full deferred graph.** Alpha/WBOIT, aerial haze, post/tonemap/exposure, glow,
  DoF, FXAA/SMAA — faithful to the GL pass graph.
- **Phase D — the rest of the scene.** Rigged avatars, terrain, water, particles, UI.

Shaders port alongside each phase (GLSL→SPIR-V + UBO), UBO half GL-verified where possible.
The **GL parity oracle** (GL session vs VK session, image diff) gates each phase.

## Open question (decide before Phase B)

**Where the render logic lives.** Both are "parallel reader":
- **(i) Engine owns the passes** (Rust/wgpu), fed *typed scene data* by a thin viewer-side reader
  (the `scene.rs` direction already started for atmospherics). Most idiomatic; VK resource
  lifetime stays in Rust; reuses the engine's existing pass work. Cost: port `pipeline.cpp`'s
  orchestration to Rust. **← current lean.**
- **(ii) Viewer-side C++ parallel pipeline** calling the engine's fine-grained VK API. Keeps the
  orchestration in C++ (closer to the GL reference) but pushes per-draw VK across a C-ABI and
  manages VK lifetimes from C++.

Phase A is identical under both, so it does not block this decision.
