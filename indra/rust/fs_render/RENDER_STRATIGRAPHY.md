# Firestorm render substrate — living stratigraphy

> Foundation for the Vulkan/wgpu reimplementation. The viewer is ~20 years of rendering **strata**:
> every subsystem is **bolted on top of** older legacy layers. This document excavates the WHOLE
> substrate as one connected system — layers, the **fossil seams** between them, and the
> cross-dependency edges — grounded in file:line. **No render intervention is made without the full
> column understood.** Nothing here is "minor"; everything is relevant.
>
> **Stratigraphic order is load-bearing** — a newer layer DEPENDS ON the one beneath it; inverting the
> order inverts the dependency. Named columns, bottom (oldest) → top (newest):
> - **Texturing:** legacy texturing → materials (Blinn-Phong) → PBR (GLTF)
> - **Lighting:** legacy day/night → WindLight → EEP → HDR
>
> Status: EXCAVATION COMPLETE — all 7 clusters dug + integrated (2026-08-04), crux seams source-verified.
> Cross-dependency map + load-bearing open questions below. Interventions are chosen FROM this map, done
> holistically over each full column; open questions in a column are resolved within its intervention.

## Legend
- **Stratum** — a historical layer. **Seam** — the compatibility bolt joining two strata (e.g.
  `RenderSkyAutoAdjustLegacy`, `hdr_scale=sqrt(gamma)*2`, `RenderHDREnabled` path forks). **Edge** — a
  runtime dependency (X reads Y's output). Every claim carries `file:line`.

---

## 1. Skeleton — frame construction, pass order, render targets, GL abstraction, VkBridge seams
_Dug 2026-08-04. Four strata bottom→top + a VkBridge overlay._

- **S0 GL immediate-mode substrate (llrender/):** `LLRender gGL` CPU matrix stacks (llrender.h:583, 32-deep), `begin/vertex/end`→cached VBO in `flush()` (llrender.cpp:1759-1854); `LLTexUnit`; **`LLRenderTarget`** = the FBO primitive everything allocates (caller-supplied color format, depth hard-wired GL_DEPTH_COMPONENT24 texture, max 4 attach, `shareDepthBuffer`, RT-stack via mPreviousRT). Fossils: `eTextureColorSpace{TCS_LINEAR,TCS_SRGB}` declared-unused (sRGB migrated up to LLImageGL formats + srgbF.glsl); `glClampColor` resolved-never-called.
- **S1 legacy forward = REMOVED FOSSIL:** `renderGeom()` gone; only `renderGeomDeferred/PostDeferred/Shadow` remain. Surviving fossils: `RenderDeferred=true // DEPRECATED` hard-wired (pipeline.cpp:1229, llappviewer.cpp:660), and the **pool render-order enum** (lldrawpool.h:57-79): SKY→WATEREXCLUSION→WL_SKY→SIMPLE→FULLBRIGHT→BUMP→MATERIALS→GLTF_PBR→TERRAIN→GRASS→…→AVATAR→GLOW→ALPHA_PRE_WATER→WATER→ALPHA_POST_WATER→ALPHA.
- **S2 deferred graft:** the G-buffer `mRT->deferredScreen` (addDeferredAttachments pipeline.cpp:381) + `renderDeferredLighting()` (:9653) resolve → HDR `mRT->screen`. Hosts the lighting sub-column (§2).
- **S3 HDR wrapper:** gated by ONE seam in 3 places — `hdr = GLVersion>4.05 && RenderHDREnabled` (pipeline.cpp:395/912/9195). hdr → screen FP16 + norm/emissive attach upgrade + renderFinalize(luminance→exposure→tonemap→CAS); !hdr → single gammaCorrect.
- **S4 VkBridge overlay (fork-only, orthogonal):** observes S0-S3, replaces the PRESENT. `FS_ENGINE_MODE=1`→`liveActive()`. **initGLFromVulkan hardcodes GL 4.6** (llgl.cpp:1082) so the HDR gate passes with no GL context. Present: `swapBuffers`→`s_fsr_end_frame()` renders live 3D+UI and presents to HWND, **skips GDI SwapBuffers** (llwindowwin32.cpp:4100-4113). Suppress machinery `sSuppressDepth` (one guard, fsscenedump.cpp:519) + the ONE hard DEFLATE `generateSunShadow` early-return (pipeline.cpp:11108). Note: the GL draw-tap is RETIRED by default (sTapDraws only under FS_TAP_DRAWS); live mode feeds TYPED scene + honest UI-flush + palette/texture.

**Frame order (display() llviewerdisplay.cpp:511):** onFrame+typed feed (:513-710) → probes → cull → generateSunShadow(:1179, DEFLATED) → impostor bake → stateSort/updateSky → G-buffer clear-magenta+renderGeomDeferred(:1341-1386) → renderDeferredLighting(:1408) → render_ui→renderFinalize(:1880)+UI → swap(:1422).

**Render targets (writer→reader):** deferredScreen = G-buffer [fd0 albedo GL_RGBA / fd1 spec-or-ORM GL_RGBA / fd2 normal GL_RGBA16|RGB10_A2 / fd3 emissive GL_RGB16F (RenderEnableEmissiveBuffer) / depth24 owned+shared]; **screen GL_RGBA16F always**; oit [accum RGBA32F / revealage R16F]; deferredLight (sun/SSAO lightmap); shadow[4]+mSpotShadow[2] (depth); luminance 256²R16F-mipped / exposure 1×1 / lastExposure 1×1; postPing/Pong (8-bit); glow[3]; sceneMap (SSR, pre-gamma); waterDis/waterExclusion(R8); pbrBrdfLut RG16F 512²; uiScreen; mSkySH. **RT-pack switching:** probes/hero re-point `mRT` to mAuxillaryRT (16× supersample) / mHeroProbeRT and re-run the WHOLE deferred pipeline per face.
- **Open:** FSSceneDump::endFrame vestigial (real present is window-layer s_fsr_end_frame); mSkySH writer unlocated; FsrEepSkyBlock tail zero-filled (sky parity trap); non-Windows present path (llwindowsdl2) unread.

---

## 2. Atmospherics + lighting substrate — the magnitude PRODUCERS  [lighting column]
_Dug + key seams source-verified 2026-08-04. Emits `sunlit`/`amblit`/`additive`/`atten` + EEP scalars
that HDR/exposure, sky, water, clouds, terrain all read. Two hand-synced twins: CPU
`LLSettingsSky::calculateLightSettings` (llsettingssky.cpp:1707) + GPU `calcAtmosphericVars`
(atmosphericsFuncs.glsl:51) — keep-in-sync-by-hand is itself a fossil seam._

**Order bottom→top (dependency = upper reads lower):**
- **L0 legacy day/night** — surviving fossils: sun/moon dir from quats (llsettingssky.cpp:1336), `getIsSunUp` = `sunDir.z>=0` (:1324), legacy unit conversions (star ×250 :1067, cloud scroll −(10,10) :1029). **Seam L0→L1:** `sun_up_factor` selects sunlight vs moonlight color (atmosphericsFuncs.glsl:62).
- **L1 WindLight** — the scatter/magnitude model. Defaults (the calibration baseline everything assumes): blue_horizon (0.4954,0.4954,0.6399), blue_density (0.2447,0.4487,0.7599), haze_density 0.7, haze_horizon 0.19, density_mult 0.0001, distance_mult 0.8, ambient (0.25,0.25,0.25), **gamma 1.0**, max_y 1605, glow (5.0,0.001,−0.4799), sunlight (0.7342,0.7815,0.8999) — llsettingssky.cpp:900-919,1227-1233. **The WL "×2" fossil**: `additive*2.0` (atmosphericsF.glsl:38), `mSceneLightStrength=2.0` baseline (llsettingsvo.cpp:676).
- **L2 EEP** — wraps (not replaces) WL as an asset; **owns gamma**. **Seam L1→L2:** the LEGACY_HAZE inner-map dual-read `get_float(use_legacy,…)` (llsettingssky.cpp:1136) + `translateLegacyHazeSettings` (:951).
- **L3 HDR/PBR** — **Seam L2→L3 = the master branch `classic_mode = canAutoAdjust() && !should_auto_adjust()` (llsettingsvo.cpp:810, VERIFIED).** `canAutoAdjust` = sky has NO authored reflection-probe ambiance (llsettingssky.cpp:1175); `should_auto_adjust` = `RenderSkyAutoAdjustLegacy` (default false). Three-way `SKY_HDR_SCALE`: PBR-native sky → `sqrt(gamma)*2` (:834); auto-adjust-legacy → 2.0 + param scales (:843); **classic → 1.0 (:856)**. Legacy-sky HDR getters neutralize: `getHDRMin/Max→0`, `getHDROffset→1`, `getTonemapMix→0` (llsettingssky.cpp:2042-2075) ⇒ **a classic sky gets exposure range [1,1] and NO tonemap.**

**Magnitude flow** (atmosphericsFuncs.glsl:51-165): sunlight attenuation (Beer, :66-76) → transmittance `atten` (:84) → haze glow (:90-108) → cloud-lifted ambient (:113) → `additive` haze combine (:119-120) → outputs `sunlit`, `amblit = pow(tmpAmbient,0.9)*0.57` (:125), `additive=min(.,10)` (:130). Then `calcAtmosphericVarsLinear` (:147-165): if `classic_mode<1` → `srgb_to_linear(amblit)` + **desaturate ambient to luminance** `dot(.,0.2126/0.7152/0.0722)` (:157) + `srgb_to_linear(sunlit)`; `sunlit *= sky_sunlight_scale`; `amblit *= sky_ambient_scale`. softenLightF (class3 only): classic sun ×1.35 (:160), SKIP_ATMOS WL sky `srgb_to_linear(color)*sky_hdr_scale` (:203), final ×1.1 if classic (:281).

**Scale table (magic constants):** sky_sunlight_scale 1.5 / sky_ambient_scale 1.5 (llsettingsvo.cpp:805-806); sky_hdr_scale = sqrt(gamma)*2 | 2.0 | 1.0; **PBR sun ×3.0** (deferredUtil.glsl:645) + PBR local-light ×3.0 (:580); classic sun ×1.35, classic final ×1.1, classic local ×0.9; amblit pow(.,0.9)*0.57; clampHDRRange [0,11.2] (deferredUtil.glsl:90); auto_adjust_ambient_scale 0.75.

**PRODUCES →** lit HDR linear scene in mRT->screen (clamped [0,11.2]) consumed by generateExposure/tonemap; `sunlit/amblit/additive/atten` consumed by every lighting shader; EEP uniforms (sky_hdr_scale, gamma, probe_ambiance, sun/moon dir, blue_density/horizon, glow, cloud_*) consumed by sky/cloud/water/terrain. **READS ←** EEP sky asset + gSavedSettings (Render Sky/HDR/Exposure/Tonemap) + G-buffer + shadow/SSAO lightMap + reflection probes.

**Open seams:** (1) TWO inconsistent classic_mode defs — softenLight uses `canAutoAdjust && !should_auto_adjust` (llsettingsvo.cpp:810) but local-light passes push `CLASSIC_MODE = canAutoAdjust ? 1:0` (pipeline.cpp:9945,10007,10047,10084) → with auto-adjust ON, softenLight runs non-classic while local lights get classic=1 → calibration divergence. (2) HDRMin/Max/Offset/TonemapMix HARDCODED at load (0.5/2.0/1.0/1.0, llsettingssky.cpp:1185-1188), NOT read from the EEP asset. (3) probe_ambiance consumed in-shader only at class2 (reflectionProbeF.glsl:40); class3 ignores it in-shader → same EEP value, different magnitude per graphics level. (4) gammaF.glsl soft-clip = dead no-op (:41). (5) CPU twin omits the pow(0.9)*0.57 + srgb→luminance shaping → must pick canonical. (6) **faithful-camera graft** (RenderFaithfulCamera) sits atop this — non-stock; flag when calibrating vs upstream.

---

## 3. HDR / exposure / tonemap + post FX — the magnitude CONSUMERS  [top of lighting column]
_Dug 2026-08-04. Pure consumer: reads the lit HDR scene `mRT->screen` (RGBA16F, pipeline.cpp:983), meters
it, applies a film curve, emits LDR sRGB for the UI to draw over. NO motion blur exists (verified absent).
SSAO/SSR are UPSTREAM (computed in renderDeferredLighting, modulate ambient before mRT->screen) — not post._

**Seams (top of the lighting column L3):**
- **S1 HDR fork (pipeline.cpp:9194):** `hdr = GLVersion>4.05 && RenderHDREnabled`. HDR → luminance→exposure→tonemap→(CAS). non-HDR → single `gammaCorrect` (no metering, no curve). `mRT->screen` is RGBA16F unconditionally; LDR staging mPostPing/Pong are 8-bit.
- **S2 legacy-sky bypass (pipeline.cpp:8106-8144):** the fossil test `psky->getReflectionProbeAmbiance()==0` (= legacy sky) forks `no_post` and `legacy_gamma`. Stock: a legacy sky → `no_post` → NO ACES/PBR curve, exposure-only. **FIRESTORM GRAFT `faithful_camera` (RenderFaithfulCamera)** overrides: `!faithful_camera && ambiance==0` → forces the full film for legacy skies too. This master IS on `fs/faithful-camera`.
- **S3 gamma placement hazard:** sRGB+legacyGamma baked into postDeferredTonemap.glsl:34-71; old windlight gammaF.glsl:41 is a DEAD identity no-op. **Where gamma happens flips with CAS:** CAS off → tonemap bakes sRGB; CAS on → tonemap emits linear, applyCAS does linear_to_srgb (CASF.glsl:2567). A port hardcoding gamma one place double/zero-applies when CAS toggles.
- **S4 exposure-bound bridge (pipeline.cpp:8058-8090):** default (`RenderUseExposureSkySettings`=false) → bounds from `hdr_scale=sqrt(gamma)*2` ONLY if `probe_ambiance>0`; else `exp_min=exp_max=1`. Opt-in → `getHDROffset ± getHDRMin/Max`.
- **S5 G-buffer flag env-mask (llshadermgr.cpp:634-640):** `SKIP_ATMOS 0.0 / HAS_ATMOS 0.34 / HAS_PBR 0.67 / HAS_HDRI 1.0`, `GET_GBUFFER_FLAG(d,f)=abs(d-f)<0.1`. Sky writes HAS_HDRI/SKIP_ATMOS → excluded from metering.

**THE EXPOSURE LOOP (exact):**
- `luminanceF.glsl:40-67` — sample `tc=vary_fragcoord*0.6+0.2; tc.y-=0.1` (center, ground-biased); if NOT sky-flag → `c *= diffuse_luminance_scale` (RenderDiffuseLuminanceScale=1.0); `+ emissive(mGlow[1], last frame)`; `L=dot(c,0.2126/0.7152/0.0722)`. Output 256²R16F, auto-mipped (pipeline.cpp:1700).
- `exposureF.glsl:47-65` — `L=textureLod(lum,0.5,8)` (mip8=1×1=frame avg); `max_L=0.175`; `L=clamp(L,0,0.175)/0.175; L=pow(L,2)`; **`s = mix(exp_max, exp_min, L)`** ⇒ **dark frame → brighten, bright frame → darken**; temporal `s=mix(prev,s,1-exp(-1.1513*dt))` from mLastExposure (1×1). Note: `exp_min=exp_max=1` for a classic sky ⇒ **NO dynamic exposure**.
- `tonemapUtilF.glsl:116-155` `toneMap` — `exp_scale = faithful_camera? 1.0 : exposureMap.r`; `final_exposure=clamp(RenderExposure,0.5,4)*exp_scale`; `tonemap_type 0=PBRNeutral(Khronos, default) / 1=ACES-Hill`; `color=mix(exposed_linear, tonemapped, tonemap_mix)`. Classic sky → `tonemap_mix=0` ⇒ **pure exposed-linear, curve disabled.**

**Post FX order (renderFinalize pipeline.cpp:9173, once/frame @ llviewerdisplay.cpp:1880):** SSR-copy(feedback) → generateLuminance → generateExposure → tonemap → CAS(RenderCASSharpness .4) → generateGlow → combineGlow(bloom, LDR) → DoF → FXAA(type1)/SMAA(type2) → RLV/vignette/snapshot-frame(FS grafts) → final present + film noise. SSAO/SSR live upstream in lighting; motion blur absent.

**BRIDGE DEFECT (the current mess):** `FSSceneDump::setSceneRegime` (llviewerdisplay.cpp:570-584) ships the **static** `RenderExposure` (=1.0, :582) + tonemap regime, but NEVER the converged per-frame `mExposureMap.r`. So the engine (a) substitutes 1.0 for `s=mix(exp_max,exp_min,L²)`, (b) meters only its partial scene (not the ground-biased/sky-excluded full mRT->screen), (c) has no temporal smoothing → flashing. **A faithful fix reproduces the WHOLE loop AND respects classic-vs-PBR sky** (classic → exp=1 fixed + no curve; PBR → dynamic + curve).

**Open seams:** faithful_camera (forces exp_scale=1 = fixed camera) CONTRADICTS feeding a dynamic meter — the engine may already be de-facto `faithful_camera=true`, so the "static 1.0" might be half-intended; decide per-target. Vestigial `dynamic_exposure_params2.x=getHDROffset` uploaded but unread. RenderUseExposureSkySettings default false ⇒ EEP HDRMin/Max/Offset ignored by default.

---

## 4. Sky + clouds + water
_All three ride on the §2 atmospherics substrate. Dug 2026-08-04._

### 4a. SKY (legacy sky pool is a HOLLOW fossil; all sky via LLDrawPoolWLSky)
- **THREE parallel copies of the haze math, hand-synced (fossil seam):** GLSL `calcAtmosphericVars` (atmosphericsFuncs.glsl:51) + CPU `LLSettingsSky::calculateLightSettings` (llsettingssky.cpp:1707) + CPU sky-dome twin `calcSkyColorWLVert` (lllegacyatmospherics.cpp:260). **The CPU sky-dome twin is largely DEAD** — LLVOSky::updateSky early-outs when reflection probes are on (llvosky.cpp:707, the default).
- Legacy `lldrawpoolsky.cpp:31 // DEPRECATED` — all methods empty stubs. Live path = `LLDrawPoolWLSky::renderDeferred` (lldrawpoolwlsky.cpp:477): haze dome → sun+moon → stars → clouds. Programs gDeferredWLSky/Cloud/Sun/Moon/Star (group SG_SKY).
- **Dome:** renderDome (Y-up rotate + scale 0.333), drawDome (radius 15000), depth-write OFF. **G-buffer flags:** sky dome writes `SKIP_ATMOS` (skyF.glsl:117) or `HAS_HDRI` (:97, ×sky_hdr_scale); sun/moon/stars write `SKIP_ATMOS`. **Depth-clear-for-haze-mask trick:** endDeferredPass clears depth (lldrawpoolwlsky.cpp:92) → the deferred hazeF discards depth>=1.0 (sky pixels already have haze baked into vary_HazeColor at dome-render). Halos/rainbows in skyF (rainbow/halo22 from moisture/droplet/ice).
- Sun/moon discs (legacy LLVOSky FACE_SUN/MOON) carry DUAL textures (DIFFUSE + ALTERNATE) for keyframe A/B cross-fade; color via getInterpColor.

### 4b. CLOUDS
- Same dome; two-texture day-cycle blend (CLOUD_NOISE_MAP / CLOUD_NOISE_MAP_NEXT, `blend_factor=getBlendFactor()`, cloudsF.glsl:51). `cloud_shadow` = master density/darkening knob (`vary_CloudDensity=2*(cloud_shadow-0.25)`, cloudsV.glsl:184; also lifts ambient + dims sunlight in the substrate). cloudsV computes CloudColorSun/Ambient from the full substrate. Writes SKIP_ATMOS. EEP cloud X-flip fossil (SL-13084). Clouds EXCLUDED from probe irradiance (anti-popping).

### 4c. WATER (deferred/PBR ONLY; legacy forward water is a magenta error stub)
- **No planar reflection pass:** `generateWaterReflection`/`mWaterRef`/`mRT->water*` DO NOT EXIST. Reflections = reflection probes + hero/mirror probe (`sampleReflectionProbesWater`, class3 reflectionProbeF.glsl:781; mirror = mHeroProbeManager mirror pass). The only water RTs are **`mWaterDis`** (full-res screen-HDR-format + depth, "always needed as scratch") and **`mWaterExclusionMask`** (R8). updateCull sets only a user CLIP plane at water height (pipeline.cpp:2655-2688), no mirror matrix in deferred.
- **Pass (post-deferred, camera <1024m, lldrawpoolwater.cpp):** `beginPostDeferredPass` copies screen color + deferredScreen depth → mWaterDis via gCopyDepthProgram (:112-144). `renderPostDeferred` (:146-354): `gWaterProgram` / `gUnderWaterProgram` (underwater branch only — legacy/deferred fork gone); binds 2 blended normal maps (BUMP_MAP/BUMP_MAP2 + BLEND_FACTOR), `mWaterDis` as `screenTex`(WATER_SCREENTEX)+depth (via bindDeferredShader 3rd arg), `mWaterExclusionMask` as `exclusionTex`; pushes the full water param block + **exposure/tonemap_type/tonemap_mix + classic_mode** (HDR seam, :284-287). VkBridge taps the whole uniform payload into aux F4 slots a0-a7 (:318-346) — exact constant block for the port.
- **Geometry:** LLVOWater, tessellated grid (8×8 quads for transparent water, region-sized, flat Z); horizon warp/clamp (2560m) in the VERTEX shader (class1/environment/waterV.glsl), not C++.
- **Shaders:** vertex ALWAYS class1 waterV; fragment class3 waterF (above) / underWaterF (below); class1 waterF = magenta stub; shared fog lib class1 waterFogF; deferred fog pass class3 waterHazeF/V (gHazeWaterProgram). Composite (waterF): wave normals (2-map blend) → distortion (screen-space refraction offset from refCoord + wavef·refScale) → fresnel `df` → `calcAtmosphericVarsLinear` (substrate) → refraction sample `screenTex` at distort2 + depth-reject foreground → PBR probe reflection (metallic=1, roughness=blurMultiplier) → punctual sun spec → **`color = mix(fb.refraction, radiance.reflection, df2.x) + punctual`** (fresnel-weighted) → water fog. Underwater: fog applied unconditionally.
- **Haze/exclusion order (renderGeomPostDeferred pipeline.cpp:4402):** exclusion-mask (draw water planes white=1, invisiprims black=0) → (underwater: atmospherics) → water-haze (gHazeWaterProgram, re-renders water patches above-water) → water pool draw → (above-water: atmospherics) → post-water alpha. All re-copy screen→mWaterDis first; blend `BF_ONE, BF_SOURCE_ALPHA` (fog model).
- **Substrate seam:** `calcAtmosphericVarsLinear` — classic_mode<1 → ambient linearized + desaturated to luminance + sunlit linearized; classic → stays sRGB/colored. Water consumes sunlit/amblit/atten/additive.
- **Open:** mWaterImagep/mOpaqueWaterImagep populated but never bound (dead?); renderOpaqueLegacyWater declared-undefined; SHORELINE_FADE compiled out (fade=1); mWaterDis TRIPLE-purposed (refraction copy / haze scratch / depth target) — watch lifetime when splitting into wgpu images; calcAtmosphericVarsLinear prototype param-name swap (additive/atten) in waterF.glsl:37 (links, but mirror carefully).

### 4d. DAY-CYCLE DRIVE + **twilight-nuance RESOLVED**
- `LLEnvironment::update()` (llenvironment.cpp:1759) every frame: `applyTimeDelta` → `updateSettingsUniforms` → mark ALL shaders dirty. **No dirty-gating; blend + uniform recompute run unconditionally per frame.**
- **Blend = piecewise-LINEAR in time** (convert_time_to_blend_factor, llenvironment.cpp:182; LLTrackBlenderLoopingTime). `LLSettingsSky::blend` (llsettingssky.cpp:577-691): sun/moon rotation → **slerp**; colors + all scalars (gamma, cloud_shadow, glow, ambient, maxY, star/moon brightness…) → **lerp**; legacy haze (blue_density/horizon, haze, multipliers) → lerp_legacy; **textures SNAP** (A/B two-texture cross-fade instead); atomics hard-switch at 0.5.
- **★ TWILIGHT RESOLUTION:** the DEFAULT day keyframes ONLY sun/moon ROTATION (defaults(), llsettingssky.cpp:892; all colour params constant across keyframes). **Twilight colour EMERGES from the scattering math responding continuously to the smoothly-slerped `lightnorm` (sun dir), re-evaluated per frame** — NOT from keyframed colours. So our engine's "binary day/night" = one of: (a) snapping lightnorm to keyframe values instead of feeding the per-frame slerped direction; (b) not re-evaluating calcAtmosphericVars/calculateLightSettings per frame from the interpolated lightnorm; (c) not re-pushing the atmospherics uniforms each frame. **The fix is per-frame atmospherics re-evaluation from a smoothly-moving sun dir, NOT adding keyframe colour blending.** (SKYDIAG shows our sundir DOES sweep — so suspect (b)/(c): the magnitudes/colours aren't re-derived from it.) Latent quirk: `defaults(position)` caches into a function-static guarded by size()==0 → position only affects the FIRST call; the built-in fallback day's sun rotation freezes (would itself give a binary sky on fallback).

---

## 5. Terrain + objects/prims + materials  [texturing column]
_Dug 2026-08-04. TWO columns share ONE 4-target G-buffer. **Tier is decided PER-FACE from the
`LLTextureEntry` at batch-build (NOT cached on LLVOVolume — corrects an earlier assumption):**
`getGLTFRenderMaterial()`→PBR, else `getMaterialParams()`→Blinn-Phong, else legacy._

**Object column (bottom→top):** legacy texturing (1 diffuse on DIFFUSE_MAP, TE offset/scale/rot, emboss bump, env shiny) → **Blinn-Phong** (LLMaterial adds normal+specular map UUIDs + independent transforms + env/spec/cutoff; diffuse stays the legacy face texture; +2 texcoord streams+tangents) → **PBR/GLTF** (LLGLTFMaterial: 4-slot baseColor/normal/ORM/emissive, OCCLUSION aliased onto METALLIC_ROUGHNESS, per-info KHR transforms + factors).
**Terrain column (bottom→top):** legacy 4-detail splat (gDeferredTerrainProgram, class1 only) → PBR terrain (gDeferredPBRTerrainProgram[paint_type], up to 4 GLTF swatches).

**Fossil seams:** A1 pool fork `getPoolTypeFromTE` (pipeline.cpp:1948-1994; legacy BUMP tested BEFORE gltf → rescued by override llvovolume.cpp:6202). A2 tri-fork bucket (llvovolume.cpp:6289-6350). A3 PASS dispatch "**ignore traditional material if GLTF present**" (genDrawInfo :6910-6952; BP sub-tier via 12-entry getShaderMask LUT). **A4 texture-transform fork (the load-bearing one): legacy+Blinn-Phong BAKE 3 CPU texcoord streams (XFORM_BLINNPHONG_COLOR/NORMAL/SPECULAR, llface.cpp:1916-1966); GLTF SKIPS CPU baking, applies KHR transform in the vertex shader** (llface.cpp:1543-1545,1773-1774; pack [0].xy=scale/.z=rot/[1].xy=offset, llgltfmaterial.cpp:84). B1 terrain fork `getMaterialType()==TEXTURE` → splat, else PBR[paint_type] (lldrawpoolterrain.cpp:219-243). B2 paint-type program array (HEIGHTMAP_WITH_NOISE=0 / PBR_PAINTMAP=1) × TERRAIN_PBR_DETAIL degradation level. B3 paint-source: alpha-ramp vs client-baked paintmap (:527-547).

**G-buffer OVERLOAD (one buffer, tier-dependent semantics, disambiguated ONLY by fd2.w flag):**
| target | legacy/Blinn-Phong (HAS_ATMOS 0.34) | PBR (HAS_PBR 0.67) |
|---|---|---|
| fd0 | **sRGB** diffuse (+emissive in .a for BP) | **LINEAR** base color |
| fd1 | spec color+exponent(.a) | ORM (R=occ,G=rough,B=metal) |
| fd2 | normal.xy + env-intensity in .b | normal.xy + .b=0 |
| fd3 | 0 | linear emissive |

`encodeNormal` = Lambert-azimuthal spheremap into .xy, flag into .w (globalF.glsl:46). Writes: legacy diffuseF.glsl:44, BP **class3** materialF.glsl:448 (class1 = magenta debug stub, no class2), PBR pbropaqueF.glsl:115, PBR terrain **pbrterrainF.glsl:431** (fd1.a = base_color_factor_alpha, ORM degrades to "potato" (1,1,0)). **The sRGB-vs-linear fd0 seam is load-bearing** — softenLightF.glsl:167 forks on the flag: HAS_PBR → pbrBaseLight (ORM BRDF+IBL, sun ×3), else `srgb_to_linear(baseColor)` + legacy Blinn-Phong (spec.a/env, sun ×1.35 classic).

**READS:** UUID→LLViewerTexture via getFetchedTexture; boost `BOOST_TERRAIN`/`setBoostLevel>=BOOST_HIGH` pins 2048²+discard0 (llviewertexture.cpp:833); GLTF maps fetched BOOST_NONE 64² floor. **WRITES:** the shared 4-MRT G-buffer. **Texture batching disabled whenever any material present** (llvovolume.cpp:5721).

**Relevance to our PBR-terrain work (validates + roadmap):** our engine's flag routing (HAS_ATMOS 0.34 / HAS_PBR 0.67) + LINEAR base + ORM matches stock. Deferred pieces confirmed as real strata: 4-target G-buffer (fd1 ORM + fd3 emissive), the paintmap paint-type, per-swatch KHR transforms, the ORM potato-degradation, factor arrays. Our base-color-first is the correct bottom of the PBR terrain stratum.
- **Open:** getPoolTypeFromTE BLEND-GLTF+bump ordering; class1 materialF magenta if class≤2 resolves; PBR fd1.a inconsistency; HAS_EMISSIVE guard commented out (pbrmetallicroughnessF.glsl:248 latent MRT mismatch); impostorF raw-normal exception; BP-scalar-vs-KHR never combined.

---

## 6. Meshes + rigged + avatars + alpha/OIT + shadows
_Dug 2026-08-04. Five strata bottom→top: S0 static mesh → S1 legacy avatar skinning → S2 rigged-mesh skinning → S3a shadows / S3b legacy sorted alpha → S4 WBOIT. F-seam (VkBridge) cuts across._

- **S0 static mesh:** LLVOVolume face→LLDrawInfo→pass. Rigged fork: `rigged = skinInfo && isAttachment()` stamps `LLFace::RIGGED`+mSkinInfo+mAvatar (llvovolume.cpp:6088-6172); skinInfo from gMeshRepo (:1339).
- **S1 legacy avatar skinning** (oldest skinning graft): base "system avatar" via LLDrawPoolAvatar→renderSkinned (lldrawpoolavatar.cpp:403+). `avatarSkinV.glsl:27-42`: `matrixPalette[45]` (15 joints×3 rows), SINGLE weight, `mix` between adjacent joints. **Seam S1→S2:** same `matrixPalette` name + `floor/fract(weight)` convention.
- **S2 rigged skinning** (descendant): rigged attachments flow through NORMAL pools' rigged shader VARIANTS (`make_rigged_variant`+HAS_SKIN, llviewershadermgr.cpp:253; attaches objectSkinV.glsl, llshadermgr.cpp:168). `objectSkinV.glsl:27-63`: `mat3x4 matrixPalette[MAX_JOINTS]`, FOUR weights, renormalized. Palette: build llvoavatar.cpp:11244 (`mat=invBind×world`, llskinningutil.cpp:146), pack mGLMp 12f/joint, upload `uniformMatrix3x4fv(AVATAR_MATRIX)` + tap `setMatrixPalette` (lldrawpool.cpp:676-686).
- **S3a shadows:** generateSunShadow (pipeline.cpp:11103) — 4-cascade CSM (sphere-bound+texel-snap stable fit :11535), +2 spot (detail>1). Consumed by sampleDirectionalShadow (shadowUtil.glsl:96, 5-tap PCF, cascade select by view-z) in the sun lightmap pass. Default RenderShadowDetail=2.
- **S3b legacy sorted alpha:** per-group depth + 0.64 view-angle re-sort (llspatialpartition.cpp:669-704).
- **S4 WBOIT** (over S3b): RenderAlphaOIT (default on) — accum(RGBA32F)/revealage(R16F)/composite (pipeline.cpp:5214-5266); weight `oit_w=a*clamp(0.03/(z/200)^4,...)` (alphaF.glsl:324). A thin `oit_mode` switch over the SAME alpha shaders; POST_WATER main-view only.
- **F-seam DEFLATE (VkBridge):** `generateSunShadow` returns under `liveActive()` (pipeline.cpp:11108, VERIFIED) → NO CSM/spot maps generated; AND WBOIT forced OFF → S3b sorted painter stream (lldrawpoolalpha.cpp:219). Engine supplies shadowing + gets an ordered alpha stream.
- **READS:** skin (invBind/joints), materials/textures per DrawInfo, G-buffer depth, shadow maps→lighting, lit mRT->screen (alpha-over). **WRITES:** G-buffer (opaque static+rigged), shadow maps, alpha composite into mRT->screen, taps (palette+material).
- **Open:** deferred base-avatar CPU-vs-GPU skinning unconfirmed; within-group DrawInfo alpha comparator; MAX_JOINTS vs GLTF UBO joint ceiling reconciliation; VSM (detail>2) unpopulated.

---

## 7. UI compositing + snapshot  [top of the whole column]
_Dug 2026-08-04. Three strata: scene→G-buffer → post/tonemap+PRESENT → 2D UI over the backbuffer._

- **Order (llviewerdisplay.cpp, VERIFIED site):** `renderFinalize()` (pipeline.cpp:9173) called from `render_ui()` @ :1880 = tonemap + **present scene into FBO 0 at the world-view rect** (fullscreen tri, pipeline.cpp:9314-9328) → HUD/UI draws :1887-1942 → `swap()` :1957. UI is **ortho 2D drawn directly into FBO 0 over the presented pixels** (llrender2dutils.cpp:64 `gl_ortho`); it does NOT sample the scene as a texture (except the optional RenderUIBuffer `mUIScreen` cache, render_ui_2d :2192).
- **Cursor:** OS-managed, NO in-GL draw (llviewerwindow.cpp:2717). "Under-cursor artifact" is therefore a root-view overlay (gToolTipView / hover-pick) or a missing-scissor issue, NOT a cursor draw.
- **Minimap escaping (ROOT CAUSE):** LLNetMap::draw (llnetmap.cpp:310) clips via `LLLocalClipRect`→bare `glScissor()` (lllocalcliprect.cpp:95). But the engine UI feed **`fsr_ui_submit` carries NO scissor rect** (llrender.cpp:1833-1850; fsscenedump.h:37) → dots unbounded. Generalizes to EVERY clipped widget (scroll lists, editors, tabs, accordions). The `gGL.flush()` at each scissor change (lllocalcliprect.cpp:86) gives a natural transmit hook.
- **Toasts/chiclets:** ordinary LLViews in the root-view tree (mRootView->draw, llviewerwindow.cpp:3154; LLScreenChannel, llchannelmanager.cpp:154) — same 2D stratum + same UI feed.
- **Login tonality (ROOT):** login uses `display_startup()` (llviewerdisplay.cpp:169,873) which does `setup2DRender + draw` and **NEVER calls renderFinalize** → NO scene tonemap beneath. So login color = raw `gUIProgram`+sRGB-framebuffer output; the engine UI compositor must match the raw UI path in isolation (not the tonemapped-scene path). The A/B delta is a UI color-space question.
- **Engine handoff:** typed UI stream `fsr_ui_submit(mvp16, tex_id, mode, count, verts{pos,uv,rgba8})` at LLRender::flush (llrender.cpp:1833) + cached-font path (llfontvertexbuffer.cpp:239). Frame bracket `onFrame`→`fsr_begin/scene_begin/ui_begin` (fsscenedump.cpp:1129) … `swapBuffers`→`s_fsr_end_frame()` (llwindowwin32.cpp:4100) which renders live 3D queue + UI list + presents to HWND, **skipping GDI SwapBuffers**. mUIScreen forced full-dirty each frame under liveActive (llviewerwindow.cpp:2989).
- **Snapshot:** rawSnapshot `glReadPixels` from FBO 0 (llviewerwindow.cpp:6387) — reads the null-GL stub in engine mode; no engine-mode capture path exists (our fsr_capture.req is a separate engine facility).
- **Open:** UI-vs-engine composite ordering is IMPLICIT (append order in one onFrame→end_frame bracket), not enforced — confirm end_frame tonemaps 3D before UI + never tonemaps UI; missing-scissor (minimap+widgets+cursor artifact); login color-space; engine-mode snapshot unbuilt; fsr_ui_submit lacks scissor + blend-mode fields.

---

## Cross-dependency map (the shared substrate edges)
_All 7 clusters dug + integrated 2026-08-04. The connected system, following the real edges:_

```
day-cycle (§4d) ──per-frame slerped lightnorm + lerp'd params──▶ ATMOSPHERICS SUBSTRATE (§2)
                                                                    │ produces sunlit/amblit/additive/atten
                                                                    │ + EEP uniforms (sky_hdr_scale, gamma,
                                                                    │   classic_mode, probe_ambiance)
        ┌───────────────────────────┬───────────────────────────┬──┴────────────────────┐
        ▼                           ▼                           ▼                        ▼
   SKY/CLOUDS (§4)            softenLight lighting (§2)     WATER (§4)              legacy/BP materials (§5)
   write SKIP_ATMOS/HAS_HDRI  reads G-buffer(§5,§6) + flag  reads substrate +       read substrate (sun_contrib)
        │                     routing → lit mRT->screen     mWaterDis screen-copy
        │                           │  (HDR magnitudes)          │
        │                           ▼                            │
        └──sky_hdr_scale + ──▶ HDR EXPOSURE METER (§3) ◀─────────┘ (sky drives whole-frame tone;
           SKIP_ATMOS pixels    luminanceF ground-biased,          sky EXCLUDED from diffuse_luminance_scale)
                                sky-excluded → exposureF → tonemap → present (§1/§7)
                                                                       │
   G-buffer producers (§5 terrain/materials, §6 meshes/rigged) ──▶ softenLight(§2)   ▼
   shadows (§6, DEFLATED in VK) ──▶ softenLight(§2)                              UI COMPOSITE (§7)
   sky+clouds+water+terrain ──▶ reflection probes ──▶ all PBR surfaces (§5,§6,§4)  draws over tonemapped
   VkBridge (§1 S4) ──typed scene + UI feed──▶ engine; suppress/deflate cuts        backbuffer; NO scissor fed
```

**The load-bearing shared edges a faithful Vulkan port MUST honor (not any single layer):**
1. **`classic_mode` (llsettingsvo.cpp:810)** forks magnitudes AND exposure AND tonemap AND sky_hdr_scale AND ambient-linearization — one seam, whole-frame consequence. Classic sky → fixed exposure[1,1] + no curve + sun ×1.35 + colored sRGB ambient; PBR sky → dynamic exposure + curve + ×3.0 + linear luminance ambient. **Our engine ignores this → the overexposure.**
2. **Per-frame atmospherics re-evaluation from the slerped lightnorm** — the source of ALL colour incl. twilight. **Our engine's binary day/night = this edge broken (magnitudes not re-derived per frame from the moving sun).**
3. **Sky → exposure** — sky_hdr_scale + the ground-biased/sky-excluded meter over the FULL scene. **Our engine meters a partial scene → wrong exposure + flashing.**
4. **G-buffer flag routing (fd2.w)** — one overloaded buffer, sRGB-legacy vs linear-PBR albedo disambiguated only by the flag. Governs softenLight + all light shaders.
5. **UI feed lacks scissor + blend fields** (fsr_ui_submit) — the minimap/clip/cursor family.
6. **VkBridge present replaces the swap** (llwindowwin32.cpp:4100); shadows + WBOIT deflated; the engine owns 3D→tonemap→UI ordering internally.

## Open excavation questions (load-bearing, resolve before/within the relevant intervention)
- **Exposure/HDR:** faithful_camera (forces exp_scale=1) vs a dynamic meter — the engine may be de-facto faithful_camera=true; decide. TWO inconsistent classic_mode defs (softenLight vs local-light/point/spot passes, pipeline.cpp:9945+). HDRMin/Max/Offset/TonemapMix HARDCODED at load (not read from asset). RenderUseExposureSkySettings default false ⇒ EEP HDR fields ignored by default. CAS-branch gamma placement (double/zero-apply on toggle). SG_SKY-vs-SG_ANY sunlight_color asymmetry.
- **Day-cycle:** which env is live (getCurrentSky vs mCurrentEnvironment->getSky) can differ in edit mode; defaults(position) function-static freeze on fallback.
- **Materials/terrain:** getPoolTypeFromTE BLEND-GLTF+bump ordering; class1 materialF magenta if class≤2 resolves; PBR fd1.a inconsistency (pbropaque 0 vs pbrterrain base_color_factor_alpha); HAS_EMISSIVE guard commented out (latent MRT mismatch); impostorF raw-normal; BP-per-map-scalars never routed through KHR.
- **Meshes/alpha/shadows:** deferred base-avatar CPU-vs-GPU skinning; within-group DrawInfo alpha comparator; MAX_JOINTS vs GLTF UBO joint ceiling; VSM (detail>2) unpopulated.
- **Water:** mWaterImagep/OpaqueWater + renderOpaqueLegacyWater dead?; SHORELINE_FADE compiled out; mWaterDis triple-purposed (lifetime hazard when split into wgpu images); no planar reflection RT (probes only — port must implement sampleReflectionProbesWater + screen-copy refraction).
- **UI/skeleton:** engine composite ordering IMPLICIT (confirm end_frame tonemaps 3D before UI, never tonemaps UI); engine-mode snapshot unbuilt (glReadPixels reads null-GL FBO0); FSSceneDump::endFrame vestigial; mSkySH writer unlocated; FsrEepSkyBlock tail zero-filled (sky parity trap); non-Windows present path unread.
