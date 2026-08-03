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
