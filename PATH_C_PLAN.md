# PATH_C_PLAN.md — Native-Vulkan Renderer ("Path C"), split off from OpenGL

**Status:** design, pre-implementation. Synthesises four deep code analyses (boot/context, render
dispatch + pass graph, geometry bridge, texture/material bridge) plus the existing `fs_render`
engine. File:line refs are against the current `c:/fs/firestorm-upstream` tree.

---

## 0. Goal & non-negotiables

Firestorm gets **three restart-selectable rendering backends** (Prefs → Graphics → Renderer,
`RenderGLBackend`):

- **Path A — Native OpenGL** — system `opengl32`. **Unchanged.**
- **Path B — Vulkan (Zink)** — Mesa/Zink `opengl32`+gallium (GL-over-Vulkan). **Unchanged.**
- **Path C — Vulkan (Native)** — **NEW.** A complete, parallel, **Vulkan-native** renderer
  (`fs_render.dll`) that is an *idiomatic port of the deferred OGL pipeline*, reading the
  backend-agnostic **scene** and rendering it directly on the GPU's own Vulkan driver.

**Path C is split off from GL entirely: it creates no GL context, calls no `gl*`, and — at its end
state — loads no `opengl32` (real or stub).** The null-GL stub is *retired*, its jobs *replaced* by
explicit engine-mode code.

### Invariants (hard)
1. **Real capabilities.** `LLGLManager` / feature class come from the real `VkPhysicalDevice`
   (`fsr_query_gpu_info`), never a benchmark over a fake driver.
2. **ALM always on.** Deferred is structurally unconditional in this tree (`sRenderDeferred=true`
   forced at [llappviewer.cpp:660](indra/newview/llappviewer.cpp#L660); the forward renderer is
   *gone* — no `renderGeom`). Path C only ever implements the deferred pipeline; it cannot be "off."
3. **UI never disappears.** The UI renders every frame (already covered by the `LLRender::flush`
   VK feed; re-rooted here).
4. **Honest incompleteness.** A not-yet-ported subsystem renders **nothing** (blank), never a fake.
   Paths A/B are always the full, correct fallback.

---

## 1. The split boundary — scene vs renderer

The viewer is two halves:

- **Scene (backend-agnostic):** `LLViewerObject`/`LLVOVolume`/`LLVOAvatar`, faces + CPU vertex
  data, textures *as decoded pixels* (`LLImageRaw`) keyed by **UUID**, `LLGLTFMaterial`/`LLMaterial`
  params, camera, `LLEnvironment`/sky, lights, the per-frame **visible draw list** (`LLCullResult`),
  and the sun/shadow matrices `stateSort`/`generateSunShadow` already computed.
- **Renderer (backend-specific):** `LLPipeline` passes, `LLDrawPool*`, `LLRender`, `LLVertexBuffer`→GL,
  `LLRenderTarget`→GL, `LLGLSLShader`, `LLImageGL`. **All GL.**

A/B share the renderer. **Path C replaces the renderer wholesale** and reads the scene. The seam is
the line between *"a batch/texture/matrix exists in CPU memory"* and *"push it to the GPU + draw."*
The current `FSSceneDump` bridge already proves this seam is real; Path C reads it **cleanly**
(enumerate the typed scene objects) instead of tapping live GL state.

---

## 2. The branch sites (only three)

### 2.1 Boot / window — `LLWindowWin32::switchContext` ([llwindowwin32.cpp:1208](indra/llwindow/llwindowwin32.cpp#L1208))
Engine mode keeps `recreateWindow` (HWND/DC for the swapchain, :1359/:1664), **skips the whole GL
block :1373–1768** (pixel format, `wglCreateContext`, `wglMakeCurrent`, `initWGL`, ARB selection,
`createSharedContext`, `initGL`), calls `fsrEnsureInit()` (:1770/:4006 — real HWND → `fsr_init`,
which already uses `wgpu::Backends::VULKAN`), and returns. **No GL context is ever created.**

### 2.2 Per-frame render — `display()` ([llviewerdisplay.cpp:1208](indra/newview/llviewerdisplay.cpp#L1208))
Divert **immediately before `gPipeline.mRT->deferredScreen.bindTarget()`**. At that instant the CPU
work is done and *nothing GPU-side has been submitted*: camera (:889–891, :971), environment/sky
(:977, `gSky.updateSky` :1146), geometry rebuild (:991–993), **`updateCull`→`LLCullResult`** (:1021),
**`generateSunShadow`** (cascade matrices, :1046), impostors (:1057), **`stateSort`** (draw lists,
:1122), `sUnderWaterRender` (:1194).
```
if (gUseVulkanEngine) { engine.render(scene, cullResult); swap(); }
else { /* GL path 1208..1290 unchanged, then render_ui()/renderFinalize/swap */ }
```
**Scope note:** post-process **and present** (`renderFinalize`) run *inside* `render_ui()` at
[:1747](indra/newview/llviewerdisplay.cpp#L1747) — the engine owns the whole chain through present
and composites the UI (the `LLRender::flush` VK feed).

### 2.3 Resource residency (two cuts)
- **Textures — upload cut at `LLViewerFetchedTexture::createTexture()`
  ([llviewertexture.cpp:1639](indra/newview/llviewertexture.cpp#L1639))**, the single choke point
  where `mRawImage` + `mRawDiscardLevel` are in hand *before* `createGLTexture`. Emit
  `{UUID, discard, raw pixels, components, format/swizzle, colorspace}` → engine `VkImage` keyed by
  **UUID**. ⚠ may run on the ImageGL worker thread (`scheduleCreateTexture` :1721) → the tap must be
  thread-safe. `LLImageRaw` is freed within a frame or two of upload (`destroyRawImage`, :1679/2895),
  so the engine must copy at the tap, not hold the pointer.
- **Geometry — read cut at the visible draw list.** Enumerate `LLCullResult::mRenderMap[type]`
  (built at [pipeline.cpp:3802–3841](indra/newview/pipeline.cpp#L3802-L3841)) → `LLDrawInfo`
  batches → read each `mVertexBuffer`'s **persistent CPU shadow** (`getMappedData()`/
  `getMappedIndices()`) using `getTypeMask()`+`getOffset(type)`+`sTypeSize` (SoA layout). No GL.

---

## 3. Scene → engine bridge contracts (typed C-ABI, UUID-keyed)

Extend the existing `fsr_scene_*` / `fsr_texture_*` / `fsr_ui_*` ABI. **Bridge the data, not the calls.**

| Data | Source (backend-agnostic) | Contract |
|---|---|---|
| Camera | `gGLModelView`/`gGLProjection`, near/far/fov | `fsr_scene_set_camera` (exists) |
| Sky/atmos + regime | `LLEnvironment` WindLight set | `fsr_scene_set_sky`/`set_regime` (exists) |
| Sun/shadow | `mSunShadowMatrix[0..5]`, `mSunClipPlanes`, `mTransformedSunDir` ([pipeline.cpp:11727](indra/newview/pipeline.cpp#L11727)) | **new** `fsr_scene_set_shadows` |
| Lights | `setupHWLights` local/spot list | **new** `fsr_scene_set_lights` (stub exists) |
| Draw batch | `LLDrawInfo` ([llspatialpartition.h:80-204](indra/newview/llspatialpartition.h#L80-L204)): buffer, `mStart/mEnd/mCount/mOffset`, `mModelMatrix`, texture UUIDs, material, skin, blend/alpha/flags | **new** `fsr_scene_submit_batch` (typed, replaces the glGet `fsr_submit`) |
| Vertex/index bytes | `LLVertexBuffer` CPU shadow + `typemask`+`offsets[16]`+`sTypeSize`+`mIndicesStride` | ship blob + layout (as `FsrDrawDesc` does today, minus GL state) |
| Texture | `LLViewerTexture::mID` (UUID) + `LLImageRaw` + format/swizzle/colorspace | **new** `fsr_texture_upload_uuid` (UUID-keyed; supersedes GL-name keying) |
| PBR material | `LLGLTFMaterial` ([llgltfmaterial.h:274-288](indra/llrender/llgltfmaterial.h#L274-L288)): 4 tex UUIDs, transforms, base/emissive/metallic/roughness/cutoff, alpha mode, double-sided | typed struct on the batch |
| Legacy material | `LLMaterial` ([llmaterial.h:138-156](indra/llrender/llmaterial.h#L138-L156)) + `LLDrawInfo` mirrors (spec/env/cutoff/bump/shiny/fullbright) | typed struct on the batch |
| Skin (rigged) | `LLVOAvatar` matrix palette `mGLMp` (mat3×4, [llvoavatar.cpp:11244](indra/newview/llvoavatar.cpp#L11244)) | `fsr_set_matrix_palette` (exists) |

**Identity rule:** cross the bridge on **UUID + decoded pixels + typed structs**; never `mTexName`,
never live GL state.

---

## 4. Phase 0 — de-stub the boot (the no-stub skeleton)

The load-bearing phase. Booting with no GL context is **replace, not skip**: the null-GL stub does
six load-bearing jobs today; Path C lifts each into explicit engine-mode C++ so the stub is gone
from day one.

| # | Stub job today | Path C replacement | Where |
|---|---|---|---|
| 1 | Fake caps → `gGLManager` (**194 sites/41 files** — THE blocker) | engine-mode `LLGLManager::initGL` that fills caps from `fsr_query_gpu_info` + VK limits | [llgl.cpp:1075](indra/llrender/llgl.cpp#L1075); prototype exists in `loadGPUClass` ([llfeaturemanager.cpp:443](indra/newview/llfeaturemanager.cpp#L443)) |
| 2 | No-op `gGL`/`LLRender` substrate | CPU-only `LLRender::init` (it already is the honest UI-feed source at [llrender.cpp:1829](indra/llrender/llrender.cpp#L1829)) | [llrender.cpp:866](indra/llrender/llrender.cpp#L866) |
| 3 | Fake shader compile/link | keep `LLGLSLShader` objects constructible (uniform metadata), GL compile/bind → no-op | [llviewershadermgr.cpp:530](indra/newview/llviewershadermgr.cpp#L530) |
| 4 | Fake FBO-complete | set `gPipeline.mInitialized=true`, route/skip GL RT allocation | [pipeline.cpp:458](indra/newview/pipeline.cpp#L458), :907 |
| 5 | Capture `glTexImage2D` → forward pixels | direct UUID+`LLImageRaw` upload at `createTexture` (§2.3) | [llviewertexture.cpp:1639](indra/newview/llviewertexture.cpp#L1639) |
| 6 | Satisfy wgpu's `wglGetCurrentContext` import | link wgpu without the GL backend, or a 1-function shim | build config |

Plus the `switchContext` branch (§2.1). **Deliverable:** Path C boots, **no GL context, no
`opengl32` stub**, engine presents a clear + the UI (via the flush feed). This is the step that
actually deletes the stub — everything else hangs off it.

**Sequencing within Phase 0** (each independently in-world testable, A/B untouched):
0a caps replacement (unblocks everything) → 0b `switchContext` branch + `gGL` CPU-init →
0c shader-object + pipeline-init satisfaction → 0d UUID texture upload for fonts/UI → 0e loader import.

---

## 5. Phase ladder (after the skeleton) — gated, honest-blank

Each phase: port a pass group; un-ported = blank; a **GL-vs-VK image-diff oracle** gates it; A/B
always the fallback. Reuse: **A.1–A.3 (sky, atmospherics/tonemap, UI) are real Vulkan already** and
re-root onto the skeleton (scene-fed, not tap/stub-fed).

- **P1 — Sky + UI + present.** Re-root the fullscreen procedural sky + tonemap + the `LLRender::flush`
  UI over the skeleton. Usable viewer (login, navigate, switch back), no world.
- **P2 — Opaque world + deferred resolve + sun shadows (ALM baseline).** The core. Read `mRenderMap`
  opaque pools → G-buffer MRT ([deferredScreen](indra/newview/pipeline.cpp#L978)); CSM (4 cascades,
  reuse `mSunShadowMatrix`); `gDeferredSoftenProgram` resolve (softenLight) + SSAO/lightmap
  ([renderDeferredLighting](indra/newview/pipeline.cpp#L9653)). Textures via §2.3, materials typed.
- **P3 — Alpha + water + local/spot lights + haze.** WBOIT alpha (accum RGBA32F + revealage R16F,
  [lldrawpoolalpha.cpp:208](indra/newview/lldrawpoolalpha.cpp#L208)); water; deferred local/spot/
  multi-light passes; `gHazeProgram`/`gHazeWaterProgram`.
- **P4 — Post + probes.** Luminance→exposure→tonemap(+CAS)→glow→DoF→FXAA/SMAA
  ([renderFinalize](indra/newview/pipeline.cpp#L9173)); reflection-probe cubemaps + irradiance
  ([llreflectionmapmanager.cpp:207](indra/newview/llreflectionmapmanager.cpp#L207)).
- **P5 — Avatars + terrain + particles + impostors.** Skinning (matrix palette, `WEIGHT4`/`JOINT`);
  terrain detail/composite; particles; impostor bakes.

---

## 6. Deferred pass graph (idiomatic-port reference)

Execution order the engine reproduces (targets/shaders/gates from the dispatch analysis):

1. **Pre-frame:** hero/mirror probes; reflection-probe cubemaps (post-frame, next-frame prep).
2. **Shadows** ([generateSunShadow:11103](indra/newview/pipeline.cpp#L11103)): 4 sun CSM cascades
   (`gDeferredShadow*`) → `shadow[0..3]`; 2 spot → `mSpotShadow[0..1]`. Gate `RenderShadowDetail`.
3. **G-buffer** ([renderGeomDeferred:4263](indra/newview/pipeline.cpp#L4263)): opaque pools → MRT
   `deferredScreen` (color / ORM-spec / normal `RGBA16` / emissive `RGB16F`) + depth.
4. **Deferred resolve** ([renderDeferredLighting:9653](indra/newview/pipeline.cpp#L9653)):
   sun+SSAO lightmap (`gDeferredSunProgram`) → blur → **soften/atmospherics** (`gDeferredSoftenProgram`
   → `screen` RGBA16F) → local (`gDeferredLightProgram`) / spot / multi-light.
5. **Post-deferred geom** ([renderGeomPostDeferred:4402](indra/newview/pipeline.cpp#L4402)): sky
   (`gWLSkyProgram`), water-exclusion, **alpha WBOIT**, water, glow — interleaved with `doAtmospherics`
   /`doWaterHaze` at pool thresholds.
6. **Finalize** ([renderFinalize:9173](indra/newview/pipeline.cpp#L9173)): SSR copy → luminance →
   exposure → tonemap(+CAS)/gammaCorrect → glow extract/blur/combine → DoF → FXAA/SMAA → vignette →
   present.
7. **UI overlay** (`render_ui`): HUD, 3D/2D UI (our flush feed).

Render targets, per-pass shaders, and the `RenderXxx` gate for each are enumerated in the dispatch
analysis; the engine must respond to those gates (shadows/SSAO/atmospherics/glow/DoF/SSR/AA/local
lights/probes/HDR/mirrors), all cached in `refreshCachedSettings` ([pipeline.cpp:1197](indra/newview/pipeline.cpp#L1197)).

---

## 7. Shader porting

Per-phase, port the GLSL the phase needs → SPIR-V (glslc), applying the two mechanical transforms:
**loose uniforms → std140 UBO** and **combined → separate samplers** (wgpu has no combined type).
Already ported + oracle-locked: fullscreen sky, tonemap/exposure regime, UI. Remaining by phase:
P2 `softenLight`/`sun`/`shadow`/`pbropaque`/material; P3 `oitAccum`/`oitComposite`/water/haze/light;
P4 luminance/exposure/glow/DoF/CAS/FXAA/SMAA. Color-space per PBR slot is explicit (base/emissive
sRGB-source→linear in-shader; normal/MR linear), carried as a bridge tag, **not** inferred from a GL
internal format.

---

## 8. Keep / retire

**Keep & extend:** `fs_render` engine core + `fsr_init` (real `Backends::VULKAN`); the typed
camera/sky/regime bridge; A.1–A.3 rendering; `fsr_query_gpu_info`; the `LLRender::flush` UI feed;
the three-path `RenderGLBackend` selector + Prefs dropdown.

**Retire (as phases land):** the null-GL stub (`indra/newview/nullgl/opengl32.c` — all six jobs
replaced by Phase 0); the glGet GL-draw tap (`recordDraw`/`fsr_submit` → typed `fsr_scene_submit_batch`);
GL-name-keyed texture upload → UUID-keyed.

---

## 9. Hard parts / risks (named, not buried)

1. **`gGLManager` caps (194 sites).** The load-bearing coupling; must be *populated*, not skipped.
   Phase 0a. De-risked: `loadGPUClass` already pulls real adapter info.
2. **Thread-safety of the texture tap** (`createTexture` may run on the ImageGL worker thread).
3. **Shadow-matrix reuse.** P2 reuses the viewer's `generateSunShadow` CPU math via the bridge; the
   engine renders the depth maps — verify matrix conventions (reverse-Z / clip-space) match the
   engine's existing depth_fix.
4. **WBOIT + reflection probes** are the most involved passes (P3/P4); both have working GL analogs
   to diff against.
5. **wgpu GL-backend import** (`wglGetCurrentContext`) in a no-`opengl32` world (Phase 0e).
6. **Per-phase GL-vs-VK parity oracle** must exist before each phase is called done (image diff of
   the same scene under Path A vs Path C), else "looks right" hides regressions.

---

## 10. Immediate next step

**Phase 0a — the caps replacement:** an engine-mode `LLGLManager::initGL` that fills `gGLManager`
from `fsr_query_gpu_info` + Vulkan limits, so the ~194 consumers see honest values with no GL
context and no stub benchmark. It unblocks the `switchContext` branch and every phase after. It is
independently testable (boots to the existing engine path with real caps) and keeps A/B untouched.
