# Ground-Up Vulkan Engine — Phased Plan

**Status:** proposal, awaiting approval. Supersedes the GL-tap deferred bridge.
**Date:** 2026-08-01.
**Method:** agent assessment against upstream-as-spec; every phase cited to real code.

## The contract (the user's words)

> An engine that renders a Second Life scene top-to-bottom through **Vulkan** (not OpenGL), using **SPIR-V** as the shader language, that **accepts ALL settings** in the Firestorm viewer and **honours them.**

The contract is *implicit in the upstream engine*: **upstream FS/LL is the spec.** Our job is to reproduce its behavior on a different backend — no subset. `RenderDeferred` and `WindLightUseAtmosShaders` are deprecated and force-true (`pipeline.cpp:1229-1230`), so the deferred/PBR path is the *only* path — no forward fallback to support.

## Why the GL-tap is being retired

The engine sits **downstream of a tapped GL draw stream** through a fake GL that reflects shaders from text. It reconstructs a renderer from data that already threw its structure away, so honoring a setting means re-deriving it back out of the stream by hand. Every feature fights the pipe. The deferred resolve we wired was unphysical ("inside brighter than outside") for exactly this reason: the tap **never ships the local-light list** and crushes the atmospheric model into 12 floats.

## Keep / Cut / Replace

- **KEEP (input-agnostic, proven):** wgpu device+surface bring-up (`lib.rs:58-110`), the deferred pass *shape* (`flush_clear`), render targets + reverse-Z, the texture subsystem + VRAM-budget LRU (already prevented a WDDM paging storm), pipeline cache, ring UBO, the FFI house pattern, and the **headless harness** verbatim (it caught a deploy-breaking pipeline bug the eyeball missed).
- **RE-KEY (don't discard):** the upload-once geo cache, but keyed by `LLVertexBuffer` identity, not the raw CPU-shadow pointer.
- **CUT ("the ground"):** the null-GL stub + runtime shader reflection, and tapping GL *draw calls*.
- **REPLACE the input:** bridge the **DATA** — the viewer hands the engine a typed scene + settings; the engine **owns** the whole Vulkan frame.

## Honoring ALL settings = mirroring four mechanisms

The settings study found settings reach the GL renderer by **four** distinct mechanisms; the engine must mirror all four (this is the concrete meaning of "honor all settings"):

1. **Cached statics snapshot** — `LLPipeline::refreshCachedSettings` bulk-reads ~90 keys into statics on any wired change (`pipeline.cpp:1197-1333`, wiring `575-684`). → One `SettingsSnapshot` struct, atomically swapped; render code reads the snapshot, never the store.
2. **`LLCachedControl` uniforms** — per-frame post/lighting values (exposure, tonemap type, dynamic-exposure, local-light count). → Fold into the same snapshot as uniform inputs.
3. **Shader `#define` permutation** — `SUN_SHADOW`/`SSR`/`HERO_PROBES`/`TERRAIN_*` baked at shader link (`llviewershadermgr.cpp:817-863`); a change triggers a full `setShaders()` rebuild. → SPIR-V **specialization constants** / discrete pipeline objects, rebuilt on change.
4. **Render-target reallocation** — settings that resize FBOs go through `handleReleaseGLBufferChanged`/`handleShadowsResized`/etc. → An RT-realloc entrypoint keyed on the same settings (the engine already owns its targets).

**Effective-value layer:** `LLFeatureManager` can force-disable settings regardless of the saved value — the bridge resolves and ships **effective** values viewer-side, so the engine never reads raw `gSavedSettings`.

## The gated migration principle (no big-bang, no dead stretch)

The typed data-bridge stands up **beside** the tap. Through P0–P11 the tap still renders whatever hasn't migrated; `pipeline.cpp`'s GL passes stay no-op'd through the stub. Each subsystem phase **flips the tap's suppression** for its class the moment the engine renders it natively (or it double-renders). `pipeline.cpp` / the stub are removed only at the **very last** phase. **The screen is never blank.** Most verification rides the surfaceless headless harness; in-world parity is Vulkan-typed-path vs Vulkan-tap-path (same run) or against a **separately-run stock-GL screenshot** (one-API-per-run HWND forbids same-run GL/Vk A/B).

## The phases

| # | Phase | Delivers | Gate |
|---|---|---|---|
| **P0** | Parallel typed-bridge scaffold beside the tap | Empty typed channel co-exists in the same frame; tap renders 100% | Harness unchanged; login pixel-identical |
| **P1** | Camera block + Settings snapshot | Engine owns camera (reverse-Z from near/far) + atomic snapshot (mech 1&2) | N·L test fed from the typed path, not baked mvp |
| **P2** | Typed EEP sky + water blocks | Real atmospherics from `LLSettingsSky/Water` (kills the 12-float bottleneck) | Sky/water color oracle; A/B vs stock-GL ref |
| **P3** | Frame-level light array | The local-light list the tap never sent → **fixes "inside brighter than outside"** | Point-light attenuation test; indoor A/B |
| **P4** | Terrain (first whole subsystem off GL) | Native terrain; first tap-suppression flip; first SPIR-V specialization | Synthetic patch A/B (both paths, same run) |
| **P5** | Sky + water geometry | Dome/water geometry native; honor `RenderTransparentWater` (RT realloc) | Oracle + transparent-water toggle |
| **P6** | Opaque / PBR / legacy materials (the bulk) | G-buffer fed **real typed materials**, not scraped GL color | Material-parity tests; populated-region A/B |
| **P7** | Sun + spot shadows (CSM) | Honor the `RenderShadowDetail` family (mech 3&4 on real geometry) | Shadow-cast test; detail 0/1/2 sweeps |
| **P8** | Post chain | SSAO, tonemap/exposure, DoF, FSAA, glow — exact operator match | Tonemap oracle; settings sweeps |
| **P9** | Reflection probes + SSR + hero mirrors | Cubemap-array IBL, planar mirrors, screen-space reflections | Probe-reflection test; on/off A/B |
| **P10** | Rigged avatars + impostors | Avatars from typed skinning; impostor/cloth features | Palette-parity; posed-avatar A/B |
| **P11** | Particles + UI/HUD | Last GL-coupled surfaces; tap now emits nothing | Particle/UI color + legibility |
| **P12** | Retire the tap + null-GL stub | **Contract fulfilled:** engine owns the whole Vulkan frame; no GL, no stub, no tap | Full suite + full in-world sign-off |

Dependency spine: P0 → P1 → {P2, (P4, P6)} → P3 → P5 → {P7, P8, P9} ; P10/P11 off P6 ; **P12 depends on everything.**

## Decisions needed from you

1. **SPIR-V shader source** — *port GLSL via glslc* vs *author fresh*. **Recommendation:** port the ~227 LL/FS GLSL shaders through glslc for the **parity-critical** passes (deferred resolve, atmospheric integral, tonemap/exposure) — re-authoring that math is exactly where the retired resolve went unphysical. Author fresh only where the GL shape is hostile (fixed-function terrain multitexture, particle quads). Per-pass, not project-wide. Also: permutation settings → specialization constants vs pre-compiled pipeline objects.
2. **Salvage vs clean-slate per subsystem** — recommendation above (keep the input-agnostic core, re-key the geo cache, cut the stub/tap).
3. **Co-existence budget** — keep the tap live beside the typed feeders through P0–P11 (pipeline.cpp no-op'd until P12) so the screen is never blank. Cost: the no-op GL traversal still runs each frame (cheap, not free). Confirm vs a riskier earlier hard cut.
4. **SettingsSnapshot granularity** — one atomic snapshot for uniform settings (mech 1&2) + separate change-triggered entrypoints for shader-permutation rebuild (mech 3) and RT-realloc (mech 4), mirroring the viewer's own handler split. Confirm vs a monolithic dirty path.
5. **Effective-value layer** — bridge ships `LLFeatureManager`-resolved effective values; engine never reads raw settings. Confirm.
6. **Culling/LOD ownership** — LOD selection + octree culling + occlusion stay scene/CPU-side; the engine accepts an already-resolved draw list + LOD and honors only the far-clip plane. Confirm.

## Risks (with mitigations baked into phase ordering)

- **One-API-per-run HWND:** no same-run GL/Vk A/B; parity is Vulkan-typed vs Vulkan-tap (same run) or an offline stock-GL screenshot; the headless harness sidesteps it and carries most verification.
- **Long-dead-stretch:** the genuine blank-screen risk — mitigated by never touching `pipeline.cpp` until P12; the tap always renders the unmigrated remainder.
- **Double-render:** any subsystem both typed-fed and still tapped draws twice — the tap-suppression flip is part of every subsystem phase's definition of done.
- **Re-sourcing GL-only values (P6):** indexed-texture channel, MASK cutoff, per-batch material uniforms exist only as resolved GL state — extract the equivalents from the viewer's material/face structures upstream (more code; a few values traced back to the pool that computes them).
- **Parity-math trap:** prefer glslc-ported GLSL for resolve/atmospherics/tonemap; gate with the harness N·L/point-light/tonemap oracles before the eyeball.
- **Scope ("ALL settings" is unbounded):** gate progress **by subsystem**, never by enumerating flags, or the tail of rare flags never converges.
