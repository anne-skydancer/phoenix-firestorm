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

Native OpenGL 4.6, the OpenGL 4.1 fallback, and Vulkan 1.3 produce 2,304 shaded
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

## Limits and revisit points

R5a is an interface and native-execution oracle, not a claim that Firestorm's
production legacy/PBR shaders have been ported. It deliberately omits alpha
mask/blend behavior, texture arrays, anisotropic quality policy, mip generation
quality, material animation, morph targets, cloth, and shadow/environment
lighting. Those enter only in the slice that owns their semantic decisions.

Revisit combined versus separate descriptors and descriptor indexing after R5b
records representative material/texture counts. Revisit palette UBO sizing and
storage-buffer use after actual avatar/animesh joint counts and update bandwidth
are captured. Neither decision should be driven by an API-specific shortcut.
