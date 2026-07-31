<!-- Canonical LL-viewer render model, built from stock source (c:/fs/ll-canonical)
     by a 3-agent sweep 2026-08-01. This is the REFERENCE our bridge must target. -->

# Canonical LL Viewer Render Model (stock, no Firestorm mods)


## Frame Flow / Deferred Pipeline

### Model

AUTHORITATIVE FRAME RENDER FLOW — LL deferred pipeline (c:/fs/ll-canonical/indra)

Top-level driver is `display()` in newview/llviewerdisplay.cpp; the heavy lifting lives in `LLPipeline` (newview/pipeline.cpp). The frame is a deferred (G-buffer) renderer: opaque geometry fills a multi-attachment G-buffer, a full-screen "soften" pass reconstructs lighting from that G-buffer + sun-shadow/SSAO lightmap + atmospherics, then forward/alpha/water is composited on top, then a post chain (tonemap/glow/DoF/AA) presents to screen.

=== ORDERED STAGE LIST (main-view path) ===

STAGE 0 — Sun shadow maps (FBO passes, off-screen depth).
  display() calls gPipeline.generateSunShadow() (llviewerdisplay.cpp:814; impl pipeline.cpp:9903). Gated on sRenderDeferred && RenderShadowDetail>0 (pipeline.cpp:9905). Renders scene depth from the sun's POV into 4 cascaded directional shadow targets mRT->shadow[0..3] (getSunShadowTarget, pipeline.cpp:9870, asserts i<4) plus up to 2 spot-light shadow maps mSpotShadow[0..1] (getSpotShadowTarget, pipeline.cpp:9876). Each cascade uses renderShadow() (pipeline.cpp:9470) with the RENDER_TYPE_SIMPLE/ALPHA/etc mask (pipeline.cpp:9929). Cascade view-proj matrices stored in mSunShadowMatrix[0..3] (pipeline.cpp:10484).

STAGE 1 — G-buffer fill / opaque geometry (FBO pass into deferredScreen).
  deferredScreen.bindTarget() + clear to magenta 1,0,1,1 (llviewerdisplay.cpp:970-980), then gPipeline.renderGeomDeferred(camera, true) (llviewerdisplay.cpp:1015; impl pipeline.cpp:3995). Optional depth pre-pass (RenderDepthPrePass) runs SIMPLE/FULLBRIGHT/SHINY through gOcclusionProgram first (llviewerdisplay.cpp:993-1012). renderGeomDeferred walks mPools in POOL enum order, and for each pool with getNumDeferredPasses()>0 calls beginDeferredPass/renderDeferred(i)/endDeferredPass (pipeline.cpp:4082-4100). Occlusion queries are issued once cur_type reaches POOL_GRASS (pipeline.cpp:4065-4072). This writes ONLY the opaque/masked draw classes into the G-buffer MRT.

STAGE 2 — deferredScreen flush; begin deferred lighting.
  rt.flush() where rt = deferredScreen (llviewerdisplay.cpp:1032-1033), then gPipeline.renderDeferredLighting() (llviewerdisplay.cpp:1037; impl pipeline.cpp:8464). All sub-stages below are inside renderDeferredLighting.

STAGE 2a — Sun-shadow + SSAO lightmap (FBO pass into deferredLight).
  If (RenderDeferredSSAO && !cube) || RenderShadowDetail>0: bind gDeferredSunProgram (shader deferred/sunLightSSAOF.glsl when AO on, else sunLightF.glsl — llviewershadermgr.cpp:1692-1696), full-screen triangle, painting a direct-light/shadow/AO lightmap into deferred_light_target = mRT->deferredLight (pipeline.cpp:8514-8541). This samples the G-buffer normal+depth and the 4 shadow cascades.

STAGE 2b — Blur the lightmap (SSAO soften, two FBO passes).
  If RenderDeferredSSAO: gDeferredBlurLightProgram horizontal blur into screen target then vertical back into deferredLight (pipeline.cpp:8543-8603).

STAGE 2c — Atmospheric soften (THE LIT LOOK) (FBO pass into screen).
  screen_target = mRT->screen bindTarget + clear alpha=0 (pipeline.cpp:8605-8608). If RenderDeferredAtmospheric: bind gDeferredSoftenProgram (class3/deferred/softenLightF.glsl) and full-screen blit (pipeline.cpp:8610-8642). This shader reads the whole G-buffer via getGBuffer() (softenLightF.glsl:125), computes atmospherics via calcAtmosphericVarsLinear (softenLightF.glsl:157), and does PBR or legacy directional lighting: pbrBaseLight for GBUFFER_FLAG_HAS_PBR (softenLightF.glsl:167-188), legacy sun_contrib path otherwise (softenLightF.glsl:231-262), applying sunlit/amblit/shadow(scol)/reflection-probe ambient+gloss. This is where albedo becomes a lit pixel.

STAGE 2d — Local lights (additive FBO passes into screen).
  If RenderLocalLightCount>0: additive blending, point lights via gDeferredMultiLightProgram batches and spot lights via gDeferredMultiSpotLightProgram, each a full-screen triangle reading the G-buffer (pipeline.cpp:8648-8897).

STAGE 3 — Forward / alpha / water / glow (into screen, post-deferred).
  Still inside renderDeferredLighting: pushRenderTypeMask limited to ALPHA/FULLBRIGHT/VOLUME/GLOW/BUMP/GLTF_PBR/AVATAR/TERRAIN/WATER/... (pipeline.cpp:8905-8935), then renderGeomPostDeferred(camera) (pipeline.cpp:8937; impl pipeline.cpp:4134). Walks mPools calling beginPostDeferredPass/renderPostDeferred(i) (pipeline.cpp:4218-4239). Inline, keyed off pool type thresholds, it runs: doWaterExclusionMask before POOL_WATEREXCLUSION (pipeline.cpp:4192), doAtmospherics (haze) before POOL_ALPHA_POST_WATER (pipeline.cpp:4198, impl 8956 — copies depth, blends gHazeProgram full-screen), doWaterHaze before POOL_ALPHA_PRE_WATER (pipeline.cpp:4204). Water itself is POOL_WATER/POOL_VOIDWATER. screen_target->flush() ends the 3D scene (pipeline.cpp:8941).

STAGE 4 — Post-processing + present (renderFinalize).
  display() -> render_ui() (llviewerdisplay.cpp:1050) -> gPipeline.renderFinalize() (llviewerdisplay.cpp:1496; impl pipeline.cpp:8017). HDR path (pipeline.cpp:8040-8058): copyScreenSpaceReflections into mSceneMap (8042), generateLuminance->mLuminanceMap (8044), generateExposure->mExposureMap (8046), tonemap into deferredLight/PostPingMap (8051), optional applyCAS sharpen+gamma (8056). Non-HDR path: gammaCorrect (8061). Then generateGlow(mPostPingMap) (8066), combineGlow ping->pong (8071), optional renderDoF (8084), FXAA (8090) or SMAA (8095-8097). Final present: gDeferredPostNoDoFNoiseProgram full-screen triangle to the default framebuffer / screen (pipeline.cpp:8136-8148). After renderFinalize, render_hud_elements + UI draw on top (llviewerdisplay.cpp:1503), then swap() (llviewerdisplay.cpp:1051).

=== DRAW-POOL ORDER (mPools sorted by POOL enum, lldrawpool.h:57-78) ===
POOL_SKY(1), POOL_WATEREXCLUSION, POOL_WL_SKY, POOL_SIMPLE, POOL_FULLBRIGHT, POOL_BUMP, POOL_MATERIALS, POOL_GLTF_PBR, POOL_TERRAIN, POOL_GRASS, POOL_GLTF_PBR_ALPHA_MASK, POOL_TREE, POOL_ALPHA_MASK, POOL_FULLBRIGHT_ALPHA_MASK, POOL_AVATAR, POOL_CONTROL_AV, POOL_GLOW, POOL_ALPHA_PRE_WATER, POOL_VOIDWATER, POOL_WATER, POOL_ALPHA_POST_WATER, POOL_ALPHA.
  G-BUFFER-FILLING pools (renderDeferred, opaque/masked): SKY/WL_SKY (sky is drawn as background), SIMPLE, FULLBRIGHT, BUMP, MATERIALS, GLTF_PBR, TERRAIN, GRASS, GLTF_PBR_ALPHA_MASK, TREE, ALPHA_MASK, FULLBRIGHT_ALPHA_MASK, AVATAR, CONTROL_AV. These have getNumDeferredPasses()>0.
  POST-DEFERRED / FORWARD pools (renderPostDeferred): GLOW, ALPHA_PRE_WATER, VOIDWATER, WATER, ALPHA_POST_WATER, ALPHA (blended alpha is forward-lit, never in G-buffer). Note comment lldrawpool.h:78: there is no literal POOL_ALPHA instance — pre/post-water pools consume the alpha faces.

=== G-BUFFER LAYOUT (addDeferredAttachments, pipeline.cpp:347-372; semantics gbufferUtil.glsl:36-58) ===
  deferredScreen base attachment (allocate GL_RGBA, with depth=true, pipeline.cpp:870): frag_data[0] = diffuseRect = albedo.rgb + alpha (gb.albedo, gbufferUtil.glsl:51).
  Attachment 1 (orm = GL_RGBA, pipeline.cpp:349,364): frag_data[1] = specularRect = legacy specular color/exponent OR PBR occlusion-roughness-metallic (gb.specular, gbufferUtil.glsl:53).
  Attachment 2 (norm = GL_RGBA16 HDR, else GL_RGB10_A2, pipeline.cpp:350,359,365): frag_data[2] = encoded normal in .xy (decodeNormal, gbufferUtil.glsl:52); .b = env intensity (gb.envIntensity, gbufferUtil.glsl:54); .w = gbufferFlag material-type selector HAS_PBR/HAS_HDRI/SKIP_ATMOS (gb.gbufferFlag, gbufferUtil.glsl:55).
  Attachment 3 (emissive = GL_RGB16F HDR/GL_RGB, OPTIONAL behind RenderEnableEmissiveBuffer, pipeline.cpp:351,366-368): frag_data[3] = emissiveRect = PBR emissive OR material env-intensity (gb.emissive, gbufferUtil.glsl:56).
  Depth: deferredScreen owns the depth buffer and shares it with mRT->screen (shareDepthBuffer, pipeline.cpp:877), so the forward/alpha passes depth-test against G-buffer opaque depth.
  Lighting scratch targets (not G-buffer): mRT->deferredLight (sun/shadow/SSAO lightmap, pipeline.cpp:881), mRT->screen (GL_RGBA16F HDR lit scene, pipeline.cpp:875).

FBO-vs-screen: Stages 0-3 all render into off-screen FBOs (shadow[], deferredScreen, deferredLight, screen, waterDis). Only the very last present in renderFinalize (pipeline.cpp:8136-8148) and the subsequent UI/HUD draw to the default framebuffer.

### Bridge implications

WHERE THE SINGLE-PASS-UNLIT BRIDGE DIVERGES FROM CANON, and the minimum path to a lit result.

The bridge taps LLVertexBuffer draws and replays them into one forward pass. In canon the SAME draws are what fill the deferred G-buffer (STAGE 1, pipeline.cpp:4082-4100) — so the bridge is effectively replaying only STAGE 1 geometry, then skipping STAGES 2a-2d and STAGE 4 entirely. Every draw the bridge captures during renderGeomDeferred is emitting to an MRT and writing raw albedo/normal/orm/emissive, NOT a lit color. In stock, those pixels are meaningless until the soften pass (STAGE 2c, gDeferredSoftenProgram) converts G-buffer albedo+normal into lit color using sun direction, atmospherics, shadows and probes. That is precisely the LIT look the unlit bridge is missing.

What the bridge already does right: replaying per-draw-class pipelines matches the canon pool split (terrain/water/sky/skinned/generic map onto POOL_TERRAIN/POOL_WATER/POOL_WL_SKY/POOL_AVATAR/POOL_SIMPLE). Porting the haze shader corresponds to STAGE 3's doAtmospherics/doWaterHaze (pipeline.cpp:8956) and STAGE 2c's atmospheric term — a reasonable first slice.

What it fundamentally CANNOT reproduce without structural change: the deferred model computes lighting in screen space from a G-buffer AFTER all opaque geometry is laid down. A forward replay of the deferred-fill draws will, per fragment, only have that fragment's material — it has no shadow map, no SSAO, no reflection-probe ambient, no screen-space soften. Rigged/skinned and generic-unlit draws that in canon go through the deferred fill (never lit in-shader) will come out as flat albedo. Terrain/simple/materials likewise. So the whole scene reads as unlit + ambient-flat.

Ordered gap list by visual importance (biggest lit-look contributor first):
  1. Atmospheric soften / directional sun (STAGE 2c, gDeferredSoftenProgram, softenLightF.glsl:157-262). This is ~all of "the world looks lit": sun+ambient+atmospherics+tonemap-in of sunlit/amblit. Missing this is the dominant visual gap. Add this FIRST.
  2. Tonemapping + gamma/exposure (STAGE 4, tonemap/gammaCorrect, pipeline.cpp:8044-8062). Without it, even a correctly-lit HDR buffer looks wrong (too dark/blown, wrong gamma). Cheap, mandatory companion to #1.
  3. Sun shadow maps (STAGE 0 + scol term in soften). Contact/cast shadows; large perceptual impact outdoors. Expensive (4 cascades) but the next tier of realism.
  4. Reflection-probe ambient + gloss (sampleReflectionProbes in soften). Gives ambient color/spec; without it, shadowed/indoor areas look dead-flat. Bridge explicitly excludes probes.
  5. Local point/spot lights (STAGE 2d, pipeline.cpp:8648-8897). Additive; matters for night/interiors.
  6. SSAO (STAGE 2a/2b). Contact darkening; subtle.
  7. Glow/bloom + DoF (STAGE 4, generateGlow/renderDoF). Stylistic polish.

MINIMUM set to get a lit result if lighting is ever added — a "mini-deferred" of exactly 3 canon stages, reusing the bridge's existing geometry tap:
  (a) Keep replaying the deferred-fill draws, but write a real G-buffer MRT: albedo (frag_data[0]), encoded normal + gbufferFlag (frag_data[2]); orm/emissive optional. Depth already available. This mirrors addDeferredAttachments (pipeline.cpp:347-372) — the bridge must at minimum reconstruct albedo+normal+depth.
  (b) One full-screen soften pass with a single directional light: port gDeferredSoftenProgram / softenLightF.glsl reading that G-buffer, applying calcAtmosphericVarsLinear + one sun_dir (drop shadows: treat scol=1; drop probes: use a constant ambient). This alone flips unlit→lit.
  (c) One tonemap/gamma present pass (gammaCorrect or tonemap, pipeline.cpp:8051/8061) into the swapchain.
  Then forward/alpha/water composite (STAGE 3) can stay roughly as the bridge already forward-renders it. Shadows (STAGE 0), probes, SSAO, local lights, glow, DoF are all additive refinements layered on afterward in the importance order above. The key structural insight: the bridge should split its single pass into G-buffer-fill + one screen-space soften + present, rather than trying to light each draw inline — that is the whole reason the deferred pipeline exists (screen-space lighting decoupled from geometry submission).


## Shader Data Contract (what mValue sees / misses)

### Model

CANONICAL SHADER DATA CONTRACT (c:/fs/ll-canonical/indra)

== THE mValue CAPTURE MECHANISM (the bridge's window) ==
LLGLSLShader caches uniform writes in `uniform_value_map_t mValue` = std::map<GLint location, LLVector4> (llglslshader.h:308). This is what a mValue-based bridge can read. Three hard limits proven from code:
1. ONLY scalar/vector setters write mValue: uniform1i/1f/2f/3f/4f, uniform1i/1iv/4iv/1fv/2fv/3fv/4fv/4uiv, both the U32-index and LLStaticHashedString overloads (llglslshader.cpp:1291-1623 and 1767-2020). Each stores an LLVector4 built from the first up-to-4 components.
2. ALL matrix setters BYPASS mValue: uniformMatrix2fv/3fv/3x4fv/4fv just call glUniformMatrix* with no mValue write (llglslshader.cpp:1625-1707). Nothing matrix-shaped is ever visible.
3. ARRAY writes (count != 1) upload the full array to GL but store ONLY element [0] in mValue: e.g. uniform4fv builds `LLVector4 vec(v[0],v[1],v[2],v[3])` and stores that single vec regardless of count (llglslshader.cpp:1586-1591). Elements [1..N-1] are invisible.

Additional non-mValue paths that exist in the draw loop:
- Raw glUniform* called directly on a cached location (getUniformLocation) - the entire Blinn-Phong material pool does this (lldrawpoolmaterials.cpp:168-200). Invisible.
- gGL matrix stacks -> LLRender::syncMatrices uploads modelview/projection/mvp/inv_*/normal_matrix/texture_matrix0-3 via uniformMatrix*fv (llrender.cpp:981-1117). All matrices, all invisible.
- Static link-time sampler->channel binding via glUniform1i in mapUniformTextureChannel (llglslshader.cpp:787). Per-draw textures are then bound to texture UNITS via gGL.getTexUnit(channel)->bindFast(tex) (llglslshader.cpp:1142), never a per-draw uniform - capture textures from texunit state, not mValue.
- LLShaderUniforms staged apply (sky/water/atmospherics): mSkyUniforms/mWaterUniforms[group].apply(shader) routes through shader->uniform1i/1f/3fv/4fv (llglslshader.cpp:2031-2053; llenvironment.cpp:1702-1714), so these ARE mValue-visible.

== VERTEX ATTRIBUTE SLOTS (fixed enum) ==
llvertexbuffer.h:132-146 / llshadermgr.cpp:1267-1280, 14 slots: 0 position, 1 normal, 2-5 texcoord0-3, 6 diffuse_color, 7 emissive, 8 tangent, 9 weight, 10 weight4, 11 clothing, 12 joint, 13 texture_index. TYPE_INDEX (14) is the separate index buffer.

== PER-DRAW-CLASS CONTRACT ==

TERRAIN (legacy, terrainV/F): attrs position, normal, diffuse_color, texcoord1. texcoord0 is SHADER-COMPUTED via texgen_object(position, texture_matrix0, object_plane_s, object_plane_t) (terrainV.glsl:44,70). object_plane_s/t via uniform4fv count=1 -> VISIBLE (lldrawpoolterrain.cpp:261-262); texture_matrix0 is a matrix -> MISS. Textures: detail_0..3, alpha_ramp (terrainF.glsl:30-34).
PBR TERRAIN (pbrterrainV/F): terrain_texture_transforms array uniform; detail_N_base_color/_normal/_metallic_roughness/_emissive channels + alpha_ramp + paint_map.

WATER (waterV / class3 waterF): attr POSITION ONLY (waterV.glsl:30); all texcoords/waves computed in-shader. Vertex uniforms waveDir1, waveDir2, time, eyeVec, waterHeight, lightDir (all scalar/vec via wrapper -> VISIBLE). Fragment: bumpMap, bumpMap2, screenTex, depthMap, exclusionTex + blend_factor, refScale, normScale, fresnelScale, fresnelOffset, specular, lightDir. waterFogColor/waterFogDensity/waterFogKS via LLShaderUniforms(SG_WATER) -> VISIBLE.

WLSKY DOME (skyV/F): attr POSITION ONLY (skyV.glsl:28). All sky/atmosphere scalars+vec3s (lightnorm, sunlight_color, moonlight_color, ambient_color, blue_horizon, blue_density, haze_horizon, haze_density, cloud_shadow, density_multiplier, distance_multiplier, max_y, glow, sun_moon_glow_factor, sun_up_factor, camPosLocal, cube_snapshot) all staged via LLShaderUniforms -> VISIBLE. env_mat is mat3 -> MISS. Textures environmentMap, rainbow_map, halo_map.
SUN DISC / MOON / STARS / CLOUDS: attrs position + texcoord0 (baked); texcoord0 transformed by texture_matrix0 in-shader (sunDiscV.glsl:53, moonV:46, starsV:52) -> MISS the transform. sunDisc: diffuseMap, altDiffuseMap, blend_factor. moon: color, moon_dir, moon_brightness, diffuseMap. stars: diffuseMap, blend_factor, custom_alpha, time. clouds: cloud_noise_texture(+_next), blend_factor, cloud_pos_density1/2, cloud_scale, cloud_variance + sky params.

MATERIALS Blinn-Phong (materialV / class3 materialF): attrs position, diffuse_color, normal, texcoord0; +tangent+texcoord1 when normal-mapped; +texcoord2 when spec-mapped (materialV.glsl:45-65). texcoords baked then x texture_matrix0 in VS (materialV.glsl:91,94,98). Textures diffuseMap, bumpMap, specularMap. CRITICAL MISS: specular_color, env_intensity, minimum_alpha, emissive_brightness are ALL set with RAW glUniform1f/glUniform4fv on cached locations (lldrawpoolmaterials.cpp:151-200), never through the wrapper -> INVISIBLE to mValue. Plus texture_matrix0 (baked scale/offset + LSL anim) -> MISS.

PBR GLTF (pbropaqueV/F, pbralpha, pbrglow): attrs position, diffuse_color (baseColorFactor is BAKED into the color stream, llfetchedgltfmaterial.cpp:129 comment), normal, tangent, texcoord0. texcoords produced by texture_transform() = KHR transform composed with SL anim matrix texture_matrix0 (pbropaqueV.glsl:85-88 via textureUtilV.glsl:62-79). KHR transforms texture_base_color/normal/metallic_roughness/emissive_transform are vec4[2] set via uniform4fv COUNT=2 (llfetchedgltfmaterial.cpp:96,135,139,143): element[0]=(scale.x,scale.y,rotation,0) captured, element[1]=(offset.x,offset.y,0,0) UPLOADED BUT NOT IN mValue (getPacked layout, llgltfmaterial.cpp) -> texture OFFSET is a MISS. texture_matrix0 anim -> MISS. Scalars metallicFactor/roughnessFactor (uniform1f) and emissiveColor (uniform3fv count=1) and minimum_alpha -> VISIBLE. Textures diffuseMap(sRGB), bumpMap(normal), specularMap(ORM packed), emissiveMap.

RIGGED objects (objectSkinV, used by every *_rigged material/pbr/diffuse/fullbright via getObjectSkinnedTransform): extra attr weight4 (packed index.fract = joint idx + weight). uniform mat3x4 matrixPalette[MAX_JOINTS] uploaded via uniformMatrix3x4fv (lldrawpool.cpp:683,719,757) -> MISS entirely.

AVATAR system mesh (avatarV / avatarSkinV): attrs position, normal, texcoord0, weight, clothing. matrixPalette is vec4[45] uploaded via uniform4fv COUNT=45 (llviewerjointmesh.cpp:184) -> only element[0] in mValue -> MISS. Wind uniforms gWindDir/gSinWaveParams/gGravity via uniform4fv count=1 (lldrawpoolavatar.cpp:810-819) -> VISIBLE.

SIMPLE / FULLBRIGHT / DIFFUSE (diffuseV/F, fullbrightV/F, simple): attrs position, diffuse_color, normal (diffuse only), texcoord0. texcoord0 x texture_matrix0 (diffuseV.glsl:66, fullbrightV:69) -> transform MISS. Texture diffuseMap. minimum_alpha via setMinimumAlpha->uniform1f (llglslshader.cpp:2024-2029; lldrawpool.cpp:532) -> VISIBLE. INDEXED variants: attr texture_index (flat int), samplers tex0..texN generated at load, selected by vary_texture_index; N textures bound to sequential units (llshadermgr.cpp:650-730).

Per-draw MODEL and TEXTURE-ANIM matrices: params.mModelMatrix loaded to gGL MM_MODELVIEW, params.mTextureMatrix (LSL llSetTextureAnim/scroll) loaded to gGL MM_TEXTURE (lldrawpool.cpp:616-622; lldrawpoolalpha.cpp:401-458), both then flushed as matrices by syncMatrices -> MISS.

### Bridge implications

WHAT THE mValue CAPTURE SEES (safe to rely on): every scalar/vec2/vec3/vec4 uniform set through the LLGLSLShader wrapper with count==1. That covers the whole sky/atmosphere/water haze parameter set (staged through LLShaderUniforms -> wrapper), terrain object_plane_s/t, PBR metallicFactor/roughnessFactor/emissiveColor, minimum_alpha on simple/fullbright/diffuse/alpha-mask/PBR pools, water wave params, avatar wind params, and cloud/star/sun/moon scalar params. The bridge's ported haze shader is well served - its inputs are all wrapper-set vec3/float.

WHAT THE mValue CAPTURE MISSES (must be sourced another way):
1. ALL MATRICES. modelview/projection/mvp/normal_matrix/texture_matrix0-3 come from LLRender::syncMatrices via uniformMatrix*fv. The bridge must read the gGL matrix stacks (gGLModelView, gGLProjection, and the MM_TEXTURE stack) directly, not mValue. This is not optional - without texture_matrix0 every baked-UV draw class (materials, simple, fullbright, diffuse, sun/moon/stars) has wrong texcoords whenever a scale/offset or LSL texture animation is active, and terrain's texgen breaks.
2. ALL SKINNING. Rigged mesh matrixPalette (uniformMatrix3x4fv) and system-avatar matrixPalette (uniform4fv count=45) are both invisible. Any skinned draw class needs the joint palette captured from LLVOAvatar::updateSkinInfoMatrixPalette / the mGLMp buffer directly (lldrawpool.cpp:674-686), plus the weight/weight4/clothing attributes. Per the bridge's "skinned" pipeline this is the single most important non-mValue payload.
3. BLINN-PHONG MATERIAL PARAMS. specular_color, env_intensity, emissive_brightness, and (in this pool) minimum_alpha are set with raw glUniform in lldrawpoolmaterials.cpp - invisible. Since the bridge is unlit it can drop specular_color/env_intensity, but emissive_brightness (fullbright flag) and minimum_alpha (alpha-mask cutoff) affect visible output and must be pulled from LLDrawInfo (params.mFullbright, params.mAlphaMaskCutoff, params.mSpecColor, params.mEnvIntensity) at the draw-tap, not from the shader.
4. PBR TEXTURE OFFSET. KHR transforms are uniform4fv count=2; element[1] carrying offset.xy is invisible. The bridge sees scale+rotation but not offset. Pull the full vec4[2] from LLGLTFMaterial::mTextureTransform (getPacked) at the material bind, or reconstruct from the draw's material.
5. FORWARD LIGHT ARRAYS (light_position[8] etc.) - only element[0] visible - but the bridge is unlit and skips these anyway; no action needed.

WHERE THE BRIDGE ALREADY DIVERGES SAFELY: it is unlit, so it correctly ignores the deferred G-buffer targets (frag_data[0..3]), env_mat, sun_dir/moon_dir lighting, reflection/irradiance/hero probes, shadow maps, SSAO - none of those need capture. Textures should be captured from texture-unit binding state (the canonical per-draw mechanism), not from uniforms, since sampler uniforms are static link-time channel assignments. Net: the bridge must add three non-mValue capture channels - (a) gGL matrix stacks incl. MM_TEXTURE, (b) the joint matrix palette, (c) selected LLDrawInfo fields (alpha cutoff, fullbright flag, PBR KHR offset) - to ship a correct per-draw-class payload; the atmospheric/haze data it already reads from mValue is complete.


## Firestorm vs Canonical Diff

### Model

CANONICAL ALPHA CONTRACT (what the bridge must match): The stock LL viewer has NO order-independent transparency. Alpha is a sorted forward pass, run post-deferred into a single RGBA16F screen target sharing G-buffer depth, split into two pools by the water plane (POOL_ALPHA_PRE_WATER / POOL_ALPHA_POST_WATER). The draw order is: (1) a rigged-only pass that writes depth, then (2) a regular forward alpha pass (depth test on, depth write off), then (3) a depth-only pass for DoF. Correctness comes entirely from three CPU sort tiers (group depth sort every frame; per-face distance sort within a group, refreshed lazily only on geometry rebuild past a 0.64 view-angle delta; per-face mDistance = center dot cam.atAxis). Blend is SRC_ALPHA/ONE_MINUS_SRC_ALPHA (alpha channel ZERO/ONE_MINUS_SRC_ALPHA); emissive/glow is a separate additive second pass. Every canonical forward-alpha fragment shader writes a single out vec4 frag_color. This is the painter-order contract the bridge already renders under.

CANONICAL TEXTURE-BUDGET CONTRACT (drives white-at-ultra / memory-churn): The discard-cycling and VRAM-budget system is entirely stock LL, unmodified by Firestorm. Under memory pressure a per-texture bias raises mDesiredDiscardLevel; LLViewerLODTexture::scaleDown() (llviewertexture.cpp:3156) is invoked when current_discard < mDesiredDiscardLevel (llviewertexture.cpp:3127), and a deferred mDownScaleQueue calls LLImageGL::scaleDown(desired) (llviewertexturelist.cpp:1200). LLImageGL::scaleDown physically shrinks the live GL texture IN PLACE via a render-to-self (FBO full-screen triangle + glCopyTexSubImage2D, or a PBO glGetTexImage path), re-specs at the smaller size, and regenerates mipmaps. This is how the stock viewer reclaims VRAM without re-fetching. A newly-loaded texture whose desired discard already dropped is downscaled immediately after creation (llviewertexturelist.cpp:1164).

CANONICAL POOL/DRAW-CLASS CONTRACT: sky (class1/deferred/skyV+skyF), water (class1/environment/waterV+waterF, class3/environment/waterF), terrain (class1/deferred/terrainV+terrainF), and skinned (class1/avatar/objectSkinV) shaders are the stock shaders and are byte-identical between the two trees. Terrain composition is 2 texture-transform vec4s (tp0/tp1) + 4 detail textures + one alpha-ramp texture; PBR terrain regions use a separate composition path.

### Bridge implications

MATCH CANONICAL, SIDESTEP WBOIT ENTIRELY. (1) WBOIT/RenderAlphaOIT is 100% Firestorm-only -- absent from every canonical .cpp, shader, and settings.xml. More decisively, Firestorm's OWN bridge code already force-disables WBOIT whenever the engine is live: lldrawpoolalpha.cpp:219 adds `&& !FSSceneDump::liveActive()` to use_oit, with a comment (lldrawpoolalpha.cpp:214-218) stating WBOIT ships an unsorted stream across the bridge so 'applier/onion layers broke beyond close-up' -- i.e. your decal breakage is already acknowledged upstream and the fix is to fall back to the canonical sorted path. So: keep your per-draw-class painter-sorted alpha, do NOT port any oit* shader, and if you ever port fullbright/alpha/pbralpha/material shaders take the canonical single-output frag_color form (the FS fullbrightF dual-output is gated by `oit_mode` and defaults to normal blend, so canonical == FS with oit off).

(2) White-at-ultra / memory-churn is a bridge-vs-canonical GL-semantics collision, not a Firestorm quirk. Canonical scaleDown reclaims VRAM by rendering a texture onto itself and re-specifying it smaller. Under the bridge's null-GL stub that render-to-self produces black/white and would wipe the engine's mirror copy, so Firestorm guards it out in engine mode (llimagegl.cpp:2518 `if (FSSceneDump::liveActive()) return false;`). Consequence you must plan for: with scaleDown neutered, the stock budget system can no longer shrink resident textures, so at ultra (low discard = large textures) VRAM only grows -> churn. The right bridge answer is to implement the downscale IN the Vulkan engine (a real vkCmdBlit / mip re-gen on the engine's texture) when the budget system requests discard N, rather than leaving textures pinned at full res. Do NOT treat the budget/discard cycling itself as a quirk -- LLViewerTexture/LLViewerLODTexture bias + mDownScaleQueue logic is stock and unmodified; honor it.

(3) The sky/water/terrain pool differences in Firestorm .cpp files are NOT extra passes or extra uniforms -- they are the bridge's own tap instrumentation (FSSceneDump::setDrawClass / setAuxF4 / setAuxTex). The underlying pool render logic and shaders are canonical. Use the terrain tap payload as the authoritative terrain draw-class contract: 2 transform vec4s + 4 detail textures + 1 alpha-ramp, plus a distinct PBR-terrain branch you must classify separately.

(4) Only ONE render-pipeline setting is genuinely Firestorm-only and relevant: RenderAlphaOIT (default true in FS, nonexistent in canon) -- ignore it, force the sorted path. RenderTransparentWater is STOCK (present in both settings.xml) so match it. FSRender* settings (FarClipStepping, Vignette, BeaconText, ParcelSelection...) are FS UI/cosmetic, not core pipeline -- safe to ignore.

(5) Firestorm-only shader complications you can bypass by porting canonical instead: starsF/V (full procedural-twinkle rewrite by paperwork 2025 -- port canonical's simpler stars if you need stars), rlvF/V (RLV vision-restriction, no canon equivalent), oitAccum/oitComposite (WBOIT), and the WBOIT dual-output variants of alphaF/pbralphaF/materialF/fullbrightF. tonemapUtilF/deferredUtil differ but live in the deferred-lighting path your unlit bridge never runs.
