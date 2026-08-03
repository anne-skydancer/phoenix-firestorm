# Ground + Shadows — implementation plan (FSVulkan engine)

Synthesized from a full trace of stock Firestorm's OpenGL deferred pipeline (repo:
`c:\fs\firestorm-upstream`). Goal: render lit **terrain**, then **shadows**, in the typed
Vulkan/wgpu engine (`indra/rust/fs_render`), gated + checkpointed, not big-bang.

Order (approved): **1 geometry → 2 resolve/atmosphere/exposure → 3 textures → 4 shadows.**

---

## Architecture (the seam)

Stock's lit frame is a pass chain; the **G-buffer is the interface** every stage plugs into:

```
G-buffer fill (opaque: terrain, objects)     -> 4 color RTs + shared depth
  RT0 albedo   RGBA8      (legacy sRGB / PBR linear; .a = mask/opacity)
  RT1 ORM/spec RGBA8      (legacy spec.rgb + gloss.a ; PBR .r=AO .g=rough .b=metal)
  RT2 normal   RGBA16     (.rg = encodeNormal, .b = envIntensity, .w = FLAG)
  RT3 emissive RGB16F     (only if RenderEnableEmissiveBuffer)
  depth        shared with the lit-output RT
shadow/AO pre-pass -> lightmap (.r = scol directional shadow, .g = ambocc SSAO)
softenLight resolve -> per-FLAG branch -> lit RGBA16F (our scene_hdr)
local lights (additive)
HAZE pass (SEPARATE) -> screen = additive + screen*atten     (atmosphere-on-geometry)
luminance meter -> exposure -> tonemap
```

**Per-fragment FLAG** (stored in RT2.w, matched by `abs(v-flag)<0.1`, NOT bit-OR):
`SKIP_ATMOS 0.0` (WL sky) · `HAS_ATMOS 0.34` (legacy geom) · `HAS_PBR 0.67` (GLTF) ·
`HAS_HDRI 1.0`. Injected from C++ `llrender/llshadermgr.cpp:636-640`. Our fullscreen sky
already writes the SKIP_ATMOS case conceptually.

**Two findings that shape everything:**
1. Atmosphere-on-geometry (haze/fog) is a **separate additive blend pass** (`hazeF.glsl`,
   `screen = additive + screen*atten.r`, blend `ONE,SRC_ALPHA`), NOT baked into softenLight.
2. Exposure meters the **LIT GROUND**: `luminanceF.glsl` samples the composited lit `screen`
   at `tc*0.6+0.2`, `y-=0.1` (nudge down to favor ground), sky-excluded, mip-averaged to 1x1.
   Our sky-only render has no lit ground -> meters bright sky -> the wash-out + giant sun.
   **Fixed for free once we have lit terrain to meter.**

**What we already have** (engine recon): a minimal G-buffer (`gbuf_albedo` RGBA8 + `gbuf_normal`
RGBA16F, `live.rs:226-227`), a dormant resolve stub (`resolve.frag`: N.L + ambient, tap-gated),
the fullscreen sky pass, the exposure meter (`exposure_measure.frag`) + tonemap. Extending, not
greenfield-from-zero. The typed bridge (`fsscenedump.cpp` -> `fsr_scene_*`) currently feeds only
camera + EEP sky + regime; **no geometry** (`SceneFrame::is_empty()` == true).

---

## EXACT STOCK TRACE (2026-08-03, 4-agent deep trace) — the verbatim formulas to port

The whole legacy ground path, traced to file:line. **Only `class1` terrain + `class3` softenLight/haze
exist** (no class2/class3 terrain, no class1/class2 soften — loader falls back).

### G-buffer fill — terrain (`class1/deferred/terrainF.glsl`, `pipeline.cpp:381-412,977-983`)
Stock G-buffer = 4 MRT + shared depth: RT0 `GL_RGBA8` albedo, RT1 `GL_RGBA8` ORM/spec, RT2 `GL_RGBA16`
normal+env+FLAG.w, RT3 `GL_RGB16F` emissive (opt, default OFF). Lit output `mRT->screen` = **`GL_RGBA16F`**.
Terrain writes: `frag_data[0]=max(splat,0)` with **`.a=0`** (albedo is **sRGB-ENCODED in a UNORM (non-sRGB)
buffer** — lighting does `srgb_to_linear` on read; do NOT use an _SRGB format or you double-linearize);
`frag_data[1]=vec4(0,0,0,-1)` (no spec); `frag_data[2]=encodeNormal(normalize(vary_normal),0,HAS_ATMOS)`
where stock normal is **EYE-space** (`normal_matrix*normal`). `encodeNormal(n,env,flag)`:
`f=sqrt(8*n.z+8); vec4(n.xy/f+0.5, env, flag)` (globalF.glsl:46-50). FLAG: SKIP_ATMOS 0.0 / HAS_ATMOS 0.34
/ HAS_PBR 0.67 / HAS_HDRI 1.0, matched `abs(v-flag)<0.1` (llshadermgr.cpp:636-640), stored RT2.w.

### softenLight legacy/atmos branch (`class3/deferred/softenLightF.glsl:206-284`) — the core combine
`baseColor.rgb = srgb_to_linear(gbuffer_albedo)`; `da = clamp(dot(n, light_dir),0,1)`;
`irradiance = amblit` (+ reflection-probe legacy → STUB = amblit for now); `adjustIrradiance(irr, ambocc)`
(SSAO → ambocc=1 stub); `color = irradiance`; `sun_contrib = min(da, scol) * sunlit_linear` (scol=1 until
P4); `color += sun_contrib`; `color *= baseColor.rgb`; (terrain spec.a=-1→0 so blinn-phong skipped);
`frag_color.rgb = clampHDRRange(color)` (`clamp[0,11.2]`, inf→1/nan→0); `frag_color.a = 0`. **NOTE: soften
does NOT apply fog** — haze is a separate pass. Position reconstructed EYE-space via `inv_proj` only,
`ndc.z = 2*depth-1` (GL) — **the #1 reverse-Z port risk**.

### atmospherics (`class1/windlight/atmosphericsFuncs.glsl` calcAtmosphericVars 51-131 / Linear 147-165)
Inputs = the WL params we already feed. Outputs (sRGB then linearized):
`light_atten = (blue_density + haze_density*0.25) * density_multiplier * max_y`;
`sunlight *= exp(-light_atten / lightnorm.y)`; `atten = exp(-max(blue_density+haze_density,1e-6) *
(len(rel_pos)*density_multiplier) * distance_multiplier)`; haze_glow from `dot(rel_pos_norm, lightnorm)`
& `dot(light_dir, rel_pos_norm)`, `*glow.x`, `pow(.,glow.z)`, `+.25`, `*sun_moon_glow_factor`;
`tmpAmbient = ambient + (1-ambient)*cloud_shadow*0.5`; `cs = sunlight*(1-cloud_shadow)`;
`additive = blue_horizon*blue_weight*(cs+tmpAmbient) + haze_horizon*haze_weight*(cs*haze_glow+tmpAmbient)`,
then `*= (1-combined_haze)`, `min(.,10)`. Linear post: `amblit *= ambientLighting(norm,light_dir)`
(`a=min(|dot|,1); a*=0.5; a*=a; 1-a`), then **non-classic**: `amblit = luma709(srgb_to_linear(amblit))`
(**GREYSCALE** — colour ambience comes only from the probe stub), `sunlit = srgb_to_linear(sunlit)`;
`sunlit *= sky_sunlight_scale`; `amblit *= sky_ambient_scale`. `amblit=pow(tmpAmbient,0.9)*0.57` pre-scale.

### haze pass (`class3/deferred/hazeF.glsl`, blend `pipeline.cpp:10189`)
Separate fullscreen, `glBlendFuncSeparate(ONE, SRC_ALPHA, ZERO, SRC_ALPHA)` → `screen = additive_haze +
screen*atten.r`. Shader: `discard` where `depth>=1.0` (sky); `alpha = atten.r` (SCALAR .r, grey attenuation);
`color = srgb_to_linear(additive*2.0) * sky_hdr_scale`. Runs after soften + local lights, before alpha pools.

### luminance meter (`class1/deferred/luminanceF.glsl:40-67`) + exposure (`exposureF.glsl:47-65`)
Meter: `tc = fragcoord*0.6+0.2; tc.y -= 0.1`; `c = screen(tc)`; if `!HAS_HDRI && !SKIP_ATMOS`:
`c *= diffuse_luminance_scale` (default 1.0); `c += emissive(tc)`; `L = luma709(c)`. Rendered 256x256 R16F,
auto-mip, exposure reads **mip 8** (1x1). Exposure: `L=clamp(L,0,maxL)/maxL; L=pow(L,2);
s = mix(exp_max, exp_min, L)` (maxL = coeff **0.175**); adapt `s = mix(prev, s, 1-exp(-speed*dt))`,
`speed = -log(0.1)/2.0`; `exp_min=1/hdr_scale, exp_max=hdr_scale`, `hdr_scale=sqrt(sky_gamma)*2`. Tonemap:
`exposed = color*exposure*exp_scale`; type0 PBRNeutral / type1 ACES-Hill; `mix(exposed_linear, tonemapped,
tonemap_mix)`; `linear_to_srgb`. **[FORK] `faithful_camera`** forces `exp_scale=1` at tonemap.

### splat (P3, `terrainF.glsl:44-69` + draw `lldrawpoolterrain.cpp`)
5 textures: 4 detail (`TAM_WRAP`) + `alpha_ramp`=IMG_ALPHA_GRAD_2D (`TAM_CLAMP`). Detail UV = planar texgen
`position.xy * sDetailScale + fmod(region_origin_global.xy, 1/sDetailScale)*sDetailScale`,
`sDetailScale = 1/RenderTerrainScale` (default **12**). Per-vertex `texcoord1`: `.x`=composition[0,3]
(`(height + perlinTwiddle - startHeight)*4/heightRange`, bilerp 4 corners; needs the exact `noise2`/
`turbulence2` tables seeded `srand(42)`), `.y`=noise[0,1]. Splat: `a1=ramp(tc1.x).a; a2=ramp(tc1.x-2).a;
aF=ramp(tc1.x-1).a; out=mix(mix(t3,t2,a2), mix(t1,t0,a1), aF)`. Default UUIDs dirt/grass/mountain/rock in
indra_constants.cpp:68-71.

## P3 TEXTURING PORT (2026-08-03, no-shortcuts trace E+F) — the faithful splat

**Composition/noise: PORTED TO THE ENGINE (Rust, `terrain_noise.rs`, commit f6322517c1).** Stock's
`noise.{h,cpp}` (NOT llperlin) seeds tables with `srand(42)` + MSVC `rand()` LCG
(`state=state*214013+2531011; (state>>16)&0x7fff`); reproduced deterministically (trailing
`srand(time)` runs AFTER tables built → static). `build_composition` = generateHeights;
`vertex_texcoord1` = eval's comp(datap cell) + per-vertex noise. All f32. Needs from the viewer:
the region heightmap (have), the 4 corner `mStartHeight[4]`/`mHeightRange[4]` (sim RegionInfo, NOT
the 20/60 settings defaults), and region `origin_global` (F64, global metres — lattice continuity).

**Detail pixels: SUPPLIED BY THE VIEWER (asset boundary — engine has no J2C pipeline).** `mRawImages[]`
is DEAD here. Route: on slot-change call `mDetailTextures[i]->forceToSaveRawImage(0, F32_MAX)` + once
`compp->boost()` (force decode — the null-GL draw pool may not boost); each change poll
`hasSavedRawImage()` → `getSavedRawImage()` → `getData/getWidth/getHeight/getComponents` (3 or 4
comp) → `fsr_texture_upload(reserved_id_i, w, h, rgba)`. Version by `(getDetailAssetID(i),
getSavedRawImageLevel())`. Alpha ramp = shipped local `alpha_gradient_2d.j2c` (256×256×1 GL_ALPHA8,
CLAMP) — decode it or pull via the same saved-raw route. Detail sampler = UNORM (NOT sRGB), WRAP,
mipmapped; ramp = CLAMP. Legacy `terrainF.glsl` does NO srgb_to_linear (that's downstream in soften).

**Splat shader (`terrainF.glsl:44-69`):** detail UV = `world.xy*sDetailScale + fmod(region_origin_global,
1/sDetailScale)*sDetailScale`, `sDetailScale = 1/RenderTerrainScale` (default 12). `a1=ramp(comp).a;
a2=ramp(comp-2).a; aF=ramp(comp-1).a; out = mix(mix(t3,t2,a2), mix(t1,t0,a1), aF)` → RT0 albedo(sRGB).

**P3 remaining (coupled engine+viewer, needs REBUILD):** (1) extend TerrainHeader/FsrTerrainHeader +
setSceneTerrain: start[4], range[4], origin_global[2] f64, detail_scale, 5 tex ids. (2) engine
ensure_terrain: build_composition + attach texcoord1 (stride 24→32); terrain_gb bind = UBO + 5 tex +
2 samplers; terrain_gb.{vert,frag} do the UV + splat. (3) viewer feed: extract detail+ramp pixels +
corner/origin params + boost. (4) rebuild + deploy + checkpoint (textured ground). NOTE: feed struct
byte-match couples engine+viewer — deploy both together.

## PORT ARCHITECTURE DECISION (P2)
Go **deferred** (faithful + needed for P4 shadows/objects), but reconstruct **WORLD** pos from depth via
`inv(view_proj)` — the SAME matrix family the sky ray already uses (proven) — and do lighting+atmospherics
in **world space** (world normal in G-buffer, world `sun_dir` already fed, `rel_pos = world_pos - cam_pos`).
Frame-invariant dots/lengths make this identical to stock's eye-space result while DODGING the reverse-Z
`inv_proj`/`2*depth-1` risk. Our compact 2-RT G-buffer (RT0 albedo+FLAG.a, RT1 world-normal RGBA16F) is
sufficient (no spec/emissive for terrain). Fold haze INTO the resolve (`out = additive + lit*atten.r`) since
we have no local lights to interleave — equivalent result, one pass.

**P2 phasing (each a checkpoint):** P2a terrain→G-buffer (world-normal + FLAG + depth). P2b faithful
softenLight core (amblit grey + min(da,scol)*sunlit, *albedo, clampHDRRange; discard non-ATMOS to keep sky
via LoadOp::Load) — NO haze/pos yet. P2c atmospherics + haze folded in (world-pos reconstruct, calcAtmos).
P2d exposure meter FLAG sky-exclusion + revert interim damp 227950d7db. P3 = splat (needs the noise port).

---

## Phase 1 — Terrain geometry + typed feed  [CHECKPOINT]

Get terrain SHAPE into the G-buffer, lit by the existing minimal resolve.

**Viewer (C++, needs a viewer rebuild):** new `fsr_scene_set_terrain` ABI + `fsscenedump`
plumbing. Feed per region:
- Heightmap: `257x257` f32 (`grids_per_edge+1`, shared +1 E/N edge = neighbor's first row),
  1 m spacing, from the sim DCT LayerData (`llsurface.cpp:832-911`). Source: `mSurfaceZ`.
- Region origin + width (256), meters-per-grid.
- (Textures/composition deferred to Phase 3 — Phase 1 can flat/normal-viz shade.)

**Engine (Rust):**
- Build terrain vertex buffers from the heightmap: per-vertex `pos(vec3)`, `normal(vec3)`
  (cross-product of height diagonals, neighbor-aware, `llsurfacepatch.cpp:271-422`),
  `texcoord1(vec2)` reserved for (composition, noise) in P3. Start simple: one buffer, full
  resolution (LOD/stitching = later refinement).
- Terrain pipeline (terrainV/F port) writing the G-buffer: RT0 = flat color or normal-viz,
  RT2 = encodeNormal + `HAS_ATMOS` flag. Slots into the G-buffer-fill pass (before the resolve).
- Reuse the existing minimal resolve (N.L + ambient) to light it.

**Checkpoint:** terrain relief visible + crudely lit in-world. Proves feed -> geometry ->
G-buffer -> resolve.

---

## Phase 2 — Deferred resolve + atmosphere + exposure  [CHECKPOINT — the big win]

Port stock's real resolve so terrain is properly lit + atmospheric, and the exposure self-corrects.

- **softenLight legacy/atmos path** (`softenLightF.glsl:206-276`): reconstruct view-space pos
  from depth; `decodeNormal`; `da = clamp(dot(n, light_dir))`; ambient/irradiance (probe stub =
  sky ambient for now); `sun_contrib = min(da, scol) * sunlit_linear` (scol = 1.0 stub until
  Phase 4); `color *= albedo`; blinn-phong spec optional. Branch on the FLAG.
- **calcAtmosphericVars** (`atmosphericsFuncs.glsl:51-165`): produce `sunlit/amblit/additive/
  atten` from the WL sky params (already fed) + fragment position.
- **HAZE pass** (`hazeF.glsl`): separate fullscreen blend into scene_hdr,
  `screen = additive + screen*atten.r` (blend `ONE, SRC_ALPHA`).
- **Luminance meter rewire** (`luminanceF.glsl`): sample the composited lit scene_hdr at
  `tc*0.6+0.2, y-=0.1`, exclude sky (FLAG == SKIP_ATMOS/HAS_HDRI), mip-average to 1x1. This is
  the dark-ground reference.

**Checkpoint:** terrain lit + atmospheric; daytime exposure no longer washes out; the giant sun
+ sunrise/sunset over-exposure resolve here. (Revert the interim day-exposure damp, commit
`227950d7db`, once this lands.)

---

## Phase 3 — Terrain textures (legacy splat)  [CHECKPOINT]

- **Feed** (extend `fsr_scene_set_terrain`): 4 detail texture IDs/pixels (default dirt/grass/
  mountain/rock), composition params (start-height/height-range per region corner), the alpha
  ramp texture (`IMG_ALPHA_GRAD_2D`).
- **Per-vertex composition** value in [0,3] = `(height + perlinNoise - startHeight)*4/heightRange`
  (bilerp corners), + per-vertex noise [0,1] -> `texcoord1` (`llsurfacepatch.cpp:254-265`,
  `llvlcomposition.cpp:522-557`).
- **Splat** (`terrainF.glsl:44-69`): 4 detail samples at one world-tiled UV (`sDetailScale=1/16`,
  region-origin offset); `alphaN = ramp(comp-{0,1,2}, noise)`;
  `out = mix(mix(t3,t2,a2), mix(t1,t0,a1), aFinal)`. Write RT0 albedo.

**Checkpoint:** textured ground (dirt/grass/mountain/rock). (PBR terrain = later option.)

---

## Phase 4 — Shadows (stable CSM)  [CHECKPOINT]

Port the **stable** cascade fit (the `8306f99449` sphere-bound + texel-snap backport), NOT the
dead FOV-fit code (`pipeline.cpp:11578+`).

- **Targets:** 4 directional cascade depth maps (D24/D32F, depth-compare LEQUAL sampler),
  res = `round16(screen_w * RenderShadowResolutionScale)`. (+2 spot maps = later.)
- **Split math** (`pipeline.cpp:11300-11335`): `near=clamp(-camMax.z,0.01,4)`,
  `far=min(clamp(-camMin.z*2,16,512),camFar)`; `clip[i]=near+range*((i+1)/4)^sxp` (sxp from
  RenderShadowSplitExponent, defaults -> 3); `clip[0]*=1.25`; cascades overlap 0.75/1.25.
- **Light-matrix fit** (`pipeline.cpp:11547-11568`): sphere-bound the 8 split-frustum corners ->
  (wc, sr); `look(wc - lightDir*(sr+max(sr,32)), lightDir, up)`; texel-snap the light-space center
  to `2*sr/mapWidth`; `ortho(cx±sr, cy±sr, 0.1, 2*sr+ext)`. Store
  `shadowMatrix[i] = biasScaleBias * proj * view * inv_camView`.
- **Depth pass:** cull, depth LESS+write, depth-clamp, color off; render terrain (+ later
  geometry) position-only into each cascade.
- **PCF lookup** (`shadowUtil.glsl:54-195`): normal-offset, pick cascade by view-space z vs
  `clip*{-0.75,-1.25}` (linear cross-fade), transform, `z += bias*2`, 5-tap hardware PCF ->
  `scol`; feed into Phase 2's sun term (`min(N.L, scol)*sunlit`).
- Constants: `shadow_bias=-0.002` (+altitude term), `shadow_offset=0.01`, PCF weights
  `4+1+1+1+1 -> *0.125`.

**Checkpoint:** terrain self-shadows + casts.

---

## Baked-in decisions

- **Legacy 4-texture splat before PBR terrain** (legacy is the default for most regions).
- **New typed terrain feed (bridge-the-data), NOT the retired GL tap** (`sTapDraws=false`).
- **Stable sphere-bound + texel-snap CSM**, not the old FOV-fit.
- **Critical port caveat:** stock cascade selection keys off **eye-space Z** and `shadowMatrix`
  folds in `inv_view` (consumes eye-space fragment pos). If our G-buffer stores world-space pos,
  drop `inv_view` and convert split planes accordingly.

## Key source references

Terrain: `llsurface.{h,cpp}`, `llsurfacepatch.{h,cpp}`, `llvlcomposition.{h,cpp}`,
`llvosurfacepatch.{h,cpp}`, `lldrawpoolterrain.{h,cpp}`; shaders `class1/deferred/terrain{V,F}.glsl`,
`globalF.glsl`. Shadows: `pipeline.cpp:11103-11862` (gen), `shadowUtil.glsl`, `shadow{V,F}.glsl`,
`sunLightSSAOF.glsl`. Resolve: `pipeline.cpp:9653-10215`, `class3/deferred/softenLightF.glsl`,
`hazeF.glsl`, `class1/windlight/atmosphericsFuncs.glsl`, `class1/deferred/{deferredUtil,gbufferUtil,
globalF,luminanceF,exposureF}.glsl`. Flags: `llrender/llshadermgr.cpp:636-640`.
