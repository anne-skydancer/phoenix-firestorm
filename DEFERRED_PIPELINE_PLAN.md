# Deferred Rendering Pipeline — Audit & Phased Implementation Plan

**Status:** proposal, awaiting approval. No engine code lands until this is signed off.
**Date:** 2026-08-01.
**Method:** every claim below was traced from real code by agents and is cited `file:line`. Canonical stock = `c:/fs/ll-canonical/indra` (Second Life PBR deferred renderer). Our fork (viewer + tap + null-GL stub) = `c:/fs/firestorm-upstream/indra`. Our engine = `c:/fs/fs-vulkan-engine/indra/rust/fs_render`.

## Decisions locked (this session)

1. **Parity target = FULL deferred parity.** The G-buffer and the tap's `DrawDesc` are designed for the complete PBR resolve (normal+tangent, ORM, roughness/metallic, emissive) from day one, so probes/IBL/mirrors need no retrofit.
2. **Capture model = ship model matrices in the tap.** `DrawDesc` gains a per-draw **world (model) matrix + separate modelview**; the engine re-projects the tapped draw stream from shadow / probe / mirror cameras. C++ keeps the cheap CPU math (cascade-split fit, probe placement); the engine owns only the GPU traversals.

### Recommended defaults on the smaller forks (will proceed unless overridden)

- **AA:** abandon the MSAA path entirely; post-AA **FXAA first**, SMAA later. `RenderFSAASamples` is a **0–3 quality tier**, not a sample count (`pipeline.cpp:8088`).
- **HDR format:** `Rgba16Float` for all HDR targets (widest wgpu support). Stock's `R11F_G11F_B10F` cube arrays are a memory optimization, not correctness (`llcubemaparray.cpp:162`).
- **Encode site:** single site. Cleanest stock match = **UNORM swapchain + in-shader `linear_to_srgb`**; acceptable fallback = keep the sRGB swapchain and let the ROP encode (then never encode in-shader). Pick one — never both (double-gamma).
- **Depth convention:** keep engine **reverse-Z** for the main/gbuffer/resolve passes; render **shadow maps in isolated forward-Z** (Less, clear 1.0) since `mSunShadowMatrix` is authored for GL_LESS. Re-derive `getPositionWithDepth` for our `[w,0]` depth in the resolve.
- **Emissive G-buffer attachment:** defer initially (stock default `RenderEnableEmissiveBuffer=false`); add when bloom fidelity needs it (Phase 9).

---

## 1. The finding

Our engine is **one clear + one unlit forward pass to an 8-bit sRGB swapchain** (`live.rs:1291-1417`, swapchain `lib.rs:90-95`, depth `live.rs:1259-1277`). Stock is a **~10-stage deferred pipeline** resolving into a float HDR buffer (`mRT->screen = GL_RGBA16F`, `pipeline.cpp:875`). Every symptom traces to a specific, named gap:

| Symptom you reported | Root cause (cited) |
|---|---|
| Flat / washed world | No deferred lighting resolve (`softenLightF`); world surfaces are raw `texture × color` (`live.frag:17-38`). Sun/ambient/atmospherics/AO never computed. |
| No global tonemap; brightness won't track | No float scene buffer to tonemap into; only `sky.frag` hardcodes PBRNeutral (`sky.frag:23-43`). Radiance >1.0 clamped at write. |
| AA "violation of contract" | Stock uses **no MSAA** on deferred; AA is post-process FXAA/SMAA on the resolved image (`pipeline.cpp:8088`). The gated-off MSAA path was a dead end by construction. |
| Mirrors shatter the background | Hero-probe `renderProbes()` is **not** wrapped in a suppress scope (`llviewerdisplay.cpp:883`); reflected-view draws leak into the single live queue with a foreign MVP. |
| Sun disc = orange ball + vertical flare | `CLASS_SKY_SUN`/`MOON` unhandled → generic unlit pipeline; **Repeat** sampler (clamp is terrain-only, `live.rs:326-343`) wraps disc edge texels into streaks; missing `pos.z = w*0.999999` far-plane smash (`sunDiscV.glsl:47`) → reverse-Z clips disc to a sliver. |
| Clouds wrong/absent | `renderSkyCloudsDeferred` never calls `setDrawClass` (`lldrawpoolwlsky.cpp:294-356`) → clouds inherit `SKY_STARS` and fall through to generic. |
| Water has no reflections | Water pipeline exists but ships **no reflection/refraction render targets** (`water.frag:2-4`). |

---

## 2. Comparison matrix

| Stage | Stock | Ours | Severity |
|---|---|---|---|
| **HDR linear scene buffer** | `mRT->screen` RGBA16F, alpha=glow (`pipeline.cpp:875`) | None — 8-bit sRGB swapchain only | **blocker** |
| **G-buffer MRT fill** | albedo RGBA8 + ORM RGBA8 + normal RGBA16 (+ emissive RGB16F opt) + shared depth (`pipeline.cpp:347-372`) | None — one unlit color out | **blocker** |
| **Tap geo/material plumbing** | per-vertex normal+tangent, normal_matrix, material model, PBR factors in-process | Ships only baked MVP + diffuse (`fsscenedump.cpp:494`) | **blocker** |
| **Deferred lighting resolve** | `softenLightF` — PBR/legacy/SKIP_ATMOS branches, sun+ambient+atmos+AO (`softenLightF.glsl:119-285`) | None for world geometry | **blocker** |
| **Sun/moon cascaded shadows** | 4 sun + 2 spot depth maps, 5-tap HW-PCF → lightMap.r (`pipeline.cpp:9903`, `shadowUtil.glsl`) | None — `generateSunShadow` early-returns (`pipeline.cpp:11108`) | major |
| **Reflection probes (IBL)** | cubemap-array radiance + irradiance, 1 face/frame capture (`llreflectionmapmanager.cpp`) | None | major |
| **Hero probes / mirrors** | per-frame mirror capture (`llheroprobemanager.cpp`) | None — **plus the live shatter** | major |
| **Local point/spot lights** | up to 256 additive light volumes (`pipeline.cpp:8645-8895`) | None | major |
| **Auto-exposure + tonemap** | luminance→exposure→tonemap (`pipeline.cpp:7293-8044`) | None global (only sky.frag) | major |
| **Anti-aliasing** | FXAA/SMAA post, tier-selected (`pipeline.cpp:8088`) | Broken MSAA, gated off (`live.rs:590`) | major |
| **Forward alpha/translucent** | forward-lit, inline atmos+probes+fog (`alphaF.glsl:251-305`) | Unlit generic quad | major |
| **Water reflection/refraction** | `mWaterDis` RGBA16F screen copy + probes (`lldrawpoolwater.cpp:112-248`) | Waves/fog/fresnel only, no RTs | major |
| **Sky dome (haze)** | deferred haze, SKIP_ATMOS resolve | Faithful single-pass reproduction | minor |
| **Clouds / sun / moon** | dedicated WLSky sub-draws | Fall through generic (untagged/unhandled) | major |
| **Stars** | additive + twinkle (`starsF.glsl:47-69`) | Additive works; no twinkle/scale | cosmetic |
| **SSR** | screen-space raymarch (`screenSpaceReflUtil.glsl`) | None | minor |
| **Bloom / glow** | extract→blur→combine (`pipeline.cpp:7350-7458`) | None | minor |
| **Depth of field** | CoC + bokeh (`pipeline.cpp:7823-8014`) | None (depth exists) | cosmetic |

---

## 3. Foundations frozen up front (decided, so they don't churn later)

### 3.1 `DrawDesc` / tap additions (design once, in Phase 3)

Today the tap ships only `projection×modelview` baked (`fsscenedump.cpp:494`) + diffuse tex0 + color + min_alpha. Add:

- **Per-draw:** world (model) matrix `mat4` + separate modelview `mat4` (→ derive normal_matrix); per-vertex **normal** and **tangent+sign** attribute bindings; a **material-model tag** (PBR / legacy-material / simple / terrain / avatar); PBR scalars (metallic, roughness, emissive color, env_intensity, glossiness) pulled from the bound shader `mValue` cache like `color[4]`/`min_alpha` already are (`fsscenedump.cpp:420-438`); ORM + emissive **texture ids** (normal `tex_ex[5]`/spec `tex_ex[6]` already cross unused, `fsscenedump.cpp:604-616`).
- **Per-frame globals (new `fsr_set_*` calls, not per-draw):** the EEP atmospherics env block (already captured for sky at `fsscenedump.cpp:513-580` — ship once/frame as a global); eye-space sun_dir/moon_dir + inv_proj; the 6 shadow matrices `mSunShadowMatrix[6]` + clip planes + bias (`fsr_set_shadow_matrices`, model on `fsr_set_matrix_palette`); the ReflectionProbes std140 UBO (`fsr_set_reflection_probes`, layout `reflectionProbeF.glsl:45-82`); the light array (`fsr_set_lights`); the post-params scalars (exposure/tonemap type/glow/AA settings).

### 3.2 G-buffer target layout (mirror `addDeferredAttachments`, `pipeline.cpp:347-372`)

- `gbuf_albedo` `Rgba8Unorm` (raw; sRGB/linear done in-shader) — loc 0
- `gbuf_orm` `Rgba8Unorm` (occ/rough/metal, or Blinn spec+gloss for legacy) — loc 1
- `gbuf_normal` `Rgba16Unorm` (Lambert-azimuthal `encodeNormal` xy + envIntensity.z + **gbufferFlag**.w using the exact codes `0.0/0.34/0.67/1.0` from `llshadermgr.cpp:630-633`) — loc 2. **16-bit required**; 8-bit bands badly.
- `gbuf_emissive` `Rgba16Float` — loc 3, gated (defer).
- `scene_hdr` `Rgba16Float` — the lit-resolve target; **one depth shared** across gbuffer-fill / resolve / forward-alpha.

### 3.3 Pass restructure

`flush_clear` records exactly one swapchain pass today (`live.rs:1291`). It becomes:

```
[shadow passes ×6  → depth-only, forward-Z, into shadow arrays]   (Phase 5)
[probe capture     → amortized 1 face/frame, into cube arrays]    (Phase 6)
 gbuffer fill      → 4-MRT offscreen, reverse-Z                   (Phase 3)
 deferred resolve  → fullscreen tri → scene_hdr (linear)          (Phase 4)
 local lights      → additive volumes → scene_hdr                 (Phase 7)
 forward alpha/water/sky → into scene_hdr (reads scene copy)      (Phase 4/6)
 post chain        → luminance→exposure→tonemap→glow→AA→dither    (Phase 1/2/9)
 present           → swapchain
```

---

## 4. Phased roadmap

Each phase is a **complete, real stage** (no stubs) with a **visible in-world result** you can confirm. Ordered by hard dependency.

### Phase 0 — Stop the mirror shatter *(independent, ship immediately)*
- Wrap `mHeroProbeManager.update()+renderProbes()` (`llviewerdisplay.cpp:883-884`) in an `FSSceneDump::SuppressScope`, exactly like `initReflectionMaps` (`pipeline.cpp:919`), or early-return under `FSSceneDump::liveActive()`.
- **Pure suppression, no engine change.** **See:** toggling RenderMirrors no longer shatters the background.

### Phase 0.5 — Sky sub-draw quick wins *(independent, cheap, fixes the sun-disc rage)*
- Route `CLASS_SKY_SUN`/`MOON` to a **clamp sampler** (clamp exists but is terrain-only, `live.rs:326-334`) and reproduce the `pos.z = w*0.999999` far-plane smash (`sunDiscV.glsl:47`) → kills the vertical flare + depth sliver.
- Add moon alpha-discard (`≤2/255`, `moonF.glsl:48`) + depth-write so the moon clips stars.
- Add `setDrawClass(DRAWCLASS_SKY_CLOUDS)` in `renderSkyCloudsDeferred` (currently missing) + a cloud pipeline; ship cloud-noise tex ids like water's `setAuxTex`.
- Star twinkle + `*32`/custom_alpha scaling (`starsF.glsl:47-69`).
- **See:** a clean orange sun disc (no flare), a moon that doesn't eat stars, real clouds, twinkling stars — *before* any deferred work.

### Phase 1 — HDR linear scene buffer + exposure/tonemap resolve *(the washed-look fix)*
- Add `scene_hdr` `Rgba16Float`; forward passes render into it, not the swapchain. **Make `sky.frag`/`water.frag` STOP tonemapping** and emit **linear HDR** (`live.rs`/`sky.frag:23-43`) so one global tonemap governs all surfaces.
- Port the post chain: luminance (256² R16F + mip reduce) → exposure (1×1 R16F, temporal adapt, prev-frame retain) → tonemap (`tonemapUtilF` + `postDeferredTonemap`, PBRNeutral/ACES by `RenderTonemapType`) → in-shader `linear_to_srgb` → dither present.
- New per-frame post-params UBO: `RenderExposure`, `RenderTonemapType`, `RenderDynamicExposure*`, sky HDR offset/min/max, `gFrameIntervalSeconds`.
- **See:** sky/water/world share one exposure under a consistent filmic tonemap; scenes adapt bright/dark; sRGB-clamp washout gone. (World still unlit — Phase 4.)

### Phase 2 — Post anti-aliasing (FXAA, correct contract)
- Park the MSAA path. Read `RenderFSAAType` (0/1/2 = none/FXAA/SMAA) + `RenderFSAASamples` as a 0–3 **quality tier** (4 shader variants). Port `fxaaF.glsl`.
- **See:** smooth edges, no shimmer, selectable by the standard settings — the contracted AA, done the way stock does it.

### Phase 3 — G-buffer MRT fill + tap geo/material plumbing *(the foundation)*
- Allocate the §3.2 targets; restructure `flush_clear` to a gbuffer-fill pass. Author fill shaders forking by material class (`pbropaqueF`/`materialF` non-blend/`diffuseF`/`avatarF`/`terrainF`), each emitting `frag_data[0..3]` with `encodeNormal` + gbufferFlag. Ship the §3.1 per-draw data.
- **Order within phase:** simple/diffuse fill first (covers most current geometry), then legacy materials, then PBR (needs tangents+ORM).
- **See:** a debug buffer-visualizer (mirror `postDeferredVisualizeBuffers`) shows correct albedo / view-space normals / ORM — material capture proven before any lighting.

### Phase 4 — Deferred lighting resolve *(the flat-look fix)*
- Fullscreen-triangle soften pass → `scene_hdr`. Port `softenLightF` branch-by-branch. **Minimum viable subset** (no probes/shadows/SSAO): `color = amblit + min(dot(N,L),1)*sunlit`, then `*albedo`, `scol=1`, `ambocc=1` — directional sun + sky ambient on every surface. Port `calcAtmosphericVars`/`calcAtmosphericVarsLinear` (`atmosphericsFuncs.glsl:51-165`) verbatim, fed by the per-frame EEP UBO. Re-derive `getPositionWithDepth` for reverse-Z.
- **Note (proven):** stock's PBR soften does **not** add distance-haze to opaque geometry (`atmosFragLighting` is water/forward-only). Matching stock = getting sunlit+amblit+probe-irradiance right; do **not** port `atmosFragLighting` into the opaque resolve or we diverge.
- **See:** the world becomes directionally lit — sun-facing surfaces brighten, back-faces fall to sky ambient, EEP/day-night drive real shading. Flat look gone.

### Phase 5 — Sun/moon cascaded shadows
- Keep C++ `generateSunShadow` computing `mSunClipPlanes`+`mSunShadowMatrix` (cheap CPU); ship them via `fsr_set_shadow_matrices`. Un-suppress the shadow draw stream, tag it `shadow cascade k`. Allocate a 4-layer (+2 spot) `Depth32Float` array + a **comparison sampler** (LessEqual). Render depth-only in **forward-Z** (Less, clear 1.0) — do **not** apply `depth_fix()` to shadow view-projs. Port `pcfShadow`/`sampleDirectionalShadow` (`shadowUtil.glsl:54-195`), sample in eye-space (matching stock `spos`).
- **See:** objects/avatars cast sun/moon shadows; cascades cover distance; low-sun scenes darken correctly.

### Phase 6 — Reflection probes + PBR IBL + forward alpha/water reflections
- Cube arrays (radiance `Rgba16Float` 128², (N+2)·6 layers, 7 mips; irradiance 16², N·6 layers). Ship the ReflectionProbes UBO. Capture by re-projecting the tapped stream from probe origins (uses the **model matrices** from §3.1) — amortize 1 face/frame (`llreflectionmapmanager.cpp:741`); GGX prefilter mips + irradiance convolution. **Cheap first milestone:** capture only the **default probe** from sky+terrain+water (biggest ambient/horizon win, no per-object probes). Port `sampleReflectionProbes`/`doProbeSample` into the resolve.
- Now that a sampleable `scene_hdr` exists: give **forward alpha** a lit pipeline (inline atmos+probes+fog, `alphaF.glsl`) and **water** its refraction copy (`mWaterDis` equivalent) + probe reflection.
- **See:** metal/glossy surfaces reflect the environment; water reflects/refracts; translucents are lit and fogged; PBR reads as metal-roughness.

### Phase 7 — Local point/spot lights
- `fsr_set_lights` per-frame array (center/radius/linear-color/falloff/spot params, respect `RenderLocalLightCount`). Additive pass over `scene_hdr`. Port `pointLightF`/`multiPointLightF`/`spotLightF` (class3). **Simplest first cut:** always fullscreen-triangle loop over N lights from a storage buffer (skip cube-volume optimization; identical correctness, more overdraw). Spots (projector + spot shadow) later.
- **See:** lamps/glow/scripted lights illuminate nearby surfaces with correct falloff; night gains local light pools.

### Phase 8 — SSR + hero probes / working mirrors
- SSR: retained prev-frame `scene_hdr` + linear depth + camera `modelview_delta`; port `traceScreenRay`/`tapScreenSpaceReflection`, gated glossiness≥0.9.
- Hero: hero cube array + clip plane; Phase 6's capture machinery from the mirror's reflected viewpoint each frame; port `tapHeroProbe`. Re-arm RenderMirrors under the bridge (replaces Phase 0's suppression with real capture).
- **See:** wet/glossy surfaces show screen-space reflections; mirrors render a live reflection.

### Phase 9 — Post polish: bloom, SMAA, DoF, dither
- Enable the emissive attachment / glow alpha; extract→blur→combine bloom. SMAA (area/search LUTs across the tap). Optional DoF (depth already present). Dither present to kill banding — full `renderFinalize` parity.
- **See:** emissive surfaces bloom; cleaner SMAA edges; optional DoF; banding gone.

---

## 5. Cross-cutting concerns

- **Reverse-Z vs forward-Z (proven clash):** main/gbuffer/resolve stay reverse-Z (`depth_fix`, `live.rs:80-91`); shadow space is isolated forward-Z (`mSunShadowMatrix` is GL_LESS). Every depth-consuming port (`getPositionWithDepth`, shadow render) must be re-derived or kept in its own space.
- **FIFO present stall:** the engine is 1 pass today; the target is ~8–12 passes/frame. Under FIFO (`lib.rs:105`) any overrun halves framerate. Keep stock's amortization (1 probe face/frame); consider `Mailbox` present during bring-up.
- **VRAM budget:** full-res RGBA16F scene (+ SSR history) + 3–4 MRT G-buffer + 6 shadow depth arrays + radiance/irradiance cube arrays + glow/exposure/ping-pong is a large step from one swapchain+depth. Gate probe count/resolution by settings (mirror `RenderShadowResolutionScale`, `RenderReflectionProbe*`).
- **Ring/upload:** new per-frame channels (shadow matrices, ~40KB probe UBO, light array, post params) must stay **once-per-frame**, not per-draw, like the existing UBO/palette rings.

## 6. Remaining open decisions (smaller — defaults chosen, flag if you disagree)

1. **SMAA** worth the area/search-LUT asset plumbing, or is FXAA enough? (Default: FXAA now, SMAA in Phase 9.)
2. **Emissive G-buffer attachment** on now or deferred? (Default: deferred to Phase 9; stock default is off.)
3. **Present color-space:** UNORM swapchain + in-shader encode (exact stock match) vs keep sRGB swapchain + ROP encode. (Default: match stock.)

## 7. Verification per phase

Each phase ships behind a flag with an in-world confirmation (the "See:" line) and, where applicable, a debug visualizer (G-buffer buffers, shadow cascades, probe cubemaps). Perf captured at the 23-av crowd spot before/after the pass-count-increasing phases (3–8). The `fsr_perf.log` instruments (draws, flush_avg_ms, acquire_ms, texMB) already exist to catch regressions.
