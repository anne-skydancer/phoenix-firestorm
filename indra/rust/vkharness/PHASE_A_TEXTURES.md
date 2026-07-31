<!-- Produced 2026-07-31 by a 6-reader + synthesis workflow over the actual viewer source
     (951k tokens, 364 tool calls; every fact cited to file:line). This document drives the
     Phase A (textures) implementation: fixes land in F-order, each in-world tested. -->

# Vulkan Bridge — Texture Correctness GAP MATRIX (Phase A)

All citations are from the six fact reports (viewer ground truth + bridge inventory). Symptom key: **GW** = ground white, **SU** = sky untextured, **WW** = water wrong, **TWC** = transparent-water corruption, **WM** = wrong mapping.

---

## 1) GAP MATRIX

| # | Gap | Ground-truth citation | Current bridge behavior | Symptom | Fix location |
|---|-----|----------------------|-------------------------|---------|--------------|
| G1 | Texture units are assigned per-shader at link time in driver enumeration order; no sampler is guaranteed on unit 0 (terrain detail_0..3 + alpha_ramp = 5 units; sky rainbow/halo; clouds possibly on unit 1 with units 0/1 explicitly unbound) | llglslshader.cpp:873, :1282; lldrawpoolterrain.cpp:273, :312; lldrawpoolwlsky.cpp:190, :309, :336 | Tap reads unit 0 only into `tex0` (fsscenedump.cpp:201); `tex[0..3]` captured but engine never reads them (live.rs — zero hits for `d.tex`) | GW, SU, WM | Tap C++ (resolve per-uniform, see G2) + engine Rust |
| G2 | The correct way to find "which texture is detail_0 / diffuseMap / alpha_ramp" is `shader->mTexture[reserved_uniform]` → unit → `getCurrTexture()`, not a fixed unit number | llglslshader.cpp:1185 (`uniform = mTexture[uniform]; bindFast`) ; llshadermgr.h:281 (reserved names) | Tap has no per-uniform texture resolution at all; only raw units 0–3 | GW, SU, WW | Tap C++ |
| G3 | World opaque batches bind up to 4 diffuse textures on units 0–3, selected per-vertex by the index packed in `position.w` (`sIndexedTextureChannels` hardcoded 4) | llviewershadermgr.cpp:576; lldrawpool.cpp:590; llface.cpp:2109 | Engine vertex shader discards `pos.w` (live.vert:11); one texture binding per draw; `tex[4]` unused | WM (one of four textures shown on batched faces) | Engine Rust + engine shaders (data already in `DrawDesc.tex[]`) |
| G4 | Terrain detail UVs are NOT in the VB: vertex shader does planar texgen `uv = dot(pos, object_plane_s/t)` with `sDetailScale = 1/RenderTerrainScale` (default 1/16) and per-region fmod offsets; VB TEXCOORD0 is only the 0..1 parcel-overlay UV | terrainV.glsl:70, :44; lldrawpoolterrain.cpp:267, :54; llsurfacepatch.cpp:249 | Engine samples the captured texture with raw VB TEXCOORD0 → even the *right* detail texture would be stretched region-wide | GW | Tap C++ (ship object_plane_s/t) + engine shaders |
| G5 | Terrain color = 4-way blend `mix(mix(d3,d2,a2), mix(d1,d0,a1), aFinal)`; alphas sampled from the GL_ALPHA8 alpha ramp (.a channel, CLAMP) at tc, tc-(1,0), tc-(2,0), where tc = VB TEXCOORD1 (composition, CPU noise) | terrainF.glsl:57; terrainV.glsl:74; llsurfacepatch.cpp:254; llviewertexturelist.cpp:190 | Single `tex(unit0) * vcolor * ucolor` shader; TEXCOORD1 never consumed; no clamp sampler (only one Repeat sampler, live.rs:151) | GW | Engine Rust + engine shaders |
| G6 | Legacy terrain forces output alpha to 0 (gbuffer convention); a bridge that alpha-discards renders terrain invisible. Terrain has NO color stream and no `color` uniform (must default white); main draw has blending OFF | terrainF.glsl:59; lldrawpoolterrain.h:38; lldrawpoolterrain.cpp:316 | Engine discards at `c.a < 0.004` (live.frag:10). Currently safe only because the bridge computes its own alpha; a naive terrain port of alpha=0 would vanish | GW (hazard) | Engine shaders (terrain path must output alpha 1) |
| G7 | Sky dome has NO diffuse texture: color is `vary_HazeColor`, computed in skyV.glsl entirely from ~15 EEP uniforms; the only samplers (rainbow/halo) are additive overlays | skyF.glsl:207, :275; skyV.glsl:143; lldrawpoolwlsky.cpp:190 | Bridge multiplies whatever landed on unit 0 (a rainbow/halo gradient) by white — no haze math exists anywhere in the engine | SU | Tap C++ (ship EEP uniforms) + engine shaders (haze VS) |
| G8 | Cloud UVs are shader-generated: cloudsV rescales texcoord0 by cloud_scale into 4 uv sets; cloudsF offsets by animated cloud_pos_density1/2 + noise disturbance, samples 8+ taps, uses only `.x` as density; discards when cloud_scale < 0.001 | cloudsV.glsl:381; cloudsF.glsl:543, :568 | Single sample of VB texcoord0 against possibly-unbound unit 0 (units 0/1 unbound just before, lldrawpoolwlsky.cpp:309) | SU (clouds) | Engine shaders + tap C++ (cloud uniforms) |
| G9 | Stars blend BT_ADD_WITH_ALPHA (SRC_ALPHA, ONE) with a per-vertex F32 `weight` attribute; sun/moon/clouds use BT_ALPHA | lldrawpoolwlsky.cpp:226; llrender.cpp:1430; lldrawpoolwlsky.h:40; starsV.glsl:889 | Stub: `glBlendFunc` is a pure no-op, nothing tracked (opengl32.c:484); engine has ONE fixed ALPHA_BLENDING pipeline (live.rs:376, :421); MAP_WEIGHT ignored | SU (stars wash out / wrong) | Stub C + tap C++ + engine Rust (pipeline key) |
| G10 | Texture matrix (`texture_matrix0`) is a live runtime UV transform: large (virtual size > 16) `llSetTextureAnim` faces ship RAW UVs and depend entirely on it; also parcel overlay 257/256 fudge, cubemap lookups, manip grid, replay buffers | llface.cpp:1739, :1774; llface.h:60; lldrawpool.cpp:603; lldrawpoolterrain.cpp:686; llrender.cpp:1155 | Never captured — "the texture matrix is never captured" (fsscenedump.cpp:294); LLRender has no public getter for it (llrender.h:404) | WM (scrolling/rotating/sheet anims frozen at base UV) | Tap C++ (add getter + DrawDesc field) + engine Rust + engine shaders |
| G11 | GLTF/PBR materials never bake UVs on CPU: KHR_texture_transform (+ SL anim matrix + V-flips) is applied in-shader from `vec4[2]` uniforms per material bind | llface.cpp:1545; textureUtilV.glsl:62; llfetchedgltfmaterial.cpp:96 | Raw TEXCOORD0 used; KHR transform never captured (fsscenedump.cpp:293 — "no texture matrix and no KHR transform") | WM (all PBR faces) | Tap C++ + engine Rust/shaders |
| G12 | Nothing in the viewer uses GL sRGB texture formats; internal format is chosen by component count (GL_RGBA8 etc.), sRGB decode is explicit in GLSL; normal/ORM/sculpt/mask/data textures are linear payloads | llimagegl.cpp:1638; llcubemap.cpp:82 (dead #if); pbropaqueF.glsl:73; llfetchedgltfmaterial.cpp:111 | ALL engine textures created `Rgba8UnormSrgb` (live.rs:222); stub discards internal format entirely (opengl32.c:518) | WM (color-space drift on vertex-color × texture products; wrong for data textures later) | Engine Rust (format policy) |
| G13 | Whole deferred frame renders into FBOs: sky+world → deferredScreen, water-time copy triangle → mWaterDis, exclusion pool → mWaterExclusionMask (R8), haze depth-copies, shadow passes → depth-only FBOs, dynamic-texture/bake renders, cube snapshots | llviewerdisplay.cpp:1376; lldrawpoolwater.cpp:122; pipeline.cpp:10290, :10170, 1111; lldynamictexture.cpp:163 | Stub: `glBindFramebuffer`/`glDrawBuffers`/`glFramebufferTexture*` unimplemented (resolve to ng_zero, opengl32.c:1080) — no FBO identity exists; engine renders EVERY tapped draw into the single swapchain pass (live.rs:479) | TWC (copy/exclusion/haze triangles smeared over frame), plus general pollution from shadow-pass geometry | Tap C++ (pass-flag filtering — see fix F2) |
| G14 | Transparent water samples screenTex/depthMap = mWaterDis and exclusionTex = mWaterExclusionMask — FBO attachments allocated with NULL pixels that never pass through glTexImage2D with data | waterF.glsl:86, :261; llrendertarget.cpp:238, :307; lldrawpoolwater.cpp:238 | Those ids exist engine-side only as zero-filled placeholders (stub creates zeros on NULL alloc, opengl32.c:438) or white fallback → water surface garbage | WW, TWC | Engine Rust (needs offscreen pass support for full fidelity; Phase-A approximation possible without) |
| G15 | Water wave UVs are generated in the VS from world position + `waveDir1/2 * time` uniforms; VB TEXCOORD0/NORMAL are ignored; screen UVs are projective from clip position | waterV.glsl:101, :87; lldrawpoolwater.cpp:260 | Generic shader samples bump map (if it even lands on unit 0) with VB UVs | WW | Tap C++ (water uniforms) + engine shaders |
| G16 | Texture re-uploads happen constantly: discard-level changes re-spec via glTexImage2D at new size; main-thread uploads arrive as glTexImage2D(NULL) + row-batched glTexSubImage2D | llimagegl.cpp:1503, :1067, :1086 | Engine bind groups keyed `(slot, tex0)` are NEVER evicted; `upload_texture` replaces the view but existing bind groups pin the OLD view or white fallback forever (live.rs:361, :259-262) | GW, SU, WM — textures stuck white/stale everywhere | Engine Rust |
| G17 | Default-path texture downscale (RenderDownScaleMethod=0) is glTexImage2D(NULL, smaller size) + glCopyTexSubImage2D from an FBO | llimagegl.cpp:2545 | Stub: NULL re-spec at a DIFFERENT size uploads zeros (opengl32.c:438 create-once only guards same-size), and glCopyTexSubImage2D is an explicit no-op (opengl32.c:488) → downscaled textures turn black | GW, WM (textures degrade to black over time) | Viewer C++ (disable scaleDown in engine mode) or stub C |
| G18 | Cube-map faces upload through ordinary glTexImage2D/glTexSubImage2D with GL_TEXTURE_CUBE_MAP_* targets; float uploads exist (mNoiseMap GL_FLOAT, gEXRImage, mLightFunc) | llcubemap.cpp:87, :162; pipeline.cpp:1503, :1657 | Stub drops any target ≠ GL_TEXTURE_2D and any type ≠ GL_UNSIGNED_BYTE (opengl32.c:433, :310) | (latent — not Phase-A visible; unlit engine doesn't sample these) | Stub C (later phase) |
| G19 | GL_ALPHA under core profile becomes GL_RED + swizzle {0,0,0,R}; GL_LUMINANCE becomes GL_RED + {R,R,R,1} — distinguished only by glTexParameteriv swizzle | llimagegl.cpp:1360 | Stub maps both ALPHA and RED to white-RGB + value-in-alpha (opengl32.c:352), conflating the two. Happens to be CORRECT for the terrain alpha ramp (shader reads only .a) and fonts — but is wrong for luminance-origin reds | GW (benign for ramp — must be preserved), WM (edge cases) | Stub C (only if refined; keep .a semantics) |
| G20 | Sky pool clears depth on exit (endDeferredPass) so haze can mask unwritten depth; sun/moon force z ≈ far in their GLSL VS; dome draws depth-write OFF | lldrawpoolwlsky.cpp:92; sunDiscV.glsl:669; moonV.glsl:778; llvowlsky.cpp:326 | Stub glClear is a no-op; engine clears depth once per frame; the z-forcing lives in GL shaders the bridge never runs (only the projection squash survives via MVP, llgl.cpp:2925) | SU (minor ordering artifacts) | Engine Rust (optional; mid-frame depth clear needs pass split — flag) |
| G21 | `drawRangeFast` skips `syncMatrices` — GL-side texture matrix may be stale at that tap, but gGL's CPU-side stacks are always current | llvertexbuffer.cpp:970-975; llrender.cpp:1023 | Tap fires from drawRangeFast too (llvertexbuffer.cpp:972) and reads gGL directly — capture from gGL is safe; just never read it back from the stub | (correctness note for G10 fix) | Tap C++ (read gGL, not GL state) |
| G22 | Terrain/sky/water VBs have distinctive typemasks: terrain V|N|T|TC0|TC1 no COLOR; sky dome V|TC0; stars V|COLOR|TC0|WEIGHT; water V|N|TC0 | lldrawpoolterrain.h:38; lldrawpoolwlsky.h:37, :40; lldrawpoolwater.h:54 | Engine handles missing TEXCOORD0/COLOR with white/zero fallbacks (live.rs:505) — correct; but has no way to *identify* these draws for special-casing | GW, SU, WW | Tap C++ (ship a draw-class/program tag — typemask alone is ambiguous) |

---

## 2) ORDERED FIX LIST (Phase A)

Each step is independently testable in-world. **[FBO/pass]** marks steps needing pass awareness. Prerequisites noted explicitly.

### F1 — Engine: bind-group eviction on texture (re)upload  *(engine Rust)*  — closes G16
In `upload_texture`, before `textures.insert(id, ...)`, remove every entry in `binds` whose key's tex-id component == `id` (after F4, all four ids). Alternatively add a generation counter to the bind key.
**Why first:** every subsequent texture fix is invisible while stale bind groups pin old views/white. **Prerequisite for F4, F7, F8, F9, F10.**
**Test:** walk around; streamed textures sharpen through discard levels instead of sticking white/low-res.

### F2 — Tap: pass filtering by viewer flags  *(tap C++)*  **[FBO/pass-lite]** — closes G13 (the corruption half)
In `FSSceneDump::recordDraw`, skip recording when:
- `LLPipeline::sShadowRender` is true (shadow-pass geometry — terrain shadow pass at lldrawpoolterrain.cpp:174 included),
- `gCubeSnapshot` is true (probe faces),
- an explicit suppress flag is set; set/clear that flag around: the mWaterDis copy triangle in `LLDrawPoolWater::beginPostDeferredPass` (lldrawpoolwater.cpp:122), `doWaterExclusionMask` (pipeline.cpp:10290), the depth-copy + haze triangles in `doAtmospherics`/`doWaterHaze` (pipeline.cpp:10170), and `LLViewerDynamicTexture` render/postRender (lldynamictexture.cpp:163).
This is deliberate *viewer-side* pass awareness — no stub FBO tracking required, consistent with the fact that all classification info already exists as pipeline flags.
**Test:** with RenderTransparentWater ON, the fullscreen smear/corruption disappears; scene generally cleans up (no shadow-cascade ghost geometry).

### F3 — Viewer: neutralize scaleDown in engine mode  *(viewer C++)* — closes G17
When `FSSceneDump::liveActive()`, make `LLImageGL::scaleDown` a no-op (or skip the FBO copy branch). The stub cannot service glCopyTexSubImage2D; without this, textures degrade to black as discard levels rise.
**Test:** long session / teleport churn; textures no longer go black over time.

### F4 — Engine: multi-unit binding + per-vertex texture index  *(engine Rust + engine shaders)* — closes G3, half of G1
- Bind-group layout: 4 texture views + sampler; key = `(slot, tex[0], tex[1], tex[2], tex[3])`.
- VS: recover index via `floatBitsToInt(pos.w)` (llface.cpp:2109 packs an S32 through the float bits), pass as flat varying; FS selects among the 4 samplers. Index 0 when TEXCOORD-fallback path is active.
- Data is already shipped (`d.tex[4]`, fsscenedump.cpp:206). **Prereq: F1.**
**Test:** opaque world batches with mixed textures (typical furnished parcel) show correct per-face textures instead of one texture repeated.

### F5 — Tap+engine: texture matrix capture  *(tap C++ + engine Rust + shaders)* — closes G10, part of WM
- Add a public getter for `mMatrix[MM_TEXTURE0]` to LLRender (none exists — llrender.h:404).
- Extend `FsrDrawDesc` with `F32 texmat0[16]` (version-bump the struct in both C++ and Rust).
- Read from gGL's CPU stack at the tap (safe even at drawRangeFast — G21).
- Engine VS: `uv = (texmat0 * vec4(uv, 0, 1)).xy`. Identity in the common case (guaranteed initialized — llglslshader.cpp:418 hash trick).
**Test:** an in-world object with `llSetTextureAnim` scroll/rotate/frame-sheet on a large face animates correctly instead of showing the frozen base UV.

### F6 — Stub+tap+engine: blend-func capture  *(stub C + tap C++ + engine Rust)* — closes G9
- Stub: track `glBlendFunc`/`glBlendFuncSeparate` src/dst factors (add real entries; currently no-op at opengl32.c:484 / unresolved via ng_zero).
- Tap: read them back (like DEPTH_WRITEMASK) into two new DrawDesc fields.
- Engine: extend pipeline key; support at minimum (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) and (SRC_ALPHA, ONE) per llrender.cpp:1430.
**Test:** stars at night render additively (bright points) instead of alpha-blended mush; glow-adjacent effects improve.

### F7 — Tap: per-uniform texture + uniform resolution infrastructure  *(tap C++)* — closes G2, enables F8–F10
Generalize the DIFFUSE_COLOR pattern (fsscenedump.cpp:219): for the bound shader, resolve reserved-uniform → channel via `sh->mTexture[uniform]` → `gGL.getTexUnit(ch)->getCurrTexture()`, and read cached uniform values from `sh->mValue`. Ship a small named-slot table (or a draw-class tag + fixed-layout extra payload) in the DrawDesc extension. Also ship a **draw-class id** derived from the bound program (`sCurBoundShaderPtr` identity: terrain / sky / clouds / sun / moon / stars / water / generic) so the engine can select specialized pipelines — typemask alone is ambiguous (G22).
**Prereq for F8, F9, F10.**
**Test:** debug-dump shows terrain draws carrying 5 resolved texture ids and object_plane values.

### F8 — Terrain pipeline  *(engine Rust + engine shaders; data from F7)* — closes G4, G5, G6 → **ground fixed**
- Inputs shipped per terrain draw: detail_0..3 ids, alpha_ramp id, `object_plane_s`, `object_plane_t` (lldrawpoolterrain.cpp:267).
- Engine VS: `uv_detail = vec2(dot(pos4, tp0), dot(pos4, tp1))` (texture_matrix0 is identity in the main pass — llrender.cpp:1032 fact); alpha-ramp coords from TEXCOORD1: `tc`, `tc-(1,0)`, `tc-(2,0)` (terrainV.glsl:74).
- Engine FS: 4-way mix per terrainF.glsl:57 using ramp `.a` with a CLAMP sampler (detail textures WRAP); **output alpha = 1.0**, never the GL shader's forced 0 (G6); blending off.
- Vertex color defaults white (no COLOR stream — already the engine fallback); `color` uniform defaults white (already the default).
- MVP already contains the region translation (lldrawpoolterrain.cpp:211) — do not add offsets.
**Prereqs: F1, F4 (multi-bind infra), F7.**
**Test:** stand on mainland/Linden Home: ground shows correctly blended, correctly tiled (16 m repeat) detail textures.

### F9 — Sky handling  *(engine shaders + tap C++ uniforms via F7)* — closes G7, G8 → **sky fixed**
- **Dome:** port skyV.glsl's haze math into an engine VS; ship the EEP uniform set (blue_horizon, blue_density, haze_horizon, haze_density, glow, sunlight/moonlight/ambient color, lightnorm, max_y, density_multiplier, cloud_shadow, camPosLocal) read from the bound program's cached values / applySpecial path (llsettingsvo.cpp:777). Rainbow/halo additive overlays are optional polish — skipping them is visually minor.
- **Sun/moon:** work through the *generic* path once F6 (BT_ALPHA already default) + F7 resolve `diffuseMap`/`altDiffuseMap` to real units; the moon's `color` uniform is already captured (moonF.glsl:813 + DIFFUSE_COLOR capture). Note their GL z-forcing (sunDiscV:669) doesn't replay; the squashed projection in the MVP (llgl.cpp:2925) keeps them at ~far — acceptable Phase A.
- **Stars:** need F6 (ADD_WITH_ALPHA). MAP_WEIGHT ignored → uniform-intensity stars; acceptable Phase A. Note stars are non-indexed drawArrays of 3.72M verts (llvowlsky.cpp:300) — already supported (drawArrays is tapped).
- **Clouds:** Phase-A minimum = single-tap density approximation: apply cloudsV's cloud_scale UV rescale + cloud_pos_density1 scroll, sample noise `.x` once, alpha-blend. Full 8-tap fidelity deferred.
- Do NOT try to replicate the endDeferredPass depth clear (G20) — flag as **[FBO/pass]**, benign to skip since the sky sits at ~far depth.
**Prereqs: F1, F6, F7.**
**Test:** sky shows a graded haze dome, textured sun disc and moon, additive stars at night.

### F10 — Water handling  *(engine Rust + shaders; full fidelity is [FBO/pass])* — closes G14, G15 → **water acceptable**
Phase-A two-tier:
- **Tier 1 (no FBO support needed):** detect water draws by program tag (F7). Render the plane as a specialized pipeline: fog-tinted, fresnel-ish color from shipped `waterFogColorLinear` + wave normal from bumpMap sampled with shader-generated UVs (`pos.xy * const + waveDir * time`; ship WATER_TIME/WAVE_DIR1/2 — lldrawpoolwater.cpp:260). Ignore screenTex/depthMap/exclusionTex (treat exclusion mask as all-pass, matching its clear-to-white). Combined with F2 (corruption draws filtered), this yields plausible, stable water.
- **Tier 2 (flagged, needs offscreen passes):** true refraction requires the engine to produce its own "frame-so-far" color+depth copy at water time and route the mWaterDis/mWaterExclusionMask producers offscreen — full FBO/pass machinery. Explicitly out of Phase A.
**Prereqs: F2 (mandatory — otherwise corruption masks everything), F7.**
**Test:** RenderTransparentWater ON: water plane looks like water (animated, tinted), no fullscreen corruption; underwater view tinted, not garbage.

### F11 — (Optional Phase-A tail) PBR base-color KHR transform + sRGB policy  *(tap C++ + engine Rust)* — narrows G11, G12
Ship `texture_base_color_transform` (vec4[2], llfetchedgltfmaterial.cpp:96) via F7's uniform capture and apply the textureUtilV.glsl:62 sequence (anim matrix → V-flip → KHR → V-flip) in the engine VS for PBR-tagged draws. Decide sRGB policy: keep `Rgba8UnormSrgb` for color-tagged draws only, or switch to Unorm + shader-side decode to match GL semantics exactly. Lower priority: unlit output makes the error subtle.

**Dependency summary:** F1 → {F4, F8, F9, F10}; F2 → F10; F4 → F8; F6 → F9(stars); F7 → {F8, F9, F10, F11}. F3, F5 are independent.

---

## 3) DO-NOT-BREAK LIST

Behaviors of the current bridge that the fixes must preserve (citations from the bridge inventory):

1. **`tex0 == 0` → white fallback.** `unbind` sets `mCurrTexture = 0` but GL actually binds sWhiteTexture (llrender.cpp:435); the engine's 1×1 white fallback (live.rs:363) reproduces this exactly. F4's 4-slot binding must keep white for any zero/unknown id.
2. **Viewer-authoritative `offsets[16]`** with the `calc_offsets` replica as fallback only when all of the first 14 are zero (fsscenedump.cpp:241; live.rs:378). Do not recompute when shipped.
3. **`depth_fix()` GL→wgpu depth remap** left-multiplied onto the shipped MVP (live.rs:355). Every new pipeline (terrain/sky/water) must apply it identically.
4. **MVP = gGL projection × modelview, captured raw** (fsscenedump.cpp:294). It already contains the terrain region translation, the sky projection squash and dome modelview, and UI ortho — never "correct" it per draw-class.
5. **Strip→list conversion, u16 indices, and the synthetic strip cache key** mixing pointer/offset/count (live.rs:297, :306, :517). The sky dome is TRIANGLE_STRIP drawRange per segment (llvowlsky.cpp:336) and depends on this.
6. **Per-thread `g_bound2d`** in the stub (opengl32.c:483/244) — keeps the media-update thread from stomping the main thread's texture uploads.
7. **Create-once NULL re-spec semantics**: glTexImage2D(NULL) with a known same-size id must NOT wipe content (opengl32.c:438) — render-target re-specs and the staggered ALLOC+sub-rows upload path (llimagegl.cpp:1503) rely on it. F3/G17 changes only the *different-size* case.
8. **Monotonic texture ids from 1000, glDeleteTextures never reuses names** (opengl32.c:209-210, :488) — leak-only, no aliasing. If eviction is added later, it must not reintroduce id reuse races.
9. **Geo-cache invalidation protocol**: `flush_vbo` mirrors into the CPU shadow and calls `FSSceneDump::bufferDirty` → `invalidate(ptr)` (llvertexbuffer.cpp:1491; live.rs:265). Same-pointer-same-length = cache hit must stay.
10. **Per-draw DIFFUSE_COLOR capture with (1,1,1,1) default** (fsscenedump.cpp:219). Terrain and the sky programs have no `color` uniform — the white default is load-bearing (lldrawpoolterrain.cpp:316; only moonF declares `color`).
11. **Skip-and-count draws with no CPU shadow** (fsscenedump.cpp:172) — never submit unshadowed buffers.
12. **Frame protocol**: `fsr_begin_frame` from `FSSceneDump::onFrame`, present via `LLWindowWin32::swapBuffers` → `fsr_end_frame`, submits rejected when no frame is open (lib.rs:226; llwindowwin32.cpp:4064). `textureUploaded()`/`endFrame()` on FSSceneDump remain intentionally uncalled.
13. **Sub-upload path honoring GL_UNPACK_ROW_LENGTH** and `fsr_texture_subupload` bounds checks (opengl32.c:474; live.rs:236) — media textures and staggered row uploads depend on both.
14. **RED/ALPHA → white-RGB + value-in-alpha conversion preserving `.a`** (opengl32.c:352). The terrain alpha ramp (GL_ALPHA8, shader reads only `.a`) and font atlases work *because* of this; any format refinement (G19) must keep the alpha channel semantics byte-identical.
15. **Missing-attribute fallbacks**: uv=(0,0) and opaque-white color vertex buffers for typemasks lacking TEXCOORD0/COLOR, sized FALLBACK_VERTS=65536 (live.rs:505, :175) — terrain (no COLOR) and sky dome (no COLOR) depend on the white-color fallback.
16. **FS_ENGINE_MODE=1 gating and the fsr_frame clear-only fallback** when fsr_end_frame is absent (fsscenedump.cpp:436; lib.rs:49) — keep the degraded path working.
17. **Stub's honest GLSL parsing / stable fake uniform locations (ng_loc)** — the tap's `mValue`-based uniform capture (and F7's extension of it) depends on the fake link keeping consistent locations per name.
18. **Single Repeat/Linear sampler as the default** (live.rs:151) — terrain detail and cloud noise require WRAP (lldrawpoolterrain.cpp:275; llvosky.cpp:947). New CLAMP samplers (alpha ramp, sun/moon) are *additions*, not replacements.