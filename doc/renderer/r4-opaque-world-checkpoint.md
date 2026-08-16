# R4 opaque-world checkpoint

Date: 2026-08-16  
Branch: `render/ghi-r4-opaque-world`

R4 begins the opaque-world foundation for the additive, parallel Vulkan peer.
It does not route production world or UI rendering away from OpenGL, expose an
incomplete Vulkan renderer to users, or use Mesa + Zink as the native Vulkan
reference.

## Slices and exit gates

- **R4a — interface and G-buffer contract:** distinguish shader values from
  storage formats, reflect fragment outputs, validate multiple render targets,
  and add exact normal-buffer formats already used by Firestorm.
- **R4b — native static opaque fixture:** execute one representative deferred
  static-geometry pass through native OpenGL and native Vulkan, with depth,
  four independently declared color targets, and deterministic readback from
  every target.
- **R4c — changing geometry and visibility:** add dynamic and instanced
  geometry plus explicit culling and occlusion commands and fixtures.

R4 closes only when the applicable accepted R01/R02 baseline geometry and
deferred-target evidence reaches parity. A synthetic fixture passing is a
necessary contract gate, not a claim of viewer parity.

## R4a decisions

Shader reflection describes the numeric value delivered to or written by a
shader stage. Resource and pipeline descriptors separately describe the byte
layout that stores that value. This distinction is required by existing viewer
geometry:

- normalized `UNorm8x4` vertex colors satisfy a shader `float4` input;
- packed `UInt16x4` joint indices satisfy a shader `uint4` input;
- multiple compatible storage formats may satisfy the same fragment-output
  value shape.

Shader-package schema v3 therefore replaces reflected vertex `VertexFormat`
with `ShaderValueType` and adds reflected fragment-output locations and types.
The runtime rejects duplicate locations, missing color targets, and numeric
type/shape mismatches. This is a deliberate package-format break: old packages
are rejected rather than guessed or silently upgraded.

The rendering-pass validator now also rejects:

- a pass exceeding the device's color-attachment limit;
- duplicate color attachment views;
- a depth/stencil view aliased with any color attachment;
- a pipeline whose reflected fragment outputs cannot be represented by its
  declared color target list.

The format vocabulary adds `RGB10A2UNorm` and `RGBA16UNorm`, matching the two
normal-buffer choices in `LLPipeline::addDeferredAttachments()`. OpenGL maps
them to `GL_RGB10_A2` and `GL_RGBA16`; Vulkan maps them to
`VK_FORMAT_A2B10G10R10_UNORM_PACK32` and
`VK_FORMAT_R16G16B16A16_UNORM`.

## R4a verification

- GHI contract suite: **20/20 pass**, including packed color/joint interfaces,
  four reflected fragment outputs, missing/wrong target rejection, and MRT
  alias rejection.
- Shader packaging: schema v3 is byte deterministic; SHA-256
  `58939148fafa5bbda07577fbf3bb5390d1a2915ab04bf8b29709e096b916a5ff`.
- Native OpenGL and Vulkan resource fixtures accept the appended
  `RGB10A2UNorm` and `RGBA16UNorm` formats; Vulkan passes with Khronos
  validation enabled.
- Native OpenGL 4.6 and 4.1 fallback retain the exact R3 linear and sRGB image
  hashes.
- Native Vulkan 1.3 with Khronos validation retains the exact R3 linear and
  sRGB image hashes; no API, synchronization, layout, descriptor, or lifetime
  validation errors were reported. Loader-only duplicate-layer warnings remain
  unchanged.
- Renderer API-boundary ratchet: **pass**; no Vulkan calls or types leak above
  the backend directory and no tracked legacy-coupling category grows.
- Vulkan-enabled Release viewer build and link: **pass**, producing
  `build-vc170-64/newview/Release/firestorm-bin.exe` while production world/UI
  rendering remains OpenGL-only.

## R4a non-goals and R4b entry

R4a does not yet claim independent per-attachment blending in the native
OpenGL peer, emit a real four-target native draw, implement occlusion query
commands, ingest grid scene packets, or route `LLPipeline` through GHI. Those
are intentionally visible gaps rather than hidden fallbacks.

R4b starts with an offscreen static opaque fixture whose target declarations
mirror the viewer deferred pass. It must run the same immutable input packet on
both native peers, hash every color target and depth result, exercise reverse-Z
and culling, and prove that attachment-specific state is implemented rather
than collapsed to OpenGL's first blend state.
