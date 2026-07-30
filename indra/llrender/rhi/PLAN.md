# Firestorm Vulkan Engine — Integration Architecture & Plan (v2)

**Status:** strategy locked 2026-07-30 — **bridge the data, don't wrap the calls.**
Full-frame takeover behind a restart toggle; the standalone engine (proven H0–H4)
is the renderer; the viewer's GL path stays stock as the always-shippable fallback.

**Lineage:** v1 of this plan (243 lines, `pre-reset-backup`) designed a seam-first
RHI — abstract interface inside the viewer, GL backend #1, incremental migration.
Its **survey and analysis are salvaged verbatim below** (they were measured, not
guessed, and remain true). Its **code is quarantined**: somewhere along that line
sky and shadows got buggered (the FrameUBO clobber `6f1b172026`→reverted, the
depth-[0,1] mod `1f46e5d8ed`→reverted, and residue beyond them). `pre-reset-backup`
is *reference material* — a map of call sites and lessons — never a merge source.

## Why the strategy changed (the burn history is the argument)

Two lines of work, two track records:

- **In-viewer render surgery** — FrameUBO #1 (built green, linked clean, rendered
  wrong: day/night broke), depth [0,1] (broke shadows on BOTH backends), the v1
  seam line (sky + shadows buggered). Live modification of the GL viewer's render
  internals, validated visually, burned every time.
- **Standalone engine + oracle gates** — shadow engine M0–M5 (all gates numeric,
  the one in-viewer crossing — the M5 GL backport — oracle-designed and verified
  in-world first try), harness H0–H4 (every stage byte-exact or sub-1/255 against
  a trusted oracle, the H4a forward-oracle never drifted across eight rounds of
  change). Worked every time.

v1's Phases 1–2 required more surgery: uniform-reorg across 186 live viewer
shaders ("correctness here is visual and subtle"). The bridge strategy deletes
that class of risk: **the viewer's shaders and GL render path are never modified.**
The shader transform runs offline on a *copy* of the corpus, for the engine's own
consumption. The only in-viewer changes are the data tap and the toggle.

Native-AMD `ARB_gl_spirv` remains broken (v1's finding stands), so SPIR-V still
means Vulkan; the engine is the Vulkan path.

## What is already proven (the foundation this plan stands on)

All in `indra/rust/vkharness` (branch `fs/engine-shadows`), each gate-verified:

| Piece | Proof | Where |
|---|---|---|
| Vulkan device/swapchain/present on AMD (26.7.1 LLPC) | H0–H1; live window runs | `main.rs`, `shadow_engine.rs` |
| Viewer GLSL → shaderc → SPIR-V (Vulkan target) → wgpu | H2 | `compile_glsl` |
| Loose-uniform → std140 UBO transform | H3a byte-exact; H3b3 softenLight (heaviest shader) transformed + **runs on AMD** | H3 gates |
| Combined→separate samplers, texture descriptors | H3b2 byte-exact | |
| std140 array stride, mat4 vertex-stage UBO | H3c/H3d byte-exact | |
| Stable CSM shadows (sphere-bound + texel-snap, unified partition) | M0–M4 gates (shimmer 60105→59 px, cam-invariance ~0, continuity 0.7%); **M5 backported to viewer GL, in-world verified, on master** | `shadow_engine.rs`; `pipeline.cpp` 8306f99449 |
| Deferred G-buffer + fullscreen lighting | H4a **byte-exact vs forward oracle** (0 px whole-frame) | `run_h4a` |
| Real SL Collada meshes (node transforms, multi-primitive) | H4b loader byte-exact vs known cube; 4520-tri SL mesh renders | `dae.rs`, `run_h4b` |
| Textures via UV + sampler into deferred albedo | H4c gate 49/51 vs 0/100 | `run_h4c` |
| EEP/WindLight sky (shipped-default dome) + sun | H4d GLSL-vs-Rust-oracle mean 0.11/worst 1 per 8-bit channel over 1.02M px | `run_h4d`, `sl_sky_radiance` |
| Interactive windowed render (orbit, sun control) | `cargo run -- h4 [mesh.dae]` | `H4Scene` |

Plus the established Rust↔C++ template: `llrust` staticlib, handle/view/free FFI,
cbindgen, `LLRust.cmake` `off|cfallback|on` knobs — shipped through rust-j2c.

## Ground truth (salvaged from v1's survey + 2026-07-30 re-scout; still true)

**The draw seam is narrow.** All geometry draws funnel through **3 sites** in
`LLVertexBuffer`: `drawRange` → `glDrawRangeElements` (llvertexbuffer.cpp:963),
`drawRangeFast` (:970), `drawArrays` (:989); only 2 stray raw `glDraw*` in the
tree. State: `LLGLState::setEnabled` chokepoint + `LLRender`'s ~30 setters.
Immediate mode (`gGL.begin`, ~155 sites, UI/debug) funnels through
`LLRender::flush` → `drawArrays`. Known leaks: glTF path ~30 raw-GL sites, probe
managers — bounded, addressed at parity phase.

**Frame anatomy** (`display()`, llviewerdisplay.cpp:487): cull/sort →
`generateSunShadow` :941 → `renderGeomDeferred` :1148 → `renderDeferredLighting`
:1170 → `renderGeomPostDeferred` :1494 → `renderFinalize` :1642 → `render_ui`
:1183 → `swapBuffers` :1721.

**Uniform corpus** (227 .glsl; class1=196/2=10/3=19): ~876 loose default-block
uniform sites collapse to **~225 unique names**, steep power law (1 name ≥50
shaders … 143 names in exactly 1). Tier by **update frequency**: FrameUBO
(once/frame: sun/moon/sky/gamma/screen), TransformUBO (per-draw matrices, ring
buffer), Material/tail (per-object). Samplers are descriptors, not UBOs. The
engine consumes this via the offline transform; H3 proved the mechanism.
Vulkan-GLSL *forbids* default-block uniforms (v1's H2b finding, glslc-confirmed)
— the transform is a hard prerequisite and is already proven.

**Shader assembly:** the viewer stitches shaders at load — `#version` injection,
36-way multi-object attach, `#include`/`#define` features
(`LLShaderMgr::attachShaderFeatures`, llshadermgr.cpp). The offline pipeline must
replicate this flattening before shaderc sees a translation unit (v1 had this
working for the GL-SPIR-V route; the flattener concept is proven).

**Geometry:** `LLDrawable` → `LLFace` → `LLVertexBuffer`, typemask-driven
interleaved buffers (`MAP_VERTEX|MAP_NORMAL|…` → offsets) — maps 1:1 onto wgpu
`VertexBufferLayout`. Draw pools/`LLSpatialGroup` tag batches by material/pass.

**Resource frictions** (v1's list, each with a known shape): mutable textures
(discard-level re-spec → immutable image + blit), vertex CPU-shadow +
`glBufferSubData` (becomes persistent-mapped staging — Vulkan *inverts this in
our favor*), N-frame deferred-free lists (→ real fences; keep the 64/frame
delete throttle lesson), `data_mask` permutations (→ vertex-input states; the
per-mask VAO cache is a ready-made inventory), render targets (→ passes with
explicit load/store; no MSAA — AA is FXAA/SMAA shader-side).

**Window:** one-shot `SetPixelFormat` + WGL on the HWND
(llwindowwin32.cpp:1454); `getPlatformWindow()` → HWND. **One window, one API
per run** — the restart toggle (the shipped Zink-toggle UX) picks GL or engine
at launch; in engine mode the GL context is simply never created and the HWND
goes to wgpu.

**Linking:** two Rust staticlibs cannot co-link (llrust-j2c/PLAN.md:143 —
duplicate libstd lang items). The renderer crate is therefore a **cdylib**
(`fs_render.dll`), runtime-loaded only when the toggle is on — the
DLL-subdirectory pattern the Zink toggle already ships. `LLRust.cmake` needs a
cdylib packaging path (new work, bounded).

## Architecture: the bridge

```
┌──────────────── viewer (C++, stock GL path intact) ────────────────┐
│ cull → sort → LOD → batch  (unchanged, stays C++)                  │
│        │                                                           │
│        ▼ tap at the narrow seam (engine mode only)                 │
│  LLVertexBuffer draw sites (3) ── batch: vtx bytes + typemask +    │
│  LLRenderTarget pass bounds ────── indices + textures + matrices + │
│  LLTexUnit binds ───────────────── pool/pass tag                   │
└────────┬───────────────────────────────────────────────────────────┘
         ▼ C-ABI FFI, command-stream batched per pass (never per draw)
┌─────── fs_render.dll (Rust cdylib, the proven engine) ─────────────┐
│ shadow cascades (M-series) → G-buffer → lighting (H4) → sky (EEP)  │
│ → post → present (wgpu/Vulkan on the viewer's HWND)                │
└────────────────────────────────────────────────────────────────────┘
```

- The viewer keeps everything upstream of the GPU: culling, sorting, LOD,
  rigging, batching, object management. The engine consumes **tagged batches**
  and renders them with its own proven passes. Pool/pass tags map onto engine
  passes (simple/bump/alpha/rigged/…).
- FFI follows the house pattern (opaque handle + POD views + free; bounds-checked;
  no panics across the boundary), batched per pass — cheap because draws are
  already centralized (v1's insight, kept).
- UI/immediate-mode: a bounded `gGL.begin/end` shim over the engine (vertex
  accumulation + one textured-quad pipeline, imgui-style). Must exist by
  bring-up or engine mode boots without a HUD. Distinct, under-scope-prone
  surface — tracked as its own deliverable.
- EEP owns the sun/sky inputs in both modes; the engine's sky/shadows already
  consume EEP-shaped parameters (H4d; M-series). RenderSettings mapping
  (RenderShadowDetail, ResolutionScale, split exponent, SSAO, DoF, LOD, exposure,
  FSAA…) is the parity-phase contract: the engine responds to at least the same
  render settings stock Firestorm does.

## Phases (each independently shippable; viewer GL never at risk)

- **Phase 0 — this document.** Strategy + decisions locked (bridge, restart
  toggle, cdylib). ✅ with this commit.
- **Phase 1 — offline shader sweep** (parallel with everything). Replicate the
  `attachShaderFeatures` flattening offline; run the H3-proven UBO + sampler
  transforms across the corpus by tier (Frame/Transform/Material per the corpus
  analysis). **Gate: 227/227 assembled translation units compile clean through
  shaderc → SPIR-V (Vulkan target), `spirv-val` clean.** Zero viewer changes.
- **Phase 2 — scene-dump bridge, offline.** Instrument the tap sites to
  serialize one live frame's draw stream (engine-mode data, GL still rendering);
  replay the dump in the harness headless. **Gate: the engine renders a real sim
  frame; screenshot-diff vs the GL frame of the same scene (SSIM/threshold), with
  Zink available as a same-box Vulkan-vs-Vulkan cross-oracle.** The parity
  harness this creates is the permanent regression gate for everything after.
- **Phase 3 — in-viewer bring-up.** `fs_render` cdylib + cmake packaging; the
  restart toggle; engine mode skips GL context creation and hands the HWND to
  the engine; live per-frame stream through the Phase-2 bridge; the UI shim.
  **Gate: viewer boots in engine mode, world + HUD render, A/B restart toggle
  against stock GL.**
- **Phase 4 — parity + settings.** Pass-by-pass screenshot gates vs GL (water,
  avatars/rigged, particles, glow, probes, glTF); class1/2/3 tier behavior; the
  **RenderSettings surface** wired to stock settings; the glTF/probe raw-GL
  leaks routed through the tap.
- **Phase 5 — perf + flip.** Perf gates on real scenes vs GL (busy-region,
  23-av crowd spot); default-on for capable GPUs; GL fallback retained
  indefinitely; Zink remains the compatibility bridge for exotic cases.

## Risk register (v2)

| Risk | Mitigation |
|---|---|
| cdylib packaging unproven (cmake/FFI template is staticlib-shaped) | Bounded new work in LLRust.cmake; Zink toggle already ships the runtime-DLL pattern |
| `attachShaderFeatures` replication drifts from the viewer's real assembly | Diff assembled sources against a viewer-side dump of what it actually links (instrument once, compare bytes) |
| UI/immediate-mode shim under-scoped | Own deliverable with its own gate (HUD renders); ~155 funneled sites, one flush path |
| FFI chatter kills perf | Batched per pass (v1 design, kept); draws already centralized at 3 sites |
| Mutable-texture semantics (discard levels) | Immutable image + blit; LOD-clamp via image-view mip range (v1 list) |
| Scene-dump parity diff drowns in incidental deltas (AA, dither, anisotropy) | Normalize settings for the diff; SSIM + per-pass masks, not raw memcmp; Zink cross-oracle |
| Engine feature gaps discovered late (water, particles, rigged mesh) | Phase 2's real-frame replay surfaces them early, offline, before any in-viewer risk |
| Scope / solo bandwidth | Every phase shippable; GL never removed; harness gates make progress durable across sessions |

**Invariants:** the viewer's GL path stays stock and shippable at every commit.
No in-viewer render surgery. Every crossing (data formats, shader transforms,
pass behavior) is oracle-gated before it ships. `pre-reset-backup` is read-only
history.
