# M5 — GL Backport Plan: port the proven engine shadow design into the viewer's stock CSM

Backport the `fs/engine-shadows` design (stable frusta + unified partition, M0–M4, all
gate-verified) into Firestorm's stock GL cascaded shadow maps. Conditional trigger met: M4
passed. **Needs in-world testing (user) — the viewer's shadow code regressed 3× on prior
single-lever attempts; this only works if both changes land together and are tested in-world.**

Source of truth = the engine: `indra/rust/vkharness/src/shadow_engine.rs` on `fs/engine-shadows`
(`stable_light_matrix`, `compute_cascades`, the shader selection). Reference commits: M2
`d8048bc89d`, M3a `8c181352c8`, M4 `27b7544817`.

## The viewer's current CSM (scouted 2026-07-30) — and the seam

All in `indra/newview/pipeline.cpp`; shader `app_settings/shaders/class1/deferred/shadowUtil.glsl`.
Stock Linden, identical across branches.

- **Splits** (`11248-11292`): angle-blended power-law over a camera-space near/far AABB → four
  view-space depths in `mSunClipPlanes` (`[0]` padded ×1.25). Uploaded as `shadow_clip`
  (`9532`). *The split SCHEME is fine — keep it.*
- **Fit** (`11384-11620`): each cascade fit to the frustum-slice **point cloud** → ortho *or*
  least-squares **warped-perspective**. **NO bounding sphere, NO texel-snap.** This is the
  rotation-variant + crawling source. (`shadowUtil.glsl:60` adds a per-fragment X jitter to
  *disguise* crawl, not prevent it.)
- **Caster cull** (`11332-11340`, `10671`): casters rendered into cascade j = those inside a
  **diverging-ray perspective slice** `[dist[j]*0.75, dist[j+1]*1.25]` (`shadow_cam` →
  `updateCull`). `getVisiblePointCloud` (`10839`) takes `light_dir` but **ignores it** — no
  sun-ward extension.
- **Selection** (`shadowUtil.glsl:113-184`): fragment's **planar view-z** `spos.z` vs
  `shadow_clip*{-0.75,-1.25}`, with a `transition_domain` blend across neighbours.
- **Layout** (`1097-1106`): four **separate** depth `sampler2DShadow` targets `mRT->shadow[0..3]`
  (NOT an array), screen-derived res, `GL_LEQUAL` compare. Matrices `mSunShadowMatrix[0..5]` →
  `shadow_matrix[6]` (`9487-9498`).
- **THE SEAM (confirmed):** selection (planar view-z) and caster-cull (diverging perspective
  frustum + an extra `*0.05f` skew at `11335`) use the *same* `mSunClipPlanes` scalars as
  *different geometry*. They agree on the frustum centerline, diverge off-axis and under camera
  rotation → a receiver selects cascade j+1 whose caster was only rendered into j → dropout.

## The two changes (must land TOGETHER — this is the whole lesson of the prior failures)

### (a) Stable fit — replace `pipeline.cpp:11384-11620`
Port `stable_light_matrix`: for each cascade j, take the split-slice corners (already built at
`11332-11340`), compute their **bounding sphere** (center, radius), build a light-space ortho
sized to `2*radius` (rotation-invariant → no sweep), and **texel-snap** the light-space origin
to the shadow-map texel grid (`units_per_texel = 2*radius / shadow_res`; floor the light-space
x/y to that grid → no crawl). Write `view[j]`/`proj[j]` at `11647-11649`; keep the
`trans*proj*view*inv_view` composition (`11649`). **Delete the warped-perspective path** (it
exists to claw back grazing-sun resolution; sphere-bound trades some density for stability — see
Risks). Keep `mSunClipPlanes`/`mShadowModelview`/`mShadowProjection` outputs so downstream + the
debug overlay stay valid.

### (b) Unified partition — caster inclusion == selection partition
Make the *rendered-into* volume for cascade j match what the shader *selects* into j:
1. Caster volume = the stable light-space ortho box from (a), **extended toward the sun**
   (pancake the near plane toward the light) so off-slice casters aren't culled. This replaces
   the diverging `dist[j]*0.75 / dist[j+1]*1.25` perspective slice at `11332-11340`. Use the
   `light_dir` that `getVisiblePointCloud` (`10839`) already receives-but-ignores.
2. One partition on both sides: the shader keeps selecting by planar view-z against
   `mSunClipPlanes` (`shadowUtil.glsl:116-117`); the C++ caster box is the sphere of that SAME
   view-z slice extended sunward. Because both derive from the identical view-z bands, a
   receiver in band j always samples a cascade whose casters cover band j → **no missing
   caster, by construction** (exactly the engine's guarantee). Drop the asymmetric 0.75/1.25;
   keep a symmetric overlap band for the `transition_domain` blend.

## Layout decision
**Keep the 4 separate `sampler2DShadow` maps** for the first backport (the fit + partition are
the fix; storage is not). The engine uses a texture array, but converting the viewer to
`sampler2DArrayShadow` touches allocation (`1097-1106`), compare-mode setup (`1142-1154`), the
`shadow_matrix[6]` upload, and every `shadowMapN` sampler in the deferred shaders — a much larger
surface for no correctness gain. Defer the array conversion.

## Risks (from the scout — respect these)
- Prior **single-lever** edits regressed (iter1 widened overlap → worse; iter2 extended
  `max.mV[2]` → warped-perspective corruption + rotating wave). **Ship (a)+(b) together or not
  at all.**
- The fit feeds `mShadowError`/`mShadowFOV`/`mShadowFrustOrigin` + the `RENDER_DEBUG_SHADOW_FRUSTA`
  overlay — keep them consistent or the debug draw reads stale state.
- Sphere-bound ortho = some texel-density loss vs the warped-perspective at grazing sun. Expect
  to tune `RenderShadowResolutionScale` / cascade count.
- `gCubeSnapshot` path stretches `mSunClipPlanes` and runs only **2 cascades** (`11283-11288`) —
  the unified change must keep reflection-probe shadow passes valid.

## Branch + staging
- Branch: reset the banked **`fs/shadow-coverage`** to stock (its `SHADOW_COVERAGE_PLAN.md` +
  2 failed iters are reference only), or a fresh `fs/shadow-backport` off the same viewer
  baseline. Viewer main branches stay untouched.
- Implement (a)+(b) together. Build (`cmake --build build-vc170-64 --config Release --target
  firestorm-bin`, viewer CLOSED). **Do NOT commit until the in-world test passes** — this is the
  code path that regressed 3×.

## In-world test (user — the acceptance)
Reproduce the exact legacy failure and confirm it's gone:
1. Load a scene with a clear caster on flat ground (the curved-sofa scene from the original
   captures, or any object). Set a **low sun** (long shadow = stress).
2. `RENDER_DEBUG_SHADOW_FRUSTA` (Develop → Render Metadata → Shadow Frusta): the cascade frusta
   should **no longer sweep/refit** wildly on camera rotation (the engine's no-sweep property).
3. Slow orbit / 1° z-pan: the cast shadow must **not cut or flicker** (the whole spec). Capture
   with `c:/fs/clip2frames.py` and inspect frames the way the original dropout was filmed
   (`r:\pictures\debug images` has the reference "before" clips).
4. Compare against stock (installed Firestorm) SBS on the same box/scene — stock flickers, the
   backport should be continuous.
PASS = shadow stays continuous through the orbit at low sun, frusta don't sweep. Then commit.

## Mapping: engine → viewer
| engine (`shadow_engine.rs`) | viewer injection |
|---|---|
| `stable_light_matrix` (sphere + texel-snap) | replace fit `pipeline.cpp:11384-11620` |
| view-z slice corners | `frust[]` `11332-11340` (reuse; bound with sphere) |
| toward-sun caster extension | `getVisiblePointCloud` `10839` (`light_dir` extension) + `shadow_cam` frustum |
| `pick_cascade` (view-z vs splits) | `shadowUtil.glsl:113-184` (keep; drop 0.75/1.25 asymmetry) |
| unified `cascade_splits` array | `mSunClipPlanes` (`11273-11288`) = sole split source, both sides |
| texel-snap grid | new, in the fit; `shadow_res` from `mRT->shadow[j]` alloc |
