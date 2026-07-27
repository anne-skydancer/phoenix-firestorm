# Alpha Transparency → Order-Independent Transparency (WBOIT) — Plan

Branch `fs/alpha-oit` off the clean LL-engine baseline `85e74cfc69`. Isolated worktree
so the decode-track (`fs/rust-j2c`) build is undisturbed.

## Problem (measured, not band-aidable)

Firestorm's alpha BLEND is a sorted forward pass with **three independent sort tiers**,
none of which compose into a correct global order:

- **Tier B — group sort, every frame:** `std::sort(... CompareDepthGreater)` on group
  `mDepth` — `pipeline.cpp:3906`; rigged groups by attachment order `:3909`.
- **Tier C — per-face sort WITHIN a group, LAZY:** `std::sort(faces, CompareDistanceGreater)`
  — `llvovolume.cpp:6634`, run **only on geometry rebuild**. Rebuild is triggered only when
  the group's view-angle shifts > **0.64** dot — `llspatialpartition.cpp:672`. Until that
  trips, intra-group face order is frozen as the camera moves → **this is the visible pop.**
- **Tier A — per-face distance value:** `facep->mDistance = center · cam.atAxis` —
  `lldrawable.cpp:924` (refreshed each stateSort, but consumed lazily by Tier C).
- Rigged/animesh alpha: attachment order, NOT depth (`CompareRenderOrder`,
  `llspatialpartition.h:252`; `llvovolume.cpp:6618-6624`).

Two sort levels don't compose; per-face granularity can't order interpenetrating /
concave / mutually-overlapping transparency; nothing orders fragments within a triangle.
Batching is DISABLED for alpha to preserve per-face order (`llvovolume.cpp` alpha path) →
one draw call per alpha face = a real perf sink. **Fundamental. OIT is the fix.**

## Decision (committed 2026-07-25)

**WBOIT first** (weighted-blended OIT, McGuire/Bavoil). Then **MBOIT** as a quality
upgrade on the SAME infra. **PPLL is OFF the table** until the Vulkan backend (zero
compute/SSBO usage exists today; unbounded per-pixel memory is bad for SL overdraw).

WBOIT wins now because all the primitives already exist — it's shader + one target +
a composite pass, not a new subsystem:
- MRT attach mechanism exists (`addColorAttachment`, `pipeline.cpp:403-408`); the G-buffer
  already uses 3-4 attachments.
- `glBlendFunci` (per-attachment blend) is loaded but UNUSED (`llgl.cpp:1985`).
- Full-screen resolve/composite chain exists (`renderDeferredLighting`/`renderFinalize`).

## Current alpha flow (the terrain WBOIT rewrites)

- Alpha is a **forward, post-deferred** pass, after the G-buffer resolve, into
  **`mRT->screen`** (a SINGLE `GL_RGBA16F` attachment, shared depth with the G-buffer;
  `pipeline.cpp:965-980`). The G-buffer MRT is already resolved and NOT bound during alpha.
- Two pools split by the water plane: `POOL_ALPHA_PRE_WATER` / `POOL_ALPHA_POST_WATER`
  (`pipeline.cpp:487-488`).
- `LLDrawPoolAlpha::renderPostDeferred` (`lldrawpoolalpha.cpp:142`) → `forwardRender`
  (`:232`) → `renderAlpha` batch loop (`:583`). Blend set at `:249-255`
  (`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`; alpha `ZERO, ONE_MINUS_SRC_ALPHA`); depth test on,
  write off (except rigged prepass / pre-water / impostor).
- Every alpha frag shader writes a SINGLE `out vec4 frag_color`
  (`class2/deferred/alphaF.glsl:317`). ~dozen programs: alpha (BP) + skinned + avatar +
  impostor, PBR alpha, fullbright alpha, material[SHADER_COUNT*2], emissive/glow, HUD variants.
- Emissive/glow = a SECOND additive pass (`BF_ZERO,BF_ONE,BF_ONE,BF_ONE`,
  `lldrawpoolalpha.cpp:867`) that already declares "additive so sorting doesn't matter"
  → composes cleanly with OIT.

## WBOIT execution order

### (A) Test-scene harness FIRST — the validation tool  ← START HERE
No hash-test applies (the GOAL is to CHANGE output to be *correct*). Chosen form:
**in-code synthetic debug geometry** — a `RenderDebug`-style toggle that draws a fixed set
of transparent test primitives in a known world configuration, camera-controllable,
identical every run:
- **Interpenetrating**: two translucent quads crossing through each other (no single
  per-face depth can order them).
- **Stacked**: N parallel translucent planes front-to-back (correct back-to-front
  accumulation test).
- **Concave / self-overlapping**: one translucent object whose own faces overlap from
  some angles.
- **Mutually-overlapping groups**: two separate translucent objects in different spatial
  groups (Tier-B inter-group test).
- **Pop repro**: geometry positioned so a slow camera pan trips the 0.64 rebuild → visible
  pop on the CURRENT build (the "before" reference).
Deliverable: the toggle + captured current-broken reference at the pathological angles.
Validation after WBOIT = the same toggle, hand-verified correct back-to-front look.

### (B) Infra
- Allocate a WBOIT target: **accum (RGBA16F)** + **revealage (R8/R16F)** attachments
  (on `mRT->screen` or a new pack member), sharing the G-buffer depth (test on, write off).
- Enable per-attachment blend: accum `ONE, ONE`; revealage `ZERO, ONE_MINUS_SRC_COLOR` via
  `glBlendFunci` (wire it into LLRender — loaded, unused today).

### (C) Shaders
- Add WBOIT output variants of the ~dozen forward alpha shaders: compute the McGuire
  weight `w(z, a)`, write `frag_data[0] = vec4(color.rgb * a, a) * w` (accum) and
  `frag_data[1] = a` (revealage). Keep the alpha-test `discard` (MINIMUM_ALPHA=0.004,
  `lldrawpoolalpha.cpp:64`).

### (D) Composite + delete the sort
- Composite pass near `pipeline.cpp:9878`: `color = accum.rgb / max(accum.a, eps)`,
  `out = mix(color, dst, revealage)`; blend over the lit scene.
- Re-point `LLDrawPoolAlpha` at the WBOIT target; **DELETE Tiers B/C/A**
  (`pipeline.cpp:3906`, `llvovolume.cpp:6634`, `llspatialpartition.cpp:663-699`) and
  **re-enable alpha batching** (the perf bonus).

### (E) Special cases
- **Glow/emissive**: keep the additive second pass (already order-independent); accumulate
  into the OIT accum or a separate additive target.
- **Water**: preserve the pre/post-water split + fog interaction (`renderGeomPostDeferred`
  interleaves water haze `pipeline.cpp:4418-4434`).
- **DoF**: alpha currently feeds a depth pass (`lldrawpoolalpha.cpp:212-229`) — WBOIT has no
  single depth; decide a representative depth (nearest? weighted?) for DoF.
- **HUD**: HUD alpha path (`sRenderingHUDs`, pre-water only) — likely keep sorted/simple
  (few layers) or a separate small OIT.
- **Impostors / avatar**: impostor uses depth-write; keep its own handling.

MBOIT later = swap the accum target for moment targets + moment math in the SAME shaders/
composite scaffolding. No new infra.

## Risks
The infra is low-risk (all primitives exist). The REAL risk is **matching Firestorm's
look** — fog, water haze, glow, DoF, tone-mapping all interact with the alpha result.
Validation is visual against the harness + real scenes (sky/water/glow/DoF/crowds), not a
hash. Watch: HDR range in accum (RGBA16F), weight function tuning for SL's heavy overdraw,
and the emissive/DoF/water seams.

## Validation
- The (A) harness at every stage.
- Full visual regression: interpenetrating/stacked/concave test set + real scenes (busy
  region, water edge, glow-heavy, DoF on, HUD, avatar crowd) vs the baseline build.
- Perf: fps at a crowd spot before/after (batching re-enable should be a net win).
