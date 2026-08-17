# R5 material, avatar, terrain, and lighting checkpoint

Date: 2026-08-17
Branch: `render/ghi-r5-material-lighting`

R5 extends the additive OpenGL/Vulkan peer foundation from rigid opaque
geometry into the material, animated-geometry, terrain, and environment
domains. Production world and UI rendering remains OpenGL-only throughout R5.
Mesa + Zink remains an OpenGL provider and is not the Vulkan reference.

## Slice map

- **R5a — material and skin contract:** sampled base-color, normal, ORM, and
  emissive textures; sRGB/linear format intent; sampler addressing; material
  factors; explicit joint indices and weights; matrix-palette skinning; and
  four deferred outputs.
- **R5b — production material and rigging seam:** extend the immutable live
  packet from R4d with content-addressed textures, material records, canonical
  skin data, resource epochs, and explicit comparability classifications.
- **R5c — terrain:** legacy and PBR terrain geometry, texture layers and
  transforms, height/normal behavior, and the applicable R08/R09 reference
  scenes.
- **R5d — lighting and shadows:** sun/moon inputs, local and projector lights,
  shadow maps and bias policy, and the accepted AMD OpenGL artifact references.
- **R5e — sky, atmosphere, and water:** environment state, atmospheric and
  water passes, underwater behavior, and their ordering/dependency contracts.

R5 does not absorb alpha algorithms, mirrors/probes/cube snapshots, or UI/HUD
work. Those remain R6, R7, and R8 respectively.

## R5a contract decisions

The backend-neutral material group contains one uniform block and four combined
image samplers: base color, tangent-space normal, occlusion/roughness/metallic,
and emissive. Base color and emissive images declare sRGB storage; normal and
ORM images declare linear UNorm storage. The shader therefore receives linear
sample values without API-specific manual gamma branches.

R5a uses explicit `UInt16x4` joint indices and `Float32x4` weights. This maps
directly to the viewer's glTF vertex data and gives both APIs the same numeric
interface. Older viewer mesh paths that encode joint indices in the integer
portion of `weight4` require a one-time canonicalization at the future R5b
packet/resource seam; that legacy packing is not made part of GHI.

The test palette contains four matrices. Two opposing translations are blended
at equal weights, producing the identity transform while still exercising
dynamic palette indexing and weighted matrix construction. This keeps raster
coverage stable enough for exact cross-API target hashes.

Combined image samplers are the portable R5a baseline because they correspond
to existing Firestorm GLSL `sampler2D` declarations and are implemented by both
native peers. Separate samplers/images and descriptor indexing remain valid
future optimizations, but are not required before representative production
material counts and rebinding costs are measured.

## R5a fixture and acceptance

The shared 64 x 64 fixture draws a skinned indexed quad and exercises:

- position, normal, tangent, UV, normalized vertex color, packed joint, and
  explicit weight inputs in one 76-byte vertex;
- frame, skin-palette, and material uniform groups;
- four 2 x 2 sampled textures, including hardware sRGB decode;
- nearest filtering with both clamp and repeat addressing outside the 0-1 UV
  range;
- tangent-space normal reconstruction and material-factor application;
- reverse-Z depth and the established base-color, ORM, normal, and emissive
  G-buffer targets.

Packaged OpenGL 4.4, the OpenGL 4.1 fallback, and Vulkan 1.3 produce 2,304 shaded
pixels and bit-exact hashes for every target:

1. base color: `b1c0a0fdf973dac4aebb8eac51dac984cf46b4b3727fc843033f94e81ab07b03`;
2. ORM: `85c747cd8d2957fa7b50524b040043d09796bd87240487bf8a31527dff80727b`;
3. normal: `989b3688b9b05357ae0e6a7c5704cfaa0072cba40e1cd0d7c44ba32e1956c538`;
4. emissive: `175704093dee3c161a3e279276bc137406866fc72c90a729c32e2dfd24543137`.

The deterministic shader package SHA-256 is
`aa20dfcd1b1a0f002922420ecb70594dc858c4cf28cf245f34e3bc879ac0e510`.
Reflection requires seven bindings, seven vertex inputs, and four fragment
outputs. Contract tests reject an incomplete four-texture material group and a
floating-point substitute for the unsigned joint-index interface.

Vulkan passes with Khronos validation and synchronization validation enabled.
The known duplicate AMD and OBS loader-manifest notices remain loader warnings,
not API validation failures.

The GHI contract suite is **23/23 pass**. All R3 draw and R4 opaque/visibility
reference hashes remain unchanged on both peers, including the OpenGL 4.1
fallback. The renderer API-boundary ratchet passes with no new native calls or
types above backend directories. The Vulkan-enabled Release viewer builds and
links successfully as `build-vc170-64/newview/Release/firestorm-bin.exe` while
production rendering remains OpenGL-only.

## R5b production material and rigging seam

R5b adds a separate version-1 `LLGHIM5B` packet instead of changing the
accepted R4 opaque packet schema. The packet carries three immutable resource
tables and submission records:

- decoded texture resources with independent source-asset and exact-content
  SHA-256 identities, explicit sRGB/linear intent, dimensions, component count,
  and discard level;
- legacy or metallic/roughness material records with factors, texture
  semantics and transforms, alpha classification, fullbright, double-sided,
  and environment inputs;
- canonical current skin palettes as row-major 3x4 affine matrices, with the
  mesh/static skin identity separated from current matrix content.

Source UUIDs are not treated as content hashes. If decoded pixels are not
already available, the record has no content identity and carries an explicit
`MissingCpuTexture` comparability reason. Blend materials are similarly marked
`AlphaDeferred`, because R6 owns their ordering algorithm. The observer never
calls `readbackRawImage()`, forces a texture fetch, waits for the GPU, or changes
the visible draw route.

Because Firestorm normally releases decoded pixels after texture upload, the
opt-in observer samples them at the existing CPU decode/upload boundary. It
retains at most 512 MiB and performs no retention unless
`VULKANSTORM_GHI_R5_CAPTURE` is set. Submission records are then collected from
the simple, alpha-mask, legacy material, glTF/PBR, and rigged deferred paths.
Resources are sorted by identity and references remapped before encoding, so
resource order does not depend on pointer values or first-use order. A resource
epoch changes only when the exact canonical resource set changes.

The packet inspector verifies all embedded content hashes, byte dimensions,
record boundaries, and absence of trailing data. The 120-second live grid
capture produced:

- 406 texture records, of which 219 contained and verified exact decoded
  content;
- 146 material records and 22 current skin palettes;
- 374 observed material draws, of which 214 were immediately comparable;
- 286,158,311 decoded texture bytes in a 286,292,927-byte packet;
- packet SHA-256
  `c80d5f56ac6a7140dca82a13e40e5793f50918b07fc03e3ca6eddcd7d76129c9`.

The GHI contract suite is now **24/24 pass**. It covers exact packet round-trip,
deterministic encoding, the stable schema hash
`057045d1efb57035a83dc1385d2f92a22ad2a4613845a2d4f2597376df3f67fa`,
truncation rejection, and rejection of pixel payloads without content
identity. The Vulkan-enabled Release viewer builds and links with `LL_TESTS`
restored to `OFF`. Enabling all legacy viewer tests also exposes pre-existing
`llurlmatch_test.cpp` signature drift; that unrelated target is not an R5b
failure and the dedicated GHI contract target remains 24/24 pass.

R5b establishes resource observation and comparability; it does not claim that
the full production material scene is rasterized by Vulkan. Production world
and UI rendering remains OpenGL-only. Terrain begins at R5c.

## R5c-e world and environment contract

R5c-e adds a backend-neutral world-state contract rather than exposing viewer
or native-API objects. It names the four terrain layers and their transforms,
legacy versus metallic/roughness terrain intent, height and normal policy,
sun/moon and local/projector light inputs, explicit shadow textures and biases,
and the sky/water/underwater inputs required by the R09 references.

The dependency order is also explicit:

1. terrain depth and terrain material;
2. directional and projector shadow production;
3. deferred lighting;
4. atmosphere and water reflection;
5. water surface and underwater classification.

A backend may fuse adjacent work, but it may not change the observable inputs
or outputs. This is intentionally a semantic dependency list, not a sequence of
OpenGL calls or Vulkan render-pass objects.

The shared R5 world fixture uses a reverse-Z indexed terrain quad and produces
three independent RGBA8 targets in one deterministic execution:

- **R5c terrain:** four weight-controlled texture layers, repeat sampling,
  independent transforms, height-modified weights, height-derived normals, and
  selection between legacy detail and PBR terrain treatment;
- **R5d lighting/shadows:** sun and moon directional inputs, distance/falloff
  local lighting, projector cone classification, sampled projector shadow, and
  explicit shadow bias;
- **R5e environment:** sky zenith/horizon and haze, animated wave inputs,
  water normal and Fresnel treatment, water-line classification, and an
  underwater override.

OpenGL 4.4, the OpenGL 4.1 fallback, and Vulkan 1.3 with Khronos validation
produce 2,304 shaded pixels and bit-exact hashes for every target:

1. terrain: `a5cf6a0b67b9227adaa765a0560512cfe3c559a7bcedd8461915c873c04b2cd2`;
2. lighting/shadows: `8b6f8bb2356985056adf8d073d48b1cc5ab4d5550518ac9175ba17c1a304f751`;
3. sky/water: `a7a89831e9fa70c1cdb73cf90e28ee896c956ebca8fb27d65d2697f4da1afa9b`.

The deterministic `r5_world` shader package SHA-256 is
`fbbdc73aec6f8e89a09352be95b97bf2483dd5bbd6ff0daf6150c70607542027`.
Reflection requires seven bindings, three vertex inputs, and three fragment
outputs. The contract suite rejects incomplete terrain/environment resource
sets, rejects a pipeline missing the environment target, and verifies the
world dependency order. The suite is **25/25 pass** and the renderer boundary
ratchet remains clean with zero direct Vulkan calls or Vulkan types outside the
backend.

The R08/R09 baseline evidence on the harness branch remains the production
reference: legacy/PBR terrain (including heightmap and paintmap composition),
deferred/local/projector lighting, shadows, directional sky, and above/below
water. The R5 fixture converts those requirements into a native-peer oracle;
it does not alter the production draw route or claim that a complete live grid
frame is already rasterized by Vulkan.

## R5 checkpoint result

R5 is complete as an additive GHI contract and native-execution checkpoint.
Material/skin, live material-resource observation, terrain, lighting/shadows,
and environment semantics now have portable interfaces and exact OpenGL/Vulkan
oracles. Production world and UI rendering is still OpenGL-only, so the viewer
remains usable through native OpenGL or Mesa + Zink while R6 begins alpha
coverage. Production Vulkan selection remains prohibited until R8.

## Limits and revisit points

R5 is an interface and native-execution oracle, not a claim that Firestorm's
production legacy/PBR shaders have been ported. It deliberately omits alpha
mask/blend behavior, texture arrays, anisotropic quality policy, mip generation
quality, material animation, morph targets, and cloth. Alpha enters R6;
recursive/offscreen environment rendering enters R7.

Revisit combined versus separate descriptors and descriptor indexing after R5b
records representative material/texture counts. Revisit palette UBO sizing and
storage-buffer use after actual avatar/animesh joint counts and update bandwidth
are captured. Neither decision should be driven by an API-specific shortcut.
Revisit pass fusion, terrain texture arrays, shadow-atlas layout, and atmosphere
lookup tables only after production packet replay provides representative
counts and timings; the semantic dependency order remains invariant.
