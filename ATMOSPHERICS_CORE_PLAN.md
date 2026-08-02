# Atmospherics Core Plan — light + sky (then water, shadows)

Ground-up, gated, **faithful port** of the LL/WindLight atmospherics into the Vulkan/wgpu
engine. Source of truth: the WindLight/EEP investigation (workflow `w6dsthi6t`, 3 converging
studies + synthesis). **The math has exactly one origin — the faithful GLSL port done here;
after that it is only ever translated (glslc→SPIR-V now, idiomatic SPIR-V later), never
rewritten.** Prior failures ("blew to white / wrong sun") came from *reinventing* the model
and *mis-routing* the sky regime — both proven root causes below.

## The real architecture (PROVEN)

The classic WindLight color model is intact; EEP is only a settings wrapper. The deferred
path is a **4-stage chain**, not one resolve:

1. **Lighting resolve** — `softenLightF.glsl`: reconstruct eye-pos from depth, run
   `calcAtmosphericVarsLinear`, light opaque `HAS_ATMOS` pixels (`amblit + min(N·L,scol)·sunlit`),
   pass sky `SKIP_ATMOS` pixels through (`srgb_to_linear·sky_hdr_scale`). **No haze here.**
   Output LINEAR HDR, `clampHDRRange` [0, 11.2].
2. **Aerial-perspective haze** — `hazeF.glsl`, a **separate** fullscreen pass:
   `finalScene = sceneColor·atten.r + srgb_to_linear(additive·2)·sky_hdr_scale`. (Opaque
   surfaces get haze here, *not* in the resolve — `pbrBaseLight` receives `additive/atten`
   but never uses them; `atmosFragLightingLinear` is declared-never-called in `softenLightF`.)
3. **Auto-exposure** — `exposureF.glsl`: mip-8 luminance → 1×1 exposure scalar. (Deferred; D2.)
4. **Tonemap + gamma** — `tonemapUtilF.glsl` `toneMap()` + `postDeferredTonemap` tail:
   `exposed = color·exposure·exp_scale`; PBRNeutral (type 0) / ACES-Hill (type 1);
   `mix(exposed, tonemapped, tonemap_mix)`; `linear_to_srgb`; **optional `legacyGamma`**.

The **sky dome** is its own mesh pass: `skyV.glsl` runs the WL integral per-vertex →
`vary_HazeColor`; `skyF.glsl` does `color·2; clamp[0,5]`; writes `GBUFFER_FLAG_SKIP_ATMOS`.
Our `sky.vert` is already a faithful `skyV` port; `sky.frag` must **drop its private
PBRNeutral** and emit LINEAR HDR (the single global tonemap owns bounding).

### The regime switch — `classic_mode` (the proven blowout cause)

`classic_mode = canAutoAdjust && !RenderSkyAutoAdjustLegacy`. `canAutoAdjust = sky lacks a
reflection_probe_ambiance key`. `RenderSkyAutoAdjustLegacy` defaults false ⇒ **legacy WL skies
render classic by default.** Classic entails:
`sky_hdr_scale=1.0` (NOT `sqrt(gamma)·2`), `sky_sunlight_scale=sky_ambient_scale=1.5`,
`tonemap_mix=0` (filmic curve BYPASSED — look comes from exposure + legacyGamma), **legacyGamma
enabled**, `amblit/sunlit` kept sRGB with chroma, resolve `sunlit·=1.35`, `final_scale=1.1`.
Advanced/PBR path (`sky_hdr_scale=sqrt(gamma)·2` + full ACES + no legacyGamma) is used **only**
when a sky carries `reflection_probe_ambiance != 0`. Mixing these is the prime "blew to white".

### Reverse-Z reconstruction (HIGH risk)

Stock reconstructs eye-pos with `ndc.z = 2·depth-1` (GL [0,1] depth). Our engine is reverse-Z
(g-buffer rendered with `mvp = depth_fix()·d.mvp`, near=1/far=0). Faithful reconstruction:
`eye = inverse(depth_fix()·proj) · vec4(ndc_xy, depth, 1)` with **raw `depth` as `ndc.z`**, and
the sky test flips `depth>=1` → `depth<=0`. An S4 oracle sub-assert must DIVERGE under `2·depth-1`.

## Decisions (from the user's steer + stock defaults)

- **D1 — Default regime: legacy classic.** `classic_mode=1` default; advanced only for
  probe-ambiance skies. Hardcode `RenderSkyAutoAdjustLegacy=false` for now.
- **D2 — Fixed `exp_scale=1.0`** through S1–S5; auto-exposure = S6, behind a setting.
- **D3 — Fold aerial-perspective into the resolve** for opaque first-light (output-identical
  to the separate `hazeF` pass for opaque); split to a faithful-separate pass when
  forward/alpha/water need it.
- **D4 — Keep the sRGB swapchain.** Do tonemap→`linear_to_srgb`→legacyGamma in-shader and emit
  the ROP **pre-image** (`srgb_to_linear(disp)`) so the sRGB ROP reproduces `disp` exactly.
  Single faithful encode site, zero surface-integration change.

## Gate discipline

Every step ships a **headless oracle** (known input → CPU-computed expected, tol ≈ 2/255,
reusing the `tests/headless.rs` readback pattern) that must be green BEFORE any in-world
eyeball. The math is ported VERBATIM through glslc; only file-scope `uniform`s become UBO
members, and depth reconstruction swaps to reverse-Z.

## Steps

- **S1 — Faithful global tonemap+exposure+legacyGamma (BLOWOUT STOPPER).** [in progress]
  Upgrade `post_tonemap.frag` from the 1a PBRNeutral-only stub to the full `toneMap()`:
  exposure·exp_scale, ACES-Hill (type 1) / PBRNeutral (type 0), `mix(exposed, tonemapped,
  tonemap_mix)`, `linear_to_srgb`, optional `legacyGamma`. Add a Post-params UBO. Oracle:
  known HDR through the pass for {type∈0,1}×{mix∈0,0.7}×{legacy gamma∈1.0,2.2}, plus the
  anti-blowout gradient. **1a already proved PBRNeutral compresses HDR; S1 makes it faithful.**
- **S2 — Sky dome → LINEAR HDR.** Drop `sky.frag`'s private PBRNeutral; emit
  `min(v_haze·2,5)→srgb_to_linear→·sky_hdr_scale`. Rewire so the sky pass writes `scene_hdr`
  consumed by the S1 tonemap LAST. Oracle: CPU-port `skyV`, feed default WL params + a chosen
  `lightnorm`, assert zenith & horizon pixels post-tonemap.
- **S3 — Close the `EepSkyBlock` GAP.** Add `classic_mode`, `sky_sunlight_scale`,
  `sky_ambient_scale`, `scene_light_strength`, `reflection_probe_ambiance`, per-sky
  `tonemap_mix`; FIX `sky_hdr_scale` to the DERIVED scalar; ship `lightnorm` (swizzle
  `(y,z,x)`, z≥-0.1) distinct from eye-space `sun_dir`. Populate the full WL param set from the
  viewer P3 feed. Oracle: regime unit-test (legacy vs probe-ambiance) + re-run S2 unchanged.
- **S4 — Resolve: `calcAtmosphericVars` + sun/ambient on opaque, reverse-Z reconstruction.**
  Replace the stub `resolve.frag`. Oracle: flat quad at known distance/albedo/normal/sun; a
  reconstruction sub-assert that diverges under `2·depth-1`; both classic_mode branches.
- **S5 — Aerial perspective on opaque (fold, per D3).** `color = color·atten.r +
  srgb_to_linear(additive·2)·sky_hdr_scale`. Keep the magic `·2` (dead-scaleSoftClip
  compensation). Oracle: same quad at FAR distance, monotonic pull toward haze color.
- **S6 — Auto-exposure (optional, gated).** Port `exposureF.glsl` to drive `exp_scale`, behind
  `render_dynamic_exposure_enabled`. Only after S1–S5 eyeballed.

Then: **#2 shadows** (port `shadow_engine.rs`, plan `wkolsrwrx`) and **#3 water**.

## Port map: VERBATIM vs ADAPT

- **VERBATIM** (pure math): PBRNeutral, ACES-Hill/RRTAndODTFit/ACES matrices, `srgb_to_linear`/
  `linear_to_srgb`, `legacyGamma`, `ambientLighting`.
- **ADAPT** (only file-scope uniforms → UBO members, body unchanged): `calcAtmosphericVars` +
  `calcAtmosphericVarsLinear`, `atmosFragLighting`, `skyV` (done).
- **ADAPT WITH CARE** (depth): `getPositionWithDepth` — raw reverse-Z depth + `inv_proj_fix`,
  flip sky test.
- **Gotchas:** forward-decl param-name swap `(out atten,out additive)` vs def `(out additive,
  out atten)` — use the DEFINITION order. `skyV`'s integral legitimately differs (`+rel_pos_norm.y`
  airmass, no `distance_multiplier`, above/below-cloud sqrt-blend, moon `·0.7`) — do not "fix" it.
