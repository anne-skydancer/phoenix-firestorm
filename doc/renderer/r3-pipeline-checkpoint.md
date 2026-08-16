# R3 shader, binding, and pipeline checkpoint

Date: 2026-08-16  
Branch: `render/ghi-r3-pipelines`

Compatibility note: R4a supersedes the R3 shader package's schema v2 with
schema v3. Schema v3 separates shader value types from vertex storage formats
and reflects fragment outputs. The R3 evidence below records the state at the
R3 close; current R4 builds deliberately accept schema v3 only.

R3 establishes the first complete backend-neutral graphics draw without
routing production viewer world or UI rendering away from OpenGL. The same
offscreen indexed diagnostic workload must execute through the independent
OpenGL and Vulkan GHI peers.

## Accepted shader policy

- Vulkan-specific GLSL is the production source language for the Vulkan peer.
- A pinned offline glslang toolchain produces packaged SPIR-V. Normal viewer
  execution does not compile or translate Vulkan GLSL.
- Packaged OpenGL 4.6 GLSL is the primary Windows/Linux OpenGL artifact.
- Packaged OpenGL 4.1 GLSL is the macOS artifact and an explicit compatibility
  fallback for supported older Windows/Linux devices; it is not the universal
  OpenGL feature ceiling.
- The runtime shader-package contract is source-language independent: stage
  artifacts keyed by target profile, entry points, reflection manifest, and
  stable semantic identity.
- Slang is the contingency frontend only if adapting Vulkan GLSL proves
  objectively infeasible. The GHI, binding manifest, pipeline descriptors, and
  cache format must not depend on which frontend produced the SPIR-V.
- R3 does not maintain Vulkan GLSL and Slang implementations in parallel.

Evidence that may reopen the frontend decision is limited to reproducible
compiler defects, an unrepresentable required Vulkan feature, unmanageable
permutation/module structure, reflection instability, or measured material
correctness/performance improvement from Slang.

## R3a contract decisions

- Rendering attachments name image views, not ambient whole-image objects.
- A shader package carries separate OpenGL GLSL and Vulkan SPIR-V artifacts for
  each stage plus one backend-neutral reflected interface.
- Reflected bindings use stable groups for frame/view, pass, material,
  object/draw, and algorithm storage data.
- Immutable binding sets reference shader-package groups and typed buffer,
  image-view, and sampler resources. Dynamic offsets are explicit at bind time.
- Pipeline descriptions include vertex-buffer layouts, vertex attributes,
  specialization constants, attachment formats, samples, and immutable
  raster/depth/blend state.
- Viewport and scissor are explicit dynamic commands. No backend infers them
  from unrelated global state.
- Native handles, uniform locations, texture units, descriptor sets, and raw
  OpenGL/Vulkan enums remain below the backend boundary.
- New semantic commands append opcodes; existing opcode values and the R0 trace
  contract are not renumbered.

The R3a headers define this vocabulary. Binding-set execution intentionally
remains `Unsupported` in all peers until R3c adds complete validation and the
shared fixture. This prevents either native implementation from becoming the
de facto contract.

## R3b offline shader package

- `scripts/renderer/pack_ghi_shader.py` is the only compiler path. The viewer
  does not load glslang, SPIRV-Tools, or SPIRV-Reflect at runtime.
- The packer requires the manifest-pinned glslang and SPIRV-Tools versions,
  compiles Vulkan GLSL for Vulkan 1.3, validates before and after the pinned
  `spirv-opt -O` recipe, and reflects the optimized module.
- Reflected stage interfaces, descriptor groups/bindings/types/names, vertex
  inputs, and entry points are checked against the reviewed source manifest.
- Every stage packages OpenGL 4.1 GLSL, OpenGL 4.6 GLSL, and Vulkan 1.3 SPIR-V
  as distinct target-profile artifacts. Runtime profile choice does not alter
  the package's semantic identity.
- Canonical JSON serialization makes `.llghisp` output byte deterministic.
  Each artifact has a SHA-256 hash; semantic and toolchain SHA-256 identities
  are separate so either source changes or compiler upgrades invalidate the
  appropriate cache boundary.
- The build target `llrender_ghi_shaders` generates the package in the build
  tree. Its focused test builds twice for byte equality and proves a deliberate
  reflection/manifest mismatch is rejected.
- The runtime decoder accepts only schema v2, verifies every artifact SHA-256,
  checks SPIR-V size and magic, rejects duplicate stages/targets/bindings/input
  locations, and returns only backend-neutral `ShaderPackageDesc` data. It has
  no compiler or reflection dependency.

## R3c executable draw contract

- The validation peer now owns the complete shader-package, binding-set,
  graphics-pipeline, and indexed-draw validity rules. Native peers must match
  these results; they may not weaken the contract independently.
- Binding sets are checked against reflected group, binding, type, array, and
  stage data. Buffer usage/range, image usage, sampler presence, dynamic-offset
  count/alignment/range, stale handles, complete group population, and resource
  lifetimes are explicit.
- Pipeline creation checks graphics stages, reflected vertex inputs, buffer
  layouts and strides, attachment classes, blend count/masks, sample count,
  specialization constants, depth/stencil declarations, and depth-clamp
  capability.
- Draws require a compatible pipeline, all reflected binding groups, every
  declared vertex-buffer slot, an index buffer for indexed draws, and explicit
  positive viewport/scissor rectangles contained by the rendering extent.
  Attachments must cover that extent.
- Devices publish uniform/storage offset alignment and one capability-selected
  exact depth/stencil format. The policy prefers D24S8 when supported and uses
  D32FS8 otherwise. Resource creation never silently substitutes either format,
  and no vendor-specific renderer path is introduced.
- `llghidrawfixture.h` defines the single workload R3d and R3e must execute: a
  64x64 offscreen reverse-Z textured indexed quad with explicit vertex/index/
  uniform/texture uploads, reflected groups 0 and 2, color plus depth/stencil
  attachments, viewport, scissor, deterministic image readback, and teardown.
- The validation fixture semantic SHA-256 is
  `a12dd9256c090e3f8575508fd39f73d35f151b40ab970eef2c8e72d8ad0e906b`.

## R3d OpenGL execution peer

- The OpenGL peer selects packaged GLSL 4.6 on capable Windows/Linux contexts
  and keeps packaged GLSL 4.1 as the compatibility/macOS artifact. Shader
  compilation and linking occur inside the OpenGL backend; source symbols are
  mapped to reflected GHI groups and bindings without exposing uniform-block
  indices or texture units above the backend boundary.
- Shader packages, immutable binding sets, pipelines, framebuffer attachments,
  reverse-Z depth state, vertex/index buffers, viewport/scissor state, and
  indexed draws now execute through `CommandContext` on OpenGL.
- OpenGL native programs and vertex arrays obey the R2 deferred-retirement
  window. GHI handles are invalidated immediately while native deletion waits
  for the configured in-flight frame interval or `waitIdle()`.
- `llrender_opengl_draw_harness` creates an undisplayed WGL context, loads the
  deterministic shader package, and executes the same `runDrawFixture()` used
  by the validation peer. It does not initialize the viewer, log in, present a
  swapchain image, or alter production world/UI rendering.
- R3d acceptance on the AMD Radeon RX 9070 XT native OpenGL 4.6 driver
  26.7.1: shader-package load, GLSL 4.6 primary and GLSL 4.1 fallback
  compile/link, reflected binding, offscreen reverse-Z indexed draw, teardown,
  and `waitIdle()` all pass without a GL error. Fixed-pixel comparison remains
  deliberately assigned to R3f.

## R3e Vulkan execution peer

- Device creation explicitly requires and enables Vulkan 1.3 dynamic
  rendering. R3e does not create compatibility render passes or framebuffers
  and does not require a presentation surface or swapchain.
- Packaged Vulkan 1.3 SPIR-V creates native shader modules. Reflected sparse
  groups create descriptor-set layouts (including empty intervening layouts),
  one pipeline layout, per-binding-set descriptor pools/sets, and typed buffer,
  image, and sampler writes without leaking Vulkan objects through the GHI.
- Graphics-pipeline creation maps vertex input, topology, multisampling,
  reverse-Z depth/stencil, culling, blending, attachment formats, dynamic
  viewport/scissor, and specialization data. The centralized Vulkan shader
  clip conversion owns the Y conversion. Native front-face state therefore
  uses the canonical GHI winding directly and is not inverted a second time.
- Transfer writes receive an explicit transfer-to-graphics memory barrier.
  Images transition between transfer, shader-read/general, color-attachment,
  depth/stencil-attachment, and readback layouts with matching stage/access
  masks before use.
- Shader modules, descriptor layouts/pipeline layouts, descriptor pools, and
  graphics pipelines obey immediate GHI handle invalidation plus the R2
  deferred native-retirement window.
- `llrender_vulkan_draw_harness --validation` loads the same deterministic
  package and executes the same `runDrawFixture()` as the validation and
  OpenGL peers. The Khronos validation callback makes severity-error messages
  fail the fixture and explicitly enables synchronization validation.
- R3e acceptance on the AMD Radeon RX 9070 XT Vulkan implementation: the
  offscreen fixture passes with no API, synchronization, descriptor, image
  layout, dynamic-rendering, or lifetime validation errors. Capability probing
  selects D32FS8 because D24S8 is not available for the required attachment
  usage; no AMD-specific path is introduced. Loader-only warnings identify
  duplicate AMD switchable-graphics and OBS layer manifests.

## R3f deterministic parity and cache evidence

- The shared fixture copies its complete color target to a GHI readback buffer
  before submission completes. OpenGL's bottom-left `glGetTexImage` rows are
  normalized to the diagnostic contract's top-left row order before hashing;
  this does not change the command stream or production image resources.
- Back-face culling is enabled for the canonical counter-clockwise quad. This
  initially exposed a real Vulkan double-inversion defect: the whole quad was
  culled. Removing the redundant native winding inversion restores the exact
  OpenGL result.
- The quad is exactly 48 by 48 pixels (2304 shaded pixels) inside a 64 by 64
  clear target. That coverage plus the exact image reference jointly asserts
  clip conversion, counter-clockwise winding, 16-bit indexed topology,
  reflected uniform/texture bindings, nearest texture sampling, and reverse-Z
  `GreaterEqual` drawing against a zero depth clear. A failure in any of those
  rules leaves the target clear, changes coverage, or changes the hash.
- Linear RGBA8 output is bit-exact across native OpenGL 4.6, packaged OpenGL
  4.1 fallback, and Vulkan 1.3:
  `f126a216776f58450335bc39732b8b1df604329cee2adc11e0bd9c8ec686937f`.
- sRGB RGBA8 output is also bit-exact across OpenGL 4.6, OpenGL 4.1 fallback,
  and Vulkan 1.3:
  `15f2cdf81d3b51469bb4082df1453ba60fccd40f043c7ff1e11a7ac211a4c53a`.
  The OpenGL peer now selects `GL_FRAMEBUFFER_SRGB` from the active attachment
  formats rather than inheriting ambient context state.
- `PipelineCacheDomain` keeps opaque device and driver compatibility strings
  below the backend boundary. OpenGL uses vendor/renderer plus GL and GLSL
  versions; Vulkan uses vendor/device IDs, `pipelineCacheUUID`, and driver
  version. The backend-neutral cache identity adds backend, target profile,
  shader semantic/toolchain hashes, complete immutable pipeline state, and the
  domain strings.
- Contract tests prove the cache identity is deterministic and that shader
  source, toolchain, pipeline format/state, target profile, backend, device, or
  driver changes each invalidate it. This defines safe future native-cache
  storage; R3 does not add a persistent disk cache.
- One acceptance sample on this machine recorded native creation evidence for
  identical cold/warm fixture runs. OpenGL 4.6 shader creation changed from
  42,466 microseconds to 27 microseconds and pipeline creation from 25 to 1
  microseconds. Vulkan shader creation changed from 325 to 94 microseconds and
  pipeline creation from 202 to 90 microseconds. These figures demonstrate the
  instrumented cold/warm path and stable identity, not a performance promise.
- Khronos validation, including synchronization validation, reports no API,
  synchronization, descriptor, layout, or lifetime errors for the linear,
  warm-repeat, and sRGB Vulkan fixtures. The only messages are the previously
  identified duplicate AMD switchable-graphics and OBS layer manifests.
- GHI contract tests pass 18/18. The shader package remains byte deterministic
  with SHA-256
  `cd051070bfc871480c94909a647d4a0ea78ef4388182349d741b986dec96b61a`.
- The renderer API-boundary ratchet passes: no Vulkan calls or types leak above
  the backend directory, no tracked legacy-coupling category grows, and raw GL
  calls outside backend directories decrease by three.
- The Vulkan-enabled Release viewer build passes and produces
  `build-vc170-64/newview/Release/firestorm-bin.exe`. Production world and UI
  rendering remain OpenGL-only.

## Live-grid offscreen verification tier

The synthetic R3 fixture remains the fast contract smoke test. A later
live-grid, offscreen harness will log in, consume actual simulator,
capabilities, object, terrain, avatar, and asset traffic, and continuously feed
the resulting backend-neutral render data to independent Native OpenGL,
Mesa+Zink, and Vulkan renderer workers.

- One authoritative grid-ingest process owns login, simulator state, asset
  decoding, scene timing, camera state, and render-frame identity. This avoids
  comparing three independently logged-in viewers whose interest lists, asset
  readiness, simulator updates, and frame timing inevitably drift.
- The ingest process publishes immutable, frame-numbered GHI resource updates
  and command streams. It never publishes OpenGL calls, Vulkan calls, native
  handles, driver cache objects, credentials, or raw capability secrets.
- Native OpenGL and Mesa+Zink require separate worker processes because the
  OpenGL implementation is selected at process load time. Vulkan runs as a
  third peer worker. Every worker consumes the same accepted frame packet.
- Workers render to offscreen color/depth/ID targets and do not present a
  swapchain image or construct interactive UI. OpenGL still owns a hidden WGL
  context; Vulkan uses offscreen images without a presentation surface.
- Comparisons are keyed by frame packet and resource-readiness epoch, not wall
  clock. Exact hashes are used where the contract is bit-exact; diagnostic
  tolerances are explicit for operations whose API/format rules permit bounded
  floating-point differences.
- Coverage includes static and rigged mesh, legacy and PBR materials, alpha
  modes/cards, particles on their intended legacy path, terrain, water,
  avatars, HUD-like geometry, and the explicit impostor/probe/mirror paths.
- Offscreen operation removes presentation, compositor, and interactive-UI
  cost. It does not remove geometry, shading, texture, synchronization, or
  readback cost; readback frequency is therefore configurable and asynchronous.

This is a distinct mode from the viewer's existing `HeadlessClient`, which
skips `display()` and disables rendering. The live-grid harness must keep render
production active while suppressing presentation. It does not replace the
small contract fixture: live failures must be reduced to focused fixtures
before changing a backend or the GHI contract.

## Remaining slices

R3 is complete. Live-grid scene ingestion, persistent native pipeline-cache
storage, and production renderer slices begin in later milestones; they are not
silently folded into this contract checkpoint.

## R3 exit gate

Status: PASS.

- Both native peers render the same fixed offscreen diagnostic from the same
  GHI command sequence.
- Vulkan validation reports no API, synchronization, or lifetime errors.
- Diagnostic images and semantic command hashes match their references.
- Clip convention, reverse-Z, winding, indexing, bindings, and color-space
  behavior have focused assertions.
- Cold and warm shader/pipeline evidence is recorded with cache identity and
  invalidation behavior.
- Production world/UI rendering remains wholly OpenGL until the later parity
  ledger authorizes a selectable production Vulkan backend.
