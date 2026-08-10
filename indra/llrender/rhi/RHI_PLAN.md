# RHI_PLAN — the gRHI seam + GL-leak removal (Vulkanstorm Phase 2)

Workstream doc for the `rhi` branch. Goal: make the viewer's render layer
**backend-agnostic** — every render primitive flows through one C-ABI vtable
(`RhiApi`), GL backend now, Vulkan backend pluggable later behind the same seam.
**Endgame: no raw GL above the seam.**

## 1. Architecture

- **`gRHI`** — `extern RhiApi* gRHI` (`rhi.h`): a pure C-ABI vtable of function
  pointers, opaque `uint32` handles (0 = invalid). `rhi.h` is the ABI; `rhi_gl.cpp`
  is the GL backend ("parity by construction" — its handles ARE GL names).
- The seam sits **beneath** the existing render abstraction (`LLRender`/`gGL`,
  `LLGLState`, `LLVertexBuffer`, `LLRenderTarget`, `LLImageGL`, `LLTexUnit`,
  `LLGLSLShader`). Those classes migrate from calling GL directly to calling
  `gRHI->…`; their current GL (~434 sites in `llrender`) **relocates into
  `rhi_gl.cpp`**, it is not deleted.
- **"Leaks"** = ~363 direct `glXxx` in `newview` (+ ~14 in `llwindow`) that bypass
  the abstraction. These route through the abstraction (preferred) or `gRHI`.
- One backend per run, selected at GL init: `llgl.cpp` →
  `gRHI = rhi_create_gl(nullptr)` after `initGLStates()`.

## 2. Pins (settled with the user)

1. **Faithful-port the seam, clean-adapt the routing.** `rhi.h`/`rhi_gl.cpp` are
   ported verbatim from the `fs-vulkan-v2` backup (self-contained, proven). The
   *routing* lived in the divergent v2 fork, so it is **re-applied cleanly** on our
   baseline, not cherry-picked.
2. **No Rust yet.** J0–J8 are the C-ABI seam + C++ GL backend only. The Vulkan
   backend (C++ ash *or* a deferred Rust/wgpu decision) fills the same vtable in a
   later phase.
3. **Verification = per-junction in-world parity.** No VK yet ⇒ no GL-vs-VK oracle;
   the GL backend is parity-by-construction, so any visible change at a junction is
   a routing bug. Tester = in-world (user), same rhythm as the keepers.

## 3. Seam op groups (`rhi.h`)

R1 frame/caps · R2 buffers+VAO+attribs · R3 textures · R4 render targets ·
R5 shaders · R6 uniforms · R7 draw · R8 dynamic state (full `LLGLState` surface) ·
R9 sync · R10 queries · R11 storage/image/barrier (PPLL).

## 4. Leak inventory (scout, 2026-08-09)

~363 live `newview` leaks (416 raw − 52 dead-code/false-positive). By category:
state **154**, texture **73**, read/clear/copy **49**, buffer/VAO **42**,
query **23**, uniform **8**, misc **7**, FBO **6**, draw **1**.

Clusters (natural migration units):

| Cluster | Files | ~Leaks | Note |
|---|---|---|---|
| A pipeline / PPLL | `pipeline.cpp` | 89 | deferred + PPLL storage (R11) |
| B terrain | `lldrawpoolterrain.cpp` | 59 | **SPECIAL: 32 `glTexGeni`/`glTexGenfv`** |
| C window + display | `llviewerwindow.cpp`, `llviewerdisplay.cpp` | 62 | clear/readback/viewport |
| D glTF | `gltfscenemanager.cpp`, `gltf/*` | 30 | preview render + loader upload |
| E probes + occlusion | `llreflectionmap*`, `llheroprobemanager.cpp` | 23 | UBO/SSBO, cubemap copy, queries |
| F debug/manip/preview | `llspatialpartition`, `llmodelpreview`, `llmaniptranslate`, … | 45 | wireframe, stencil, **legacy immediate-mode** |
| G query users | `llvieweroctree.cpp`, `llvoavatar.cpp` | 16 | occlusion/timer queries |
| H monitor-FBO + dyn-tex | `llscenemonitor`, `lldynamictexture`, … | 20 | off-screen FBO/blit/readback |
| I material uniforms | `lldrawpoolmaterials.cpp` | 8 | direct `glUniform*` bypassing `LLGLSLShader` |

`llwindow` (~14): caps query + splash clear + present + finish — context/window
layer, a separate small concern (R1/R9).

## 5. Genuine coverage gaps (more than 1:1 routing)

Most leaks map onto an existing R1–R11 op (state→R8, queries→R10, uniforms→R6,
draw→R7). The exceptions are fixed-function that no backend-agnostic op or Vulkan
can honor — but on inspection they are **dead code, not live rewrites**:

- **`glTexGeni` / `glTexGenfv`** (terrain, 32) — object-linear texcoord gen, but
  ONLY in `renderFull4TU`/`renderFull2TU`/`renderSimple`, which have **zero callers**
  (dead fixed-function paths; the terrain pool only overrides `renderDeferred`/
  `renderShadow`/`prerender`). The **live** deferred/PBR terrain already computes
  texcoords in-shader — `terrainV.glsl` `texgen_object()` + `object_plane_s/t`
  uniforms (fed from the same per-patch `tp0/tp1 = (sDetailScale,0,0,offset)`).
  → **DELETE the dead paths**, don't rewrite. Live terrain is already VK-ready.
- **`glVertexPointer` + `glColor4ubv`** (2, debug immediate-mode) — legacy
  client-array / immediate. Confirm dead at J7; delete, else VBO-ify.

Principle: for any fixed-function leak, check reachability first — if dead, prune
(aligns with the dead-code workstream); if live and derivable (like object-linear
texgen), compute in-shader; bake to a VBO only for non-derivable/procedural data.

## 6. Junction arc (checkpoint each: commit + push; in-world parity check)

- **J0 — Seam in, dormant.** Port `rhi.h` + `rhi_gl.cpp` + `gRHI` creation at GL
  init. Builds + runs identical; seam live, nothing routed. **← current**
- **J1 — PPLL first customer (R11).** Route `pipeline.cpp` OIT raw-GL → `gRHI`.
  Closes the "RHI later" IOU from the PPLL keeper. Acceptance: hair-through-glass.
- **J2 — dynamic state (R8).** De-leak the ~140 `newview` state calls into
  `gGL`/`LLGLState` first, then route `LLGLState`/`LLRender` → `gRHI`.
  Highest-frequency, lowest-risk — proves the routing pattern at scale.
- **J3 — `LLVertexBuffer` + draw (R2/R7).** Geometry backbone.
- **J4 — `LLImageGL`/`LLTexUnit` textures (R3).**
- **J5 — `LLRenderTarget` FBOs (R4).**
- **J6 — `LLGLSLShader` + uniforms (R5/R6).** De-leak `lldrawpoolmaterials`
  `glUniform*` → `LLGLSLShader::uniform*` here too.
- **J7 — Sweep remaining `newview` leaks (R1/R9/R10 + stragglers).** The two
  genuine rewrites (terrain texgen, immediate-mode) land here — or as their own
  sub-junctions, given their weight.
- **J8 — Lock the invariant.** Build check fails if `glXxx` appears above the seam.

**Ordering rationale:** state first (clean 1:1 ops, touches everything, low risk) →
resource classes that own most of `llrender`'s GL → shaders last (uniform contract
is fiddlier). The rewrites are deferred because they're rewrites, not routing.

## 7. Risks / invariants

- **reverse-Z stays OFF.** `rhi.h` carries `set_reverse_z` + `RHI_CMP_GREATER`, but
  our baseline uses standard depth (LEQUAL). Keep it — enabling reverse-Z would
  flip decals/appliers. The op exists but stays unused (as in v2).
- **Parity discipline.** The GL backend must emit a byte-identical GL call stream.
  Any per-junction visual diff = a routing bug, caught in-world before checkpoint.
- `rhi_create_vulkan` is declared but undefined (no VK backend) — harmless, unused.
  `rhi_destroy` is defined in `rhi_gl.cpp`.
