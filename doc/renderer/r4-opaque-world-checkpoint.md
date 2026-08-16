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

## R4b native static opaque fixture

The R4 opaque shader package adds a frame transform, a simple material block,
packed normalized vertex color, and four reflected fragment outputs. Its
offline package is byte deterministic with SHA-256
`38c7b923cd01e9321d16bb9d7bce2993b48d224591d86428224e687c626280c8`.

The shared offscreen fixture draws two overlapping, counter-clockwise indexed
quads at different depths. It therefore exercises static vertex/index data,
`UNorm8x4` color conversion, simple material bindings, back-face culling, and
reverse-Z `GreaterEqual` depth selection. Its targets are:

1. RGBA8 base color;
2. RGBA8 ORM;
3. RGBA16 normalized normal data;
4. RGBA16 float emissive data.

The four targets use distinct color write masks. The OpenGL peer now applies
blend enable, blend factors/equations, and write masks with indexed attachment
entry points instead of reusing target zero's state globally. Vulkan requires
and enables the core `independentBlend` feature when available. Both peers
publish this as the backend-neutral `independentBlend` capability and reject a
pipeline requiring distinct states when it is unavailable.

An exact-format probe also established that the tested AMD Vulkan
implementation does not support three-component `RGB16Float` for the required
image/attachment combination. GHI keeps that format explicit and permits an
`Unsupported` result; it never substitutes storage silently. The common R4b
fixture uses `RGBA16Float`, whose alpha channel is semantically unused, as the
portable emissive target. Future viewer target policy must make that choice
above the backend boundary.

## R4b verification

- Native OpenGL 4.6, the packaged OpenGL 4.1 fallback, and Vulkan 1.3 produce
  bit-exact results for every target:
  - target 0: `4976657fd6e31431fd86dbd9d96624938294208ade4bb9a4a7da04ee816e3c64`;
  - target 1: `24fd8e8c34e313665797af0b0b5ade49486a0f71dc3e58cceba14d0f7efd3ce6`;
  - target 2: `43daa53d0fb687a99cb761a89248b1eef93340d86633634f686501327c99169b`;
  - target 3: `f03533b1c62faf9e85948f8e93260f935e1e16c5fefca87e4db17fe2cfe84803`.
- OpenGL selects D24S8 and Vulkan selects D32FS8 on this machine. The
  overlapping geometry and exact color results verify common reverse-Z depth
  semantics without pretending that unlike raw depth/stencil encodings should
  have the same byte hash.
- Khronos validation, including synchronization validation, reports no API,
  feature, attachment, layout, or lifetime errors. The known duplicate AMD and
  OBS loader-manifest notices remain non-validation warnings.
- Both R3 shader/draw references remain unchanged, both R3 and R4 packages pass
  byte-determinism and deliberate reflection-mismatch tests, and the GHI
  contract suite remains **20/20 pass**.
- Native OpenGL and Vulkan resource fixtures pass, including explicit handling
  of optional exact formats. The renderer API-boundary ratchet reports no
  native API leakage or tracked coupling growth.
- The Vulkan-enabled Release viewer builds and links successfully. Production
  world and UI rendering remain OpenGL-only.

## R4c entry and remaining R4 gate

R4b still does not route `LLPipeline` through GHI, ingest grid scene packets,
or claim R01/R02 viewer parity. R4c must add changing and instanced geometry,
explicit occlusion query commands, and culling/visibility fixtures. Only then
can the applicable accepted baseline evidence be compared and R4 considered
for closure.
