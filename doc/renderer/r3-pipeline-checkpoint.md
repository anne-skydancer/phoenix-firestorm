# R3 shader, binding, and pipeline checkpoint

Date: 2026-08-16  
Branch: `render/ghi-r3-pipelines`

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
  attachments, viewport, scissor, and deterministic teardown.
- The validation fixture semantic SHA-256 is
  `c160ddad9c093695bd701120a7d96edb38cc0a945979c2dffa1ea666324ae925`.

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

1. R3e: Vulkan shader modules, descriptors, dynamic rendering, pipeline, and
   draw implementation under the Khronos validation layer.
2. R3f: fixed diagnostic images, semantic hashes, reverse-Z/clip/winding/sRGB
   checks, cold/warm cache evidence, full Release build, and boundary ratchet.

## R3 exit gate

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
