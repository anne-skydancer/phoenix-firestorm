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

## R4c dynamic instancing and visibility

R4c adds `Occlusion` to the backend-neutral query vocabulary and adds explicit
`beginQuery()` / `endQuery()` commands. Occlusion queries are legal only inside
a rendering scope, may not overlap, must match at end, and must be reset before
reuse. An active query prevents the rendering scope from ending. Results remain
asynchronous and cannot be read during an active frame. Timestamp operations
reject occlusion pools and occlusion operations reject timestamp pools.

The new trace opcodes were appended, so every previously recorded semantic
opcode retains its numeric identity. The validation peer records a deterministic
visibility surrogate for contract tests; native peers return hardware sample
counts. Capability publication now corresponds to an implemented command path
instead of advertising an absent interface.

Scene and frustum culling deliberately remain renderer policy above GHI. GHI
owns explicit rasterizer `CullMode`, query lifetime, query commands, and result
availability; it does not own Firestorm's scene graph or choose which objects
to submit.

The separate `r4_visibility` package has no resource bindings and reflects two
per-vertex attributes plus two per-instance attributes. Its byte-deterministic
package SHA-256 is
`c6a025732db61ffa6361e6b9b9aab92649daaf0959ab3d456b40ba595da0ad63`.
Keeping it separate preserves the R4b package and four established G-buffer
hashes.

The shared fixture executes two frames. Each frame draws three instances of a
near quad, then submits identical far geometry behind it. The second frame
copies changed instance offsets and colors into the same device-local instance
buffer. This exposed and repaired a Vulkan inter-frame write-after-write hazard:
buffer copies now establish a destination-scoped dependency before rewriting a
buffer that an earlier submission may still consume. A broad device idle or
global frame barrier is not used.

OpenGL 4.1 cannot portably express a nonzero base instance without an extension,
so R4c fixes `firstInstance` at zero and uses the bound instance-buffer offset
for portable selection. A future nonzero-base-instance contract requires an
explicit capability; it must not become a vendor branch.

## R4c verification

- Native OpenGL 4.6, the packaged OpenGL 4.1 fallback, and Vulkan 1.3 produce
  bit-exact two-frame results:
  - frame 0: `0b5cd61f5abad7022d68402f74b34ae539a2fb25fec6755a2864745d3bb150ae`;
  - frame 1: `bec88ce09217d8c9456bdd6a656f5b53e0391efd37f17a60ab05c1df1a2df504`.
- On the tested RX 9070 XT, both native peers report 408 samples for each
  visible instanced draw and zero for each fully depth-occluded draw. The
  portable acceptance rule is positive versus zero; exact nonzero sample counts
  are diagnostic evidence, not a cross-device contract.
- Vulkan passes with Khronos validation and synchronization validation enabled.
  The initial dynamic-buffer hazard is gone; only the known duplicate AMD and
  OBS loader-manifest notices remain.
- The GHI contract suite is **21/21 pass**, including pool-type rejection,
  rendering-scope rules, non-overlap, matching begin/end, reset-before-reuse,
  asynchronous result access, and a four-instance semantic query.
- R3 linear/sRGB draw hashes, all four R4b target hashes, and both native R2
  resource fixtures remain unchanged. OpenGL 4.1 fallback results remain exact.
- R3, R4b, and R4c packages pass byte-determinism and deliberate
  reflection-mismatch tests. The negative packager test now supports packages
  with no descriptor bindings by corrupting another reflected interface.
- Renderer API-boundary ratchet: **pass**; no Vulkan calls or types leak above
  the backend directory and no tracked coupling category grows.
- Vulkan-enabled Release viewer build and link: **pass**, producing
  `build-vc170-64/newview/Release/firestorm-bin.exe`. Production world and UI
  rendering remain OpenGL-only.

## R4d production seam and live opaque replay

R4d adds a dormant observation seam to the existing production OpenGL path.
For the main viewer camera, `LLPipeline::renderGeomDeferred()` brackets capture
and the legacy render-pass submission functions expose the already selected,
post-cull `LLDrawInfo` work. The visible frame continues through the existing
OpenGL renderer without interception or replacement.

The observer accepts only rigid legacy `PASS_SIMPLE` indexed geometry in this
checkpoint. It copies backend-neutral vertex/index data from the persistent CPU
shadow owned by `LLVertexBuffer`, records the canonical clip transform, and
serializes an immutable versioned packet. It does not read native GL objects
back from the driver. Rigged draws are counted but excluded; material/PBR,
fullbright, alpha, particles, mirrors, reflection/hero probes, cube snapshots,
HUDs, impostors, and other recursive or offscreen phases remain owned by later
R5-R7 checkpoints.

Capture is disabled unless `VULKANSTORM_GHI_R4_CAPTURE` names an output file.
`VULKANSTORM_GHI_R4_WARMUP_SECONDS` controls the one-shot settling interval and
defaults to 120 seconds. The packet contains no login credentials, capability
URLs, chat, inventory names, or network payloads.

Both native opaque harnesses can replay the same packet into offscreen GHI
targets. They use device-local buffers, per-draw transforms, reverse-Z depth,
and the four established R4 G-buffer targets. The comparator checks packet and
scene structure, normalizes readback to top-left orientation, and applies
explicit per-target pixel and coverage tolerances. This diagnostic replay is
not the production batching design and is not presented to the display.

## R4d live evidence

The Release viewer logged in automatically at the saved home location and
remained responsive. The log identifies the region as UUID
`953886b5-1611-45f2-9aaf-3d13f7419bdf`, handle `759762535144960`; it did not
emit a human-readable region name, so this record does not claim that it was
the earlier named R01 location. After the full 120-second settling interval,
the production seam wrote:

- packet: `opaque-r4d-20260817-004702.llghiop4`;
- SHA-256: `40364666cbfc4c679862caea3f546f6a56ab04b02314b88e41b78dbd292d4665`;
- source frame/extent: frame 18476 at 2560 x 1350;
- production occlusion: enabled;
- submitted: 150 draws and 126,542 triangles;
- comparable rigid slice: 146 draws and 79,974 triangles;
- explicitly excluded: 4 rigged draws;
- invalid draws: zero.

The peers replayed that packet at 512 x 512. With strict limits of at most
eight differing pixels and one pixel of coverage delta per target, comparison
passed:

| Target | OpenGL coverage | Vulkan coverage | Differing pixels |
|---|---:|---:|---:|
| base color | 60,614 | 60,615 | 5 |
| ORM | 60,619 | 60,620 | 3 |
| normal | 60,619 | 60,620 | 3 |
| emissive | 60,614 | 60,615 | 5 |

The largest difference is five edge pixels out of 262,144 pixels, about
0.0019%. Arbitrary live triangles therefore use this bounded raster-edge rule;
the synthetic R4b fixture remains the bit-exact storage, depth-selection, and
target-state oracle. Its four reference hashes remain unchanged on OpenGL 4.6,
the OpenGL 4.1 fallback, and Vulkan 1.3.

The accepted R01/R02 Native OpenGL and Mesa+Zink captures remain the external
viewer reference envelopes. R4d demonstrates that the applicable rigid/simple
post-cull geometry selected by production `LLPipeline` crosses the new seam and
reaches parity on both native GHI peers. It does not assert parity for the
rigged and material geometry deliberately assigned to R5.

## R4 closure

R4 is closed for its declared rigid opaque scope:

- the production selection seam is established without changing visible
  world or UI routing;
- a versioned, deterministic live opaque packet is validated and replayable;
- structural evidence and all four deferred targets pass the stated rule;
- the synthetic exact oracle, OpenGL 4.1 fallback, Vulkan validation, viewer
  build, and API-boundary ratchet remain clean.

Production rendering remains OpenGL-only. R5 is the next checkpoint for
legacy/PBR materials, rigging/skinning, terrain, lighting, shadows, sky,
atmosphere, and water.
