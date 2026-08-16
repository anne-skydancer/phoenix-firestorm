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

## Remaining slices

1. R3c: complete validation semantics and one shared offscreen indexed fixture.
2. R3d: OpenGL shader, binding, pipeline, and draw implementation.
3. R3e: Vulkan shader modules, descriptors, dynamic rendering, pipeline, and
   draw implementation under the Khronos validation layer.
4. R3f: fixed diagnostic images, semantic hashes, reverse-Z/clip/winding/sRGB
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
