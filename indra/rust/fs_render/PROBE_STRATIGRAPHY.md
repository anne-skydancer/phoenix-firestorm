# Reflection-Probe Subsystem — Stratigraphy & Faithful-Reproduction Plan

> Holistic excavation of the WHOLE reflection-probe subsystem in stock Firestorm
> (`c:\fs\firestorm-upstream`), then the plan to reproduce it faithfully in
> `fs_render`. Method (canonical): (a) what does OGL do for X? (b) how does VK
> reproduce it accurately? OGL is the invariant. Verify against the oracle
> (`fs_ogl_ref` running the REAL shaders) per stratum. Do not treat a sliver as
> the whole.
>
> Sources: full read of `class3/deferred/reflectionProbeF.glsl` (914 lines) +
> three excavation agents (manager, cubemap-generation, pipeline-integration).

---

## 0) What the subsystem IS

Image-Based Lighting (IBL). The renderer captures the scene into cubemaps at
probe positions, convolves each into a **radiance** map (glossy reflection, mip
chain = roughness) and an **irradiance** map (diffuse ambient), packs every
probe's spatial data into one std140 UBO, and the deferred resolve
(`softenLightF`) samples them per-fragment to add environment reflection + IBL
ambient on top of the sun/atmosphere lighting.

It is the source of BOTH remaining residuals in our resolve conformance:
- the ~10% non-classic gap = the **irradiance/ambient** term (`sampleProbeAmbient`), stubbed to 0 in our fixture;
- **environment reflections** (`applyGlossEnv`/`applyLegacyEnv`) = absent entirely from our resolve.

The subsystem decomposes into **five strata**. Strata 1–3 + 5-capture are CPU
(the two managers). Stratum 2-gen + stratum 4 are GPU shaders. The load-bearing
seam is the **UBO + dual-cubemap-array data contract** (§6): the C++ struct is
`memcpy`'d straight into the GLSL std140 block and MUST stay byte-identical
(`llreflectionmapmanager.h:61-63` carries an explicit warning).

---

## 1) STRATUM 1 — Placement & lifecycle (CPU: `LLReflectionMapManager`, `LLReflectionMap`)

Four probe kinds, all `LLReflectionMap`, all in one `std::vector<LLPointer<LLReflectionMap>> mProbes`; kind inferred from which of `mGroup`/`mViewerObject` is set:

| Kind | mGroup | mViewerObject | mPriority | Notes |
|---|---|---|---|---|
| **Default (sky/ambient)** | null | null | special | **Always `mProbes[0]`, cube layer 0, radius 4096, 64 m above camera.** Renders sky/WL-sky/water/voidwater/clouds/terrain only. The shader's fallback probe (index 0 = "smoothing"). |
| Automatic (spatial) | non-null | null | 0 | Region 32 m grid (`llviewerregion.cpp:1357`, 8×8=64/region) + spatial-group 16 m octree cells (`registerSpatialGroup`, node size 15–17 m). `autoAdjustOrigin` raycasts 8 AABB corners → centers in empty space, lifts above ground, grows radius. |
| Manual (object) | maybe | non-null | 1 | Prim flagged reflection probe (`registerViewerObject`, hacked into `setTE`). Live-tracks object position each frame. Box or sphere. |
| Terrain/water | null | null | 0 | near-clip 1 m. |

- **Box vs sphere:** a probe is a box only if a manual object probe with `getReflectionProbeIsBox()`. `getBox()` builds camera→unit-cube = `inverse(modelview · worldMatrix · scale)`. Box radius = half-extent magnitude; sphere radius = `scale.x·0.5`. Box signaled to the shader by **negating the priority** in `refIndex[i].w`.
- Default probe: distance pinned 64 m when complete, −4096 while incomplete (queue jump). `mComplete` remembered across re-alloc so a reset doesn't blank the sky (SL-20498).

## 2) STRATUM 2 — Capture & generation (CPU orchestration + GPU gen shaders)

### 2a. Scheduler (`update`/`doProbeUpdate`)
- **One probe captured across frames.** `mUpdatingProbe`/`mUpdatingFace` state machine advances one face/frame.
- **A full probe = 12 face renders**: 6 with the irradiance-pass regime, then 6 with the radiance-pass regime. This is davep's single-bounce trick: irradiance from a render with **no** probe irradiance fed back; radiance from a render that **includes** irradiance (`is_ambiance_pass = gCubeSnapshot && !isRadiancePass()` → `ambscale=0/radscale=0.5` on irradiance faces, `1/1` on radiance). Feedback-loop avoidance.
- **Realtime (mirror-ish) probes** (`RenderReflectionProbeDetail >= 2`): closest dynamic probe fully updated every frame (all 6 faces), alternating irradiance/radiance per frame.
- Priority: `check_priority` (incomplete→nearest; complete→oldest+nearest via `update_score = frameTime − lastUpdate − dist·0.1`; every 3rd count a complete probe may cut ahead). Default probe force-selected when overdue (`RenderDefaultProbeUpdatePeriod`, 2 s).

### 2b. Per-face render (`updateProbeFace` → `cubeSnapshot` → `display_cube_face`)
- Swap `gPipeline.mRT = &mAuxillaryRT`; render at **4× supersample** (`mProbeResolution·4`).
- 90° FOV square camera, 6 agent-space look/up dirs (`sLookVecs`/`sUpVecs`). Only the requested face per call.
- `display_cube_face` runs the **full deferred path**: cull → `generateSunShadow` (shadows ARE on) → `renderGeomDeferred` (G-buffer) → `renderDeferredLighting`. `renderFinalize` skipped. Avatars/particles excluded unless `dynamic`. Default probe: sky/water/terrain/clouds only.
- Probe-capture uniform `CUBE_SNAPSHOT=1` disables SSR inside captures; local lights skipped unless `probe_level>0`; sun uses `gDeferredSunProbeProgram`.

### 2c. Downsample (every face, `updateProbeFace:830-922`)
1. **Gaussian blur** on the supersampled capture — `gGaussianProgram`, 9-tap separable, weights `{.0002,.0060,.0606,.2417,.3829,.2417,.0606,.0060,.0002}`, H then V.
2. **Mip chain** — `gReflectionMipProgram` (passthrough) down `mMipChain` (len `round(log2(res))`=7@128), each real cube-mip `glCopyTexSubImage3D`'d into the **scratch** cube layer `sourceIdx = mReflectionProbeCount`.

### 2d. Convolution (on face 5 only)
- **Radiance pass** — `gRadianceGenProgram` (`radianceGenF.glsl`, GGX importance sample, Sascha Willems). Per output mip `i` (roughness = `i/max_probe_lod`, NOT the C++ `roughness` uniform which the shader ignores), per 6 cube faces (clip-space `sClipToCubeLookVecs/UpVecs`): `numSamples = max(32·roughness,1)`, per-sample source LOD = solid-angle ratio `0.5·log2(omegaS/omegaP)+1` clamped `[0,maxLOD]`, sampling the prefiltered scratch mip chain. → radiance array at `probe->mCubeIndex`, mip `i`.
- **Irradiance pass** — `gIrradianceGenProgram` (`irradianceGenF.glsl`, cosine-weighted, Khronos glTF). ONE mip (the 16-res level), 32 cosine samples, per-sample LOD = `0.5·log2(6·u_width²/(N·pdf)) + 2.0` clamped `[0,maxLOD]`, **`u_width` hardcoded 64**, TBN with **fixed bitangent (0,1,0)** (pole-robust branch disabled). → irradiance array, mip 0.

### 2e. Storage (the two arrays, `initReflectionMaps`)
- `mTexture` (**radiance**): `LLCubeMapArray::allocate(mProbeResolution=128, 3, mReflectionProbeCount + 2, mips=true, hdr)` → `GL_R11F_G11F_B10F`, mipped, **+2 scratch layers** (`sourceIdx` at `N`, realtime at `N+1`). Layers `mCubeIndex·6 .. +5` per cube.
- `mIrradianceMaps` (**irradiance**): `allocate(LL_IRRADIANCE_MAP_RESOLUTION=16, 3, mReflectionProbeCount, mips=false, hdr)`.
- `mMaxProbeLOD = log2(res)−1` (=6 @128).

## 3) STRATUM 3 — UBO packing (CPU: `updateUniforms`, `getReflectionMaps`)

`getReflectionMaps`: pick usable probes (`mCubeIndex!=-1 && !mOccluded && mComplete`), compute view-space depths (`mMinDepth = −oa.z − radius`, `mMaxDepth = −oa.z + radius`), sort by minDepth, assign `mProbeIndex`.

`updateUniforms` fills `ReflectionProbeData` (§6):
- **Bucket** (`refBucket[256]`): 1 bucket/metre of view-space depth 0–255. Bucket `i` = `mProbeIndex` of the **nearest** probe whose `[minDepth,maxDepth]` spans depth `i`. Only `[0]` written (rest sentinel). The shader's `getStartIndex(pos)` reads `refBucket[floor(-pos.z)]`.
- **refSphere** = view-space origin.xyz + radius.w. **refBox** = camera→unit-cube (box probes).
- **refParams** = `x` irradiance scale (`max(minimum_ambiance, getAmbiance())·ambscale`), `y` radiance scale (`radscale`), `z` fadeIn, `w` znear (`oa.z − radius`). `minimum_ambiance` = sky `getReflectionProbeAmbiance` (floor on every probe).
- **refIndex** = `[0]` cube layer, `[1]` neighbor start (as ivec4 index `nc/4`), `[2]` neighbor count (−1 none), `[3]` priority (sign = shape; neg = box).
- **Neighbors** (`refNeighbor[4096]`): contiguous per probe, ≤64/probe, skip occluded/uncubed, pad to ivec4 boundary.
- `refmapCount = count`; hero fields copied from `mHeroProbeManager.mHeroData` (§5); whole struct → `glBufferData(GL_STREAM_DRAW)`.

## 4) STRATUM 4 — GPU sampling (`reflectionProbeF.glsl`, 914 lines — the consumer)

The core our resolve must reproduce. Flow:

1. `getStartIndex(pos)` = `clamp(refBucket[clamp(floor(-pos.z),0,255)].x, 1, refmapCount+1)`.
2. `preProbeSample` / `sampleProbes`: walk from start over `refmapCount` probes + each hit's neighbor list; `shouldSampleProbe` tests sphere (`d²>r²` reject) or box (unit-cube reject; box suppresses automatics via `sample_automatic=false`). Probe 0 (default) always appended as the smoothing fallback.
3. **Parallax:** `sphereIntersect` (auto probes disable it via huge radius) / `boxIntersect` (Lagarde OBB-corrected). Both rotate the reflection vector by `env_mat`.
4. **Weight:** `sphereWeight` = inverse-distance × `1−(d−0.5r)/(0.5r)` falloff × coeff.
5. **Tap:** `tapRefMap` → `reflectionProbes` array, `lod=(1−glossiness)·max_probe_lod`; `tapIrradianceMap` → `irradianceProbes` array mip 0, then `mix(amblit, probe, ambiance)`.
6. **Combine:** `sampleProbes`/`sampleProbeAmbient` accumulate weighted, blend **automatic vs manual** probes.
7. **Public entry points:** `sampleReflectionProbes` (PBR: returns radiance+irradiance), `sampleReflectionProbesLegacy` (legacy: `ambenv` diffuse + `glossenv` + `legacyenv`), `sampleReflectionProbesWater`.
8. **Env application:** `applyGlossEnv` (PBR-ish gloss reflection + fresnel + fake energy conservation), `applyLegacyEnv` (legacy env-intensity reflection). Called in `softenLightF.glsl:266,274`.
9. **SSR mix:** `#if defined(SSR)`, `cube_snapshot!=1 && glossiness>=0.9` → `tapScreenSpaceReflection(sceneMap, sceneDepth)` ray-march of the **previous frame's** resolved color, `mix(glossenv, ssr.rgb, ssr.a)`.
10. **Hero:** `tapHeroProbe` (mirror) — blends `heroProbes` layer 0, gated by clip-plane distance + `glossiness>0.75` (only ¼ mips exist).

`doProbeSample` gates on `classic_mode==0` → `sampleProbeAmbient` (this is exactly the ~10% term our fixture zeroes).

## 5) STRATUM 5 — Pipeline integration & hero probes (CPU)

### 5a. Binding (`bindReflectionProbes`, `pipeline.cpp:10527`)
Per deferred shader: enable+bind `REFLECTION_PROBES` (radiance array), `IRRADIANCE_PROBES` (irradiance array), `HERO_PROBE` (if `RenderMirrors`); `setUniforms()` binds the UBO to `UB_REFLECTION_PROBES`; `setEnvMat` uploads the **3×3 modelview rotation** as `env_mat`; bind `SCENE_MAP`/`SCENE_DEPTH` (= `mSceneMap`, prev-frame color) + SSR tuning uniforms. `CUBE_SNAPSHOT` + `max_probe_lod` also pushed.

### 5b. Frame ordering (the stale-by-design contract)
- **Regular probes update AFTER `display()`** (`llappviewer.cpp:1804`). The faces `update()` generates are consumed **next frame** → regular probes are one frame stale (and a *full* probe is many frames old, 1 face/frame).
- **Hero probes update BEFORE the scene, same frame** (`llviewerdisplay.cpp:1083`): `mHeroProbeManager.update()` + `renderProbes()` run at the top of `display()` so the mirror matches the scene it reflects.
- Occlusion queries in the occlusion pass (before main deferred).

### 5c. Hero (mirror) probes (`llheroprobemanager`)
Real-time planar reflection for the single nearest front-facing mirror box (max 2, res 1024, no supersample, only ¼ radiance mips). Mirror-camera reflection across the face plane; `mCurrentClipPlane`. `heroBox`/`heroSphere`/`heroShape`/`heroMipCount`/`heroProbeCount` filled by its own `updateUniforms`, then **copied into the regular manager's UBO** (`llreflectionmapmanager.cpp:1295-1299`) so one UBO + one bind carries both. `tapHeroProbe` consumes it.

### 5d. SSR (`mSceneMap`)
`copyScreenSpaceReflections(mRT->screen → mSceneMap)` in `renderFinalize` (HDR path) — this frame's lit screen becomes next frame's SSR source (`!gCubeSnapshot`). Inline mix in `reflectionProbeF.glsl` + a standalone `screenSpaceReflPostF.glsl` pass.

### 5e. Settings gating
`RenderReflectionsEnabled` (master), `RenderReflectionProbeLevel` (0 off / 1 manual / 2 +terrain / 3 +objects), `RenderReflectionProbeDetail` (−1..2), `RenderReflectionProbeCount` (≤256), `RenderReflectionProbeResolution` (128, restart), `RenderScreenSpaceReflections`, `RenderMirrors` (hero). Per [[engine-mirrors-fs-render-settings]] our engine must respond to these.

---

## 6) THE DATA CONTRACT (locked — byte-match required)

`ReflectionProbeData` (`llreflectionmapmanager.h:64-101`) == `layout(std140) uniform ReflectionProbes` (`reflectionProbeF.glsl:45-82`), `MAX=256`:

```
mat4  refBox[256];      // camera→[-1,1] unit cube (box probes)
mat4  heroBox;
vec4  refSphere[256];   // xyz view-space origin, w radius
vec4  refParams[256];   // x irradiance-scale, y radiance-scale, z fadeIn, w znear
vec4  heroSphere;
ivec4 refIndex[256];    // x cube layer, y neighborStart(ivec4 idx), z neighborCount(-1 none), w signed priority (neg=box)
ivec4 refNeighbor[1024];// 4096 ints
ivec4 refBucket[256];   // depth→start-index LUT (only .x used)
int   refmapCount;
int   heroShape;        // 0 box, 1 sphere
int   heroMipCount;
int   heroProbeCount;
```
Cubemap arrays: **radiance** `N+2` cubes @128, mipped, `R11F_G11F_B10F` (HDR) / `RGB8`; **irradiance** `N` cubes @16, no mips. `env_mat` = 3×3 modelview rotation. `max_probe_lod = log2(res)−1`.

---

## 7) What `fs_render` has today, and the gap

Our `resolve.frag` is a faithful `softenLightF` port EXCEPT the probe terms:
- **`sampleProbeAmbient` (irradiance/IBL ambient) — stubbed to 0** → the ~10% non-classic residual.
- **`applyGlossEnv`/`applyLegacyEnv` (environment reflection) — absent.**
- SSR mix, hero — absent.

Everything upstream (bucket lookup, parallax, weighting, taps) is absent because the UBO + cubemap arrays aren't bound yet.

---

## 8) VK reproduction — the split (mirrors the materials/PBR approach)

The subsystem splits along the SAME seam we used for materials/PBR: **consume now (verify vs oracle with synthetic inputs), generate/capture later (geometry-coupled), lock the contract now.**

- **Consumption side** (stratum 4, the GPU sampling): portable NOW into `resolve.frag`, verifiable against the oracle by feeding a **synthetic UBO + synthetic cubemap arrays** — exactly as we fed synthetic G-buffers for materials/PBR. Closes the ~10% residual + adds gloss env. reverse-Z world-pos reconstruction lands here → **verify appliers/decals as an acceptance gate** ([[reverse-z-decals-careful]]).
- **Convolution generation** (stratum 2d, the gen shaders): portable NOW in isolation — feed a known captured cubemap, diff our GGX-prefilter + cosine-irradiance vs the oracle's real `radianceGenF`/`irradianceGenF`. No scene geometry needed.
- **Capture + placement + scheduling + UBO packing + pipeline ordering** (strata 1, 2a-c, 3, 5): genuinely geometry-coupled (needs the engine to render the full scene into cube faces + the frame loop). Lands with the **geometry stratum**. But its DATA CONTRACT (§6) is locked NOW so we never fight it later — the "get it right while it's cheap" principle.

This is faithful, not a sliver: the whole consumer is ported and verified; the whole generator math is ported and verified; only the capture/scheduling that *cannot* run without scene geometry is deferred, and its output format is nailed down in advance.

---

## 8a) P1 STATUS (2026-08-04) — DONE + VERIFIED (consumer side)

The GPU consumer is ported into `resolve.frag` (view-space, verbatim from `reflectionProbeF.glsl`
+ `pbrIbl`) and verified against the oracle (`fs_ogl_ref`, real shaders) with synthetic probe fixtures:

- **PBR IBL path — PIXEL-EXACT.** `PbrShiny` (iblSpec-dominant) with a filled probe → ratio **1.000,
  max Δ 0.000** in both regimes. Verifies `pbrIbl` split-sum `FssEss` + multiscatter compensation +
  the `brdfLut` (1024-sample genbrdflutF split-sum LUT, generated on CPU and bound to BOTH sides) +
  `sampleReflectionProbes` radiance → `iblSpec`.
- **Neutral-probe residual CLOSED.** Wiring the PBR + legacy branches through the probe path made the
  ~8–13% non-classic ambient residual (old `amblit` stub vs the oracle's `sampleProbeAmbient`) go to
  **pixel-exact**: `atmos_ab` pbr 1.085→**1.000**, `pbr_ab` pbr 1.132→**1.000 Δ0.000**.
- **Legacy probe path — matches to f16.** Single default probe + dual (walk + auto/manual mix +
  `sphereIntersect` parallax + cube-layer indexing): the probe *delta* (ambient via `sampleProbeAmbient`,
  gloss via `applyGlossEnv`) matches the oracle to f16; classic is byte-identical.
- **Only residual anywhere:** the pre-existing legacy `lightFunc` bilinear-LUT vs our closed-form
  (materials stratum, max Δ ~0.33 at the highlight, mean 0.006). Isolated — PbrShiny (also v-dependent)
  is pixel-exact, so it's the LUT, not the view vector or the probe port. A materials follow-up.

**Verification harness added:** `fs_ogl_ref` `ProbeFixture` (neutral / `default_probe` / `two_probe`),
`cube_array_layers`, `brdf_lut`, `probe_params_bytes`; `soften_pass`/`resolve_pass` take a `ProbeFixture`
+ bind the ReflectionProbes UBO / cube arrays / brdfLut / env_mat; `RefEngine::{soften,resolve}_probe_frame`;
`examples/probe_ab.rs`; oracle assembly gains `#define REFMAP_LEVEL 3` (harmless for ≤1 probe).

**NOT yet done (engine integration = P3):** `resolve.frag` SOURCE now declares bindings 5–11 (depth,
ReflectionProbes UBO, radiance/irradiance cube arrays, sampler, ProbeParams, brdfLut) but the engine
still runs the OLD committed `resolve.frag.spv` (5 bindings) — it stays green (`deferred_resolve_*` test
passes). Recompiling the spv + expanding `live.rs`'s `resolve_bgl`/bind to 12 bindings + feeding the real
depth buffer + real probe data happens with the manager port (P3), since the probe DATA has no producer
until then. reverse-Z pos reconstruction + the decal/applier acceptance gate also land at P3 integration.

## 8b) P2 STATUS (2026-08-04) — DONE + VERIFIED PIXEL-EXACT (generation math)

The convolution generators are ported to `fs_render/shaders/` (`gen.vert` shared + `radiance_gen.frag`
+ `irradiance_gen.frag`, wgpu-shaped: separate `textureCubeArray`+sampler, `GenParams` UBO) and verified
in isolation against the REAL `radianceGenF`/`irradianceGenF` (`fs_ogl_ref` `gen_pass.rs` + `examples/gen_ab.rs`,
`RefEngine::gen_frame`). Both sides sample an identical direction-varying source cube with an identical
per-pixel direction, so any per-pixel divergence is a fragment-math / transform bug.

- **Radiance prefilter — PIXEL-EXACT** across a mip sweep (mip 0/1/3/6 → output changes 0.597→0.563 as
  roughness drives sample count + spread): ratio **1.0000, max Δ 0.0000** at every mip. Verifies
  `importanceSample_GGX`, `D_GGX`, `computeLod`/solid-angle mip bias, roughness = mip/max_probe_lod, the
  sample-count scaling, and the "C++ `roughness` uniform is ignored" gotcha.
- **Irradiance convolution — PIXEL-EXACT**: ratio **1.0000, max Δ 0.0000**. Verifies the Lambertian
  cosine integral, `generateTBN` fixed bitangent (pole-robust branch disabled), `computeLod`, LOD bias
  +2.0, and the `u_width` hardcoded-64 gotcha.

**Isolation caveat:** the verification source cube is single-mip, so the per-sample *source-mip selection*
(the `textureLod` LOD into the prefiltered chain) clamps to mip 0 and isn't distinguished here — but the
formula is verbatim-ported and identical on both sides; it is fully exercised at P3 (real mipped scratch
cube from the Gaussian-blur → mip-chain downsample). The C++ ORCHESTRATION (Gaussian pre-blur, mip-chain
copy, per-mip/per-face draw loop, `sClipToCube*` face matrices, scratch→layer copy) is P3 (needs the real
render target + cube arrays + the capture loop).

## 9) Phased plan (each phase verified vs the oracle before the next)

### P1 — Lock the contract + port the GPU consumer (verify NOW) — ✅ DONE (see §8a)
1. Define the `ReflectionProbes` std140 UBO in `fs_render` (§6, byte-exact).
2. Bind two `CubeArray` textures (radiance + irradiance) + hero; add `env_mat`, `max_probe_lod`, `cube_snapshot` uniforms.
3. Port `reflectionProbeF.glsl` sampling core into a resolve-side module: `getStartIndex`, `preProbeSample`/`sampleProbes`, `shouldSampleProbe`, `sphereIntersect`/`boxIntersect`, `sphereWeight`, `tapRefMap`, `tapIrradianceMap`, `sampleProbeAmbient`, `sampleReflectionProbes`(+`Legacy`), `applyGlossEnv`/`applyLegacyEnv`, SSR mix (behind an SSR define), `tapHeroProbe`.
4. Wire into resolve: PBR path adds `sampleReflectionProbes`→`applyGlossEnv`; legacy path adds `sampleReflectionProbesLegacy`→`applyLegacyEnv`; `doProbeSample` gate on `classic_mode==0`.
5. **reverse-Z world-pos reconstruction** for the `pos.z` bucket + parallax world pos (GreaterEqual, clear 0). **Decal acceptance gate:** confirm coplanar appliers still render.
6. **Verify:** oracle-side, build a synthetic UBO (default probe + one manual box + one sphere) + synthetic radiance/irradiance cubemap arrays; run the REAL `reflectionProbeF.glsl` in `softenLightF` vs our resolve; per-pixel + mean diff. Target: closes the ~10% ambient residual; gloss env matches.

### P2 — Port the convolution generators (verify NOW, isolation) — ✅ DONE (see §8b)
1. `radianceGenF` GGX prefilter (32 samples·roughness, roughness=mip/maxLOD, solid-angle mip bias +1) as an engine pass over a source cube → mipped radiance.
2. `irradianceGenF` cosine convolution (32 samples, LOD bias +2, u_width=64, fixed TBN bitangent) → 16-res irradiance.
3. Gaussian pre-blur + mip-chain downsample (`gGaussianProgram`/`gReflectionMipProgram` equivalents).
4. **Verify:** feed a known HDR cube, diff our prefiltered mips + irradiance vs the oracle running the real gen shaders. Match the two known gotchas exactly (radiance ignores C++ roughness; irradiance hardcodes u_width=64).

### P3 — Capture + placement + scheduling + UBO packing (geometry-coupled; lands with geometry stratum)
1. Port `LLReflectionMapManager` placement (default sky probe [0]/layer 0/r4096/64 m; automatic 32 m region grid + 16 m spatial; manual box/sphere) + `autoAdjustOrigin`.
2. Scheduler: 12-render single-bounce (6 irradiance-regime + 6 radiance-regime), 1 probe/frame; realtime every-frame path.
3. Cube-face scene capture (engine renders scene → cube faces at 4× supersample) — **needs geometry**.
4. `updateUniforms`: buckets (1/m view depth), refSphere/refBox/refParams, neighbor packing, ambscale/radscale feedback regime.
5. Pipeline ordering: regular probes update AFTER frame (stale-by-design), hero BEFORE; occlusion pass.

### P4 — Hero (mirror) probes + SSR generation
1. Hero selection (nearest front-facing mirror box), mirror camera + clip plane, same-frame render, ¼-mip radiance, `heroData`→UBO copy.
2. SSR: `mSceneMap` = prev-frame resolved color; `tapScreenSpaceReflection` ray-march; standalone SSR post pass.

**Verification-vs-oracle applies to P1 + P2 now; P3/P4 verify in-world with the geometry stratum.** Consumers stay decal-safe (reverse-Z gate) throughout.

---

## 10) Load-bearing invariants (must hold or behavior silently corrupts)
- Default probe pinned `mProbes[0]` / cube layer 0 / radius 4096 / 64 m above camera; shader index 0 = smoothing fallback.
- The 12-render single-bounce sequence (ambscale/radscale regime) — feedback-loop avoidance.
- Two scratch radiance layers at `N` and `N+1`.
- Exact `ReflectionProbeData` field order/sizes (std140, memcpy'd) + the 1-metre view-space depth bucketing feeding `getStartIndex`.
- radiance-gen derives roughness from `mip/max_probe_lod` (ignores C++ `roughness` uniform); irradiance-gen hardcodes `u_width=64` + fixed TBN bitangent.
- Regular probes stale-by-design (update after display); hero same-frame (update before scene).
- SSR/probe captures set `cube_snapshot=1` → SSR off inside captures.
- reverse-Z is load-bearing for decals ([[reverse-z-decals-careful]]) — world-pos reconstruction in P1 must pass the applier gate.
