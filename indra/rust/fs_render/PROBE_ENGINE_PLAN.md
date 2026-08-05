# P3 — Reflection-Probe Engine Integration Plan (default sky probe first)

> P1 (consumer) + P2 (generators) are ported and verified pixel-exact against the real OGL shaders
> (see PROBE_STRATIGRAPHY §8a/§8b). P3 wires them into the live engine: place → capture → convolve →
> pack → consume, verified IN-WORLD. This plan is settled before engine code (per the workstream rule).
>
> Engine integration surface (from the code map, all `src/live.rs` unless noted):
> - Frame = `flush_clear:2642`, one encoder; pass order gbuffer → sky-fs → **resolve(3)** → forward →
>   terrain-gb → **terrain-resolve(6)** → clouds → exposure → tonemap → UI.
> - Resolve = `ensure_resolve:2315`, `resolve_bgl:564` (bindings 0-4 today), `resolve_bind:2339`
>   (0=sky_fs_ubo, 1-4=RT0-3). Uses the STALE committed `resolve.frag.spv` (5 bindings) → engine green.
> - Depth = Depth32Float, **reverse-Z** (clear 0.0, GreaterEqual, `depth_fix():109`), RENDER_ATTACHMENT
>   ONLY (`ensure_depth:2000`) — NOT sampleable yet.
> - Sky = procedural `sky_fullscreen.frag` (`ensure_sky_fs:2363`), sky UBO packed by
>   `scene.rs SceneFrame::fullscreen_sky_ubo():505` (inv_view_proj + WL params + cam + viewport).
> - Camera = `scene::CameraBlock:16`; engine rebuilds `perspective_rh` + `inv_view_proj` from fov/aspect.
> - State lives on `LiveRenderer` (`:150-310`), lazy `ensure_*`; SPV is offline/manual `glslc`.

---

## Scope of P3 (this plan): the DEFAULT SKY PROBE, end-to-end

The default sky probe (stock: `mProbes[0]`, cube layer 0, 64 m above camera, radius 4096, renders
sky/WL-sky/water/clouds/terrain only) is the highest-value, lowest-dependency probe: it is the ambient
fallback for every pixel, and it needs only what the engine ALREADY renders. Automatic 32 m-grid and
manual object probes need object geometry → deferred to the geometry stratum. Hero/SSR = P4.

---

## Key design decisions (settle these first)

### D1 — Capture strategy for slice 1  ✅ DECIDED: A (full sky environment)
- **(A, faithful) — CHOSEN (user, 2026-08-04):** re-render the full sky environment into the 6 cube faces —
  sky-fs + forward sky-dome/sun/moon + water + terrain + clouds (stock's default-probe render-type mask
  sky/WL-sky/water/voidwater/clouds/terrain). Bigger first slice, faithful from the start.
- (B, incremental) render only the procedural sky first — rejected in favor of faithfulness.
- Consequence for S2: the capture re-runs the engine's sky-ish passes per face with a face camera (not
  just `sky_fullscreen.frag`). Refactor those pass blocks to be callable with a target + face camera.

### D2 — Avoid an ambient regression when `resolve.frag.spv` is recompiled
Recompiling the spv switches the engine onto the probe consumer (`sampleProbeAmbient`), which returns 0
for an empty/incomplete probe → terrain/legacy pixels would go dark. So the plumbing MUST land together
with a real (or safely-seeded) default probe. **Decision:** couple plumbing + sky capture + convolve in
the same slice so probe 0 produces real sky irradiance from the first recompile. As a belt-and-suspenders
guard, seed the irradiance cube to a neutral (sky-ambient-ish) grey before the first capture completes.

### D3 — reverse-Z position reconstruction (decal-gate sensitive)
`resolve.frag getViewPositionFromDepth` uses `ndc.z = 2*depth-1` (OGL clip) + `pp_inv_proj` — correct for
the P1 fixture (OGL-form matrix, depth 0.95). The ENGINE depth is reverse-Z Vulkan **[0,1]** (clear 0 =
far, GreaterEqual). So `2*depth-1` is WRONG for the engine. **Decision:** parameterize the ndc-z mapping
via `ProbeParams.pp_misc` (a z-convention flag or near/far), feed `pp_inv_proj` = the engine's reverse-Z
`inv_proj`, and keep the fixture on its OGL convention. **The decal/applier acceptance gate verifies this**
([[reverse-z-decals-careful]]) — layered coplanar appliers must still render after pos reconstruction lands.

### D4 — Cube-array layout for slice 1
Radiance array = `count+2` cubes @ `RenderReflectionProbeResolution` (128) **mipped** `R11F_G11F_B10F`
(wgpu `Rgb9e5Ufloat`? no — use `Rgba16Float` for simplicity/robustness, revisit format-match later);
irradiance array = `count` cubes @ 16 no mips. Slice 1: count=1 (default probe) + 2 scratch layers.
`max_probe_lod = log2(res)-1 = 6`.

### D5 — Gen orchestration shaders still needed
P2 ported the two convolutions. The DOWNSAMPLE chain also needs: a 9-tap separable **gaussian**
(`gaussianF.glsl`) + a passthrough **mip-copy** (`reflectionmipF.glsl`). Port both (trivial) in P3.

---

## Build order — headless-testable sub-steps, then ONE in-world gate

Each sub-step compiles + passes a headless check before the next; the final in-world verification needs
the user's SL launch.

### S1 — Probe resources + 12-binding resolve plumbing  ✅ DONE (2026-08-04)
Landed: `LiveRenderer` probe fields (radiance/irradiance cube arrays, ReflectionProbes + ProbeParams UBOs,
brdfLut, probe sampler, R32Float depth stand-in) + `ensure_probes` (creates resources, seeds a neutral
grey default probe: refmapCount=1, layer 0, radius 4096) + `resolve_bgl`/`ensure_resolve` extended to 12
bindings + `ensure_depth` gains the R32Float g_depth stand-in + `resolve.frag.spv` recompiled (engine now
runs the probe consumer). Bug fixed en route: `live_gb.frag` wrote RT1.w=1.0 (the probe resolve reads it as
env-intensity → `applyLegacyEnv` overwrote lighting) → now 0.0. Depth note: wgpu rejects a Depth32Float on a
`texture2D` (Float) binding under SPIR-V passthrough; the R32Float stand-in suffices because
`normalize(view_pos)` is depth-independent (real per-pixel depth-export lands with parallax multi-probes).
Verified headless: `deferred_resolve_applies_ndl_shading` passes (sun-lit quad bright, neutral probe adds
grey ambient, no regression); all fixtures still pixel-exact; the 2 `fullscreen_sky_*` failures are
PRE-EXISTING (sky conformance), confirmed by stashing P3 and seeing identical values.

Original spec:
- New `LiveRenderer` fields: `probe_radiance_cube`, `probe_irradiance_cube` (+ views), `probe_ubo`
  (ReflectionProbes), `probe_params_ubo`, `brdf_lut` (+view), `probe_sampler`; a `probe_depth_view`.
- `ensure_depth`: add `TEXTURE_BINDING` usage; make a sampleable view for binding 5.
- Extend `resolve_bgl:564` to bindings 5-11; extend `ensure_resolve:2339` bind entries; null `resolve_bind`
  on probe-resource change.
- Generate `brdf_lut` (reuse the P2 split-sum LUT; CPU-generate + upload, or a one-time GPU pass).
- Seed a valid default probe (D2): refmapCount=1, neutral irradiance cube, identity-ish ProbeParams.
- **Recompile `resolve.frag.spv`** (glslc). Headless: `deferred_resolve_*` + a new probe-plumbing test pass.

### S2 — Sky-environment capture into the radiance cube scratch layer  ✅ DONE (2026-08-05): sky + terrain + clouds + water

**D1 (full sky environment) COMPLETE.** WATER (2026-08-05): after being caught cherry-picking (I'd
inferred "tapped → hard/fragile/circular" without reading the code), the full excavation of water.vert/frag/
packing/pipeline showed water is a SELF-CONTAINED forward pass (fog integral + fresnel sky/ambient tint +
sun glint + wave normal-maps; samples only bump0/bump1 — no scene, no probe, not circular), with a swappable
single `mvp` over region-space geometry, depth-tested (reverse-Z GreaterEqual). So it captures like terrain:
per face, per queued CLASS_WATER draw, build a face water UBO `[rev*proj_face*view_face | the draw's aux]`
(aux read back from `ubo_stage[ubo_off+64..+256]`), render the queued water geometry into cap_scene with
cap_depth (Load if terrain rendered else Clear-0, so terrain occludes water). Capture water ring
(`probe_cap_water_ubo`, 256 B × 16, dynamic offset) + per-sw_key binds. Verified: `probe_water_capture_changes_faces`
turns the down-facing face from blue sky [0.13,0.20,0.48] → water [0.25,0.26,0.28]. LESSON: excavate the WHOLE
path before assessing — the sliver-inference was exactly wrong.

Prior (sky/terrain/clouds) below. All in `capture_probe_environment`: mini deferred-frame per face rendered
into `cap_scene` (probe_res) then COPIED to the cube face. 11/12 headless pass; the 2 `fullscreen_sky_*` are
PRE-EXISTING (sky conformance, identical values with/without P3). Next: S3 convolution makes the probe light
the scene.

Prior partial note (sky + terrain + clouds):

Done + verified: **sky** + deferred **terrain** + volumetric **clouds** captured into the 6 faces of the
radiance scratch cube via `capture_probe_environment` — now a real mini deferred-frame per face rendered
into a `cap_scene` target (probe_res) then COPIED to the cube face (the terrain resolve samples the probe
cubes, so it can't render into the cube it reads). Per face: sky-fs → cap_scene; terrain-gb → face G-buffer
→ resolve over the sky (Load); clouds → composite (Load); copy cap_scene → cube face. Terrain uses a per-face
reverse-Z view_proj (`rev*proj*view`, swap `terrain_typed_ubo[0..16]` per face, cache+restore via a Cell);
face resolve bind = `resolve_bgl` with binding 0 = `probe_cap_ubo` + the face G-buffer. IMPORTANT: `ensure_resolve`
must run BEFORE the capture (the terrain resolve needs `resolve_pipeline`) — reordered in flush_clear.
Tests: `probe_sky_capture_fills_scratch_faces` (sky+clouds gradient), `probe_terrain_capture_changes_faces`
(terrain turns the down-facing face from blue sky → grey lit ground [0.20,0.22,0.26]). 9/11 headless pass; the
2 `fullscreen_sky_*` failures are PRE-EXISTING (sky conformance).

REMAINING for D1: **WATER** (tapped forward geometry, baked MVP) — needs a per-face re-camera path for tapped
draws; flag if not trivially re-runnable. Then S3 (convolution — makes the probe actually light the scene)
→ S4 (per-frame packing) → S5 (in-world).

Original spec:
Landed: `probe_cap_ubo` + `probe_cap_bind` + `capture_probe_sky()` — 6 face cameras (90° FOV, standard
cube basis) drive the existing `sky_fullscreen.frag` pipeline into the 6 faces of the radiance cube's
SCRATCH layer (one submit per face, per-face inv_view_proj, current WL params). `cube_array_filled` gains
COPY_SRC (readback + S3 mip copies); captured once per (re)allocation. VERIFIED headless
(`probe_sky_capture_fills_scratch_faces`): the faces hold a real per-direction sky gradient (zenith +Z
darkest [0.01,0.02,0.06], horizon brightest [0.13,0.20,0.48], blue-dominant) — differs from the grey seed
and varies. `deferred_resolve` still passes.
REMAINING for D1 (full environment): fold terrain (deferred terrain pipeline w/ face view_proj) + clouds
(cloud pass per face) + water into the per-face capture. These are additive passes on the proven mechanism.

Original spec:
- A cube render target (or render into radiance scratch layer `count`), 6 faces @ `res*4` supersample
  (or `res` for slice 1), 90° FOV per-face view matrices (the 6 look/up dirs), aspect 1.
- Re-run `sky_fullscreen.frag` per face with per-face `inv_view_proj` (from `perspective_rh` + face view).
- Headless: read back a face; assert the sky gradient/horizon is present.

### S3 — Gen orchestration: gaussian → mip chain → convolve — DONE (headless verified)
- `gen.vert`/`radiance_gen.frag`/`irradiance_gen.frag` compiled; ported + compiled `gaussian.frag` +
  `reflection_mip.frag` + `fsq_uv.vert`. **gen.vert clip-z fix:** stock radianceGenV uses position.z=-1
  (GL clip [-1,1] near plane); Vulkan clip-z is [0,1] so that quad clips → pinned `gl_Position.z=0` (no
  depth test in the gen passes), `vary_dir` keeps position.z=-1 as the ray. This was the bug behind an
  all-black radiance on the first run.
- **Capture at CAP_SUPER=4× (512)** — pulled forward from "deferred" below, so the gaussian pre-blur
  (resScale=1/(probeRes*2)) filters the supersample before the 4:1 reduction, faithful to stock
  `updateProbeFace`. Scene/G-buffer/depth/cloud targets scaled to cap_res.
- Per face: gaussian H (cap_scene→gauss_tmp) + V (gauss_tmp→cap_scene); reflection-mip chain
  (scratch2d[0]←cap_scene, scratch2d[i]←scratch2d[i-1], 128..2); copy each into `probe_scratch` (a
  SEPARATE 1-cube 7-mip texture — wgpu forbids read+write of one texture in a pass, so the prefilter reads
  scratch and writes radiance). `probe_radiance` re-allocated MIPPED (7 mips); irradiance stays 16.
- `convolve_probe`: radiance GGX prefilter (6 faces × 7 mips → radiance slot 0) reading the scratch mip
  chain at the solid-angle LOD; irradiance cosine convolve (6 faces → irradiance slot 0). Per-face rotation
  = `look_at(dir,up).inverse()` on the SAME face basis as capture (sClipToCube is a stock-internal cube-array
  convention, not portable — confirmed by reading llcubemaparray.cpp). GenParams dynamic-offset UBO per draw.
- Headless VERIFIED (`probe_convolve_fills_radiance_and_irradiance`): radiance mip 0 == the captured sky
  faces (roughness-0 passthrough, matches to ~1e-3); irradiance == smooth positive per-face ambient. The
  three capture tests re-pointed at `probe_scratch`. No regressions (only the 2 pre-existing sky failures).
### S3-C — single-bounce feedback — DONE (headless verified)
- Whole-chain read: the single bounce is expressed by toggling the DEFAULT probe's `refParams.x/y` between
  two capture passes (NOT a separate render path). `is_ambiance_pass` → `ambscale`/`radscale`
  (llreflectionmapmanager.cpp:1131-1211): ambiance pass `refParams=(0, 0.5)` (direct-lit: analytic ambient +
  half old radiance) → convolve IRRADIANCE; radiance pass `refParams=(minimum_ambiance, 1)` so the capture's
  terrain resolve samples the FRESH irradiance → convolve RADIANCE. `resolve.frag` already consumes it
  (`tapRefMap` × `.y`, `tapIrradianceMap` `mix(amblit, probe_irr*.x, min(.x,1))`).
- Implemented: `bake_default_probe` (two passes) + `convolve_probe(do_radiance, do_irradiance)` +
  `set_default_refparams` + `set_probe_ambiance`. ONE cycle (faithful: stock never converges within an
  update; the temporal loop converges over subsequent in-world frames). Replaces the old single capture.
- **Gating (from the code):** the diffuse bounce needs `classic_mode == 0` (resolve.frag:406/425 — legacy
  classic bypasses probe ambient) AND `minimum_ambiance = getReflectionProbeAmbiance() > 0` (getAmbiance()=0
  for the default probe). Legacy sky (ambiance 0) → diffuse bounce inert (amblit), exactly as stock; the
  radiance (0.5→1) part still affects terrain specular.
- Headless VERIFIED (`probe_single_bounce_lights_terrain`): ambiance 0→0.6 shifts the terrain-facing radiance
  toward the sky's blue irradiance (grey [0.047] → bluish [0.038,0.046,0.084]).
- **Finding (not a bug):** removing the old `refParams.x=1` default exposed that the hand-packed probe TEST
  UBOs omit `sun_dir` (u52-54) + `sky_ambient_scale` (u49), so the capture-terrain got no sun/amblit and read
  black (the grey-seed probe ambient had masked it). The engine's `fullscreen_sky_ubo` DOES pack these
  (scene.rs:536,541-542), so in-world the capture-terrain is lit; the fix was to pack a realistic sky UBO in
  the terrain/single-bounce tests.

Deferred with the manager scheduler: the multi-frame 12-call cadence, priority/occlusion, realtime probes.

### S4 — per-frame default-probe view transforms — DONE (headless verified)
- Whole-chain read: for the SINGLE default probe (radius 4096, parallax off) only two per-frame values are
  load-bearing — everything else is static. `refSphere` center + `refParams.w` are irrelevant (a single
  parallax-off probe's weight cancels in the resolve's `col/=wsum` normalization; `sphereWeight` doesn't even
  read `.w`). `refParams.x/y` = S3-C's ambiance/radiance. `refBucket`/`refmapCount`/`refIndex` static.
- The two that matter (were hard-coded to IDENTITY, only right when view=identity):
  - `pp_env_mat` = view→world = `transpose(view 3x3)` (stock `setEnvMat`: uniformMatrix3fv of modelview 3x3,
    transpose=TRUE). `pp_inv_proj` = `inverse(rev*proj)` (reverse-Z; getViewPositionFromDepth's z-conv=1 path).
- Implemented: `SceneFrame::probe_params()` (inv_proj, env_mat from the typed CameraBlock),
  `LiveRenderer::set_probe_camera` (packs the ProbeParams UBO), `SkyRegime::probe_ambiance` (=
  getReflectionProbeAmbiance), and `fsr_end_frame` now feeds `set_probe_camera` + `set_probe_ambiance`
  per-frame (mirrors the sky/terrain feeds) so it's live in-world.
- **Finding:** `env_mat`/`inv_proj` affect ONLY reflections, not diffuse ambient — in `tapIrradianceMap`
  the sample dir is `env_mat * view_norm` where `view_norm = transpose(env_mat)*n`, so `env_mat` CANCELS →
  diffuse ambient samples world normal `n` regardless. Diffuse probe ambient was already correct even at
  identity; S4's real in-world effect is on REFLECTIVE surfaces (the specular path uses the real view ray).
- Headless VERIFIED (`scene::probe_params_view_transforms`): env_mat un-rotates the view (`env_mat*view==I`)
  and inv_proj inverts `rev*proj`, for a non-identity camera. The non-identity reflection difference is a
  visual S5 gate (env_mat only affects reflections, best seen in-world). All 6 lib + 15 headless tests pass.

### S5 — IN-WORLD verification (needs the user's SL launch)
- Sky ambient + reflections appear on terrain/objects; matches stock's look qualitatively.
- **Decal/applier acceptance gate** (D3): rez layered coplanar appliers; confirm no z-fighting / dropout
  from the reverse-Z pos reconstruction.
- Scheduler: for slice 1 a single capture (or every-N-frames refresh) is fine; the full 12-render
  single-bounce cadence + occlusion come with the multi-probe slice.

---

## Deferred beyond this plan
- Automatic 32 m-grid + manual box/sphere probes (need object geometry; the walk/mix/parallax consumer is
  already verified) — geometry stratum.
- The full manager scheduler: the multi-frame 12-call cadence (one face/frame, ~2 s refresh), priority,
  occlusion, and realtime/hero probes. (The single-bounce feedback itself is DONE — see S3-C above; this is
  only the temporal scheduling that spreads it across frames + the multi-probe machinery.)
- 4× supersample — DONE in S3 (CAP_SUPER=4). Still deferred: faithful `R11F_G11F_B10F` cube format (we use
  `Rgba16Float`).
- P4: hero (mirror) probes + SSR generation.
