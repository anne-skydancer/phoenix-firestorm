# PBR Terrain — implementation plan (FSVulkan engine)

Synthesized from a full 2-agent trace of stock Firestorm's OpenGL PBR terrain (repo:
`c:\fs\firestorm-upstream`). Goal: render GLTF-material (PBR) terrain in the Vulkan/wgpu engine,
ported **idiomatically** (behavior/math faithful; structure clean for wgpu), phased + checkpointed.
Follows the shipped LEGACY 4-texture splat (E1-E4). User is in a PBR region → our legacy splat shows
fallback. "It's both P2 and PBR" — PBR needs the deferred pipeline fleshed out.

## G-buffer contract (PBR)  [stock: pbrterrainF.glsl:431-437]
4 render targets + shared depth (legacy used 2). Flag routes lighting: **HAS_PBR = 0.67** (legacy = 0.34).
- RT0 base color: **LINEAR** (`srgb_to_linear` in the fill shader), `.a = 0`.
- RT1 ORM: `.r`=occlusion, `.g`=roughness, `.b`=metallic (glTF), each `= tex.chan * factor` (occlusion
  factor forced 1.0). `.a` = base_color_factor_alpha (stock quirk: uses baseColor**.z/blue**, not .w).
  Reduced modes: MR-only -> `(1, rough, metal)`; base-only -> `(1,1,0)` (matte).
- RT2 normal: `encodeNormal(tnorm, env=0, HAS_PBR)`; tnorm = TBN(vNt, tangent, sign). (Our engine stores
  WORLD normal raw in RGBA16F — keep that; store world tnorm + flag; NO spheremap encode needed.)
- RT3 emissive: **LINEAR** (`srgb_to_linear` source), or 0 if emissive off.
Sampler fallbacks: base/MR/emissive -> WHITE, normal -> FLAT (0.5,0.5,1); all TAM_WRAP. sRGB: base +
emissive are sRGB source -> linear in shader; normal + MR strictly linear.

## Splat (both paint types)  [pbrterrainF.glsl + pbrterrainUtilF.glsl]
Weights `tm` (convex, sum 1), then sample 4 materials + blend (`mix_pbr` = weighted sum of col/orm/vNt/
emissive), MikkTSpace normal, alpha-mask discard.
- **HEIGHTMAP_WITH_NOISE (paint_type 0, the default):** weights from alpha ramp at composition (comp,
  comp-2, comp-1) -> nested mix of one-hot -> **subtract 0.1 threshold** -> ceil->usage -> renormalize.
  SAME alpha-ramp math as our legacy splat -> REUSE our composition/ramp, feed materials instead.
- **PBR_PAINTMAP (paint_type 1):** 3-channel paintmap at `position.xy/region_width` = weights of
  materials 2/3/4; material 1 = `1 - sum`. Paintmap is **client-baked** (LLTerrainPaintMap), NOT a
  region asset -> generate engine-side (DEFER; do heightmap type first).
- UVs = KHR texture transform per material (`[sx,sy,rot,ox,oy]`×4, `RenderTerrainPBRScale` folded into
  scale, origin = region SW corner). Scale-only (common) reduces to sign flips.
- Per-material GLTF factors modulate: baseColor(RGBA), metallic, roughness, emissive(RGB), alphaCutoff.
- Triplanar (RenderTerrainPBRPlanarSampleCount==3) is orthogonal + heavy; stock default flat (==1). SKIP.

## PBR resolve (BRDF)  [softenLightF HAS_PBR branch :167-189 + deferredUtil.glsl]
Read ORM from RT1 (r=ao,g=rough,b=metal), base from RT0 (linear), emissive from RT3, world normal RT2.
- `calcDiffuseSpecular`: f0=0.04; `diffuse = base*(1-f0)*(1-metal)`; `specular = mix(f0, base, metal)`.
- IBL (`pbrIbl`): **STUB with sky ambient** first — `irradiance = amblit` (grey), `radiance = 0`; the
  split-sum LUT + multiscatter are idiomatic-optional. Full cubemap probes = big subsystem, DEFER.
- Punctual (`pbrPunctual`) — MATCH EXACTLY (these constants make it read as SL): roughness floor 8/255;
  GGX D `roughSq/(pi*f*f)`; Schlick F with `reflectance90 = clamp(max(specColor)*25,0,1)`; Smith G
  `:465-467`; `spec = F*G*D/(4*NdotL*NdotV)`; `diff = (1-F)*base/pi`.
- Combine: `color = iblDiff + clamp(nl*(diff+spec),0,10) * sunlit * 3.0 * scol + iblSpec + emissive`.
  scol=1 (no shadow yet). **NO atmospheric fog in PBR** (additive/atten unused). clampHDRRange.

## Material feed (viewer -> engine, via the fetch-tap)  [llvlcomposition.cpp, lldrawpoolterrain.cpp]
Per slot i (0..3): `compp->mDetailRenderMaterials[i]` (LLFetchedGLTFMaterial). Textures:
`mBaseColorTexture / mNormalTexture / mMetallicRoughnessTexture / mEmissiveTexture` (each -> getID() ->
fetch-tap, add to NEEDED_IDS). Factors off the LLGLTFMaterial: `mBaseColor, mMetallicFactor,
mRoughnessFactor, mEmissiveColor, mAlphaMode/mAlphaCutoff, mTextureTransform[BASE_COLOR]`. Boost all
channels to BOOST_TERRAIN, gate on `makeMaterialsReady(strict)`. PBR vs legacy: `getMaterialType()`
(PBR when materials ready + textures fallback). ORM = the MR texture (occlusion shares it).

## PHASING (each a checkpoint)
- **PBR-1** deferred pipeline grows: 4-RT G-buffer (add gbuf_orm RGBA8 + gbuf_emissive RGBA16F); resolve
  gets a HAS_PBR branch (the BRDF above, IBL=sky-ambient). Fold in **P2d exposure** (meter lit ground,
  revert damp 227950d7db). Legacy terrain writes defaults to the new RTs (ORM matte, emissive 0).
- **PBR-2** PBR terrain_gb shader, BASE-COLOR first: sample 4 materials' base color, blend by our
  composition/ramp weights, write 4-RT G-buffer flag 0.67 + default ORM/emissive. -> PBR terrain lit.
- **PBR-3** material feed: bridge 4 base-color texture ids (fetch-tap) + factors; PBR-region detect.
- **PBR-4** deploy + checkpoint: PBR ground shows real materials, BRDF-lit.
- **PBR-5+** layer: tangents+normal maps -> metallic-roughness -> emissive -> paintmap paint-type last.

## Key source refs
`lldrawpoolterrain.cpp:219-243` (program select by paint_type), `:365-679` (renderFullShaderPBR),
`:398-470` (per-material binds), `:488-518` (KHR transforms), `:525-547` (ramp vs paintmap),
`:553-589` (factors). `pbrterrainV.glsl` (tangents, UVs), `pbrterrainF.glsl`+`pbrterrainUtilF.glsl`
(splat, weights, G-buffer). `softenLightF.glsl:167-189` + `deferredUtil.glsl:480-653` (BRDF).
`llvlcomposition.{h,cpp}` mDetailRenderMaterials/makeMaterialsReady/getMaterialType. `llgltfmaterial.h`.
