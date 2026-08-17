# R7 offscreen and recursive rendering checkpoint

Date: 2026-08-17  
Status: native-peer checkpoint complete; production rendering remains OpenGL

## Outcome

R7 establishes backend-neutral contracts and exact native execution for the
resource and scheduling patterns shared by reflection probes, hero probes,
mirrors, cube snapshots, impostors, media surfaces, and dynamic textures.
It is an additive peer-backend checkpoint. It does not reroute production
world, offscreen, or UI rendering away from OpenGL, and it does not change the
Native OpenGL or Mesa + Zink selector.

The accepted R07, R11, R12, and R13 evidence remains the production reference
on `test/renderer-baseline-harness`, tagged
`renderer-baseline-windows-r00-r14`. R7 converts those requirements into a
small deterministic oracle that both native GHI peers execute independently.

## Pass and recursion contract

`OffscreenPassDesc` identifies the semantic view class, cube face, probe
phase, array layer, mip, update epoch, and recursion depth without exposing
OpenGL or Vulkan objects. The following invariants are enforced:

- only the main view may schedule an offscreen world render;
- offscreen views have recursion depth one and may not schedule another
  offscreen world view;
- cube faces use the portable `+X, -X, +Y, -Y, +Z, -Z` layer order;
- face and array-layer identities must agree;
- semantic IDs contain stable scene/pass data, not native handles, timestamps,
  or process-specific addresses.

Reflection/hero probes, mirrors, cube snapshots, impostors, dynamic textures,
and media surfaces map to the legacy alpha route. PPLL and depth peeling remain
limited to the main post-water view, preserving the R6 performance and
correctness decision for recursive/offscreen work.

## Resource contract

`ImageDesc::cubeCompatible` and `ImageViewType` separate allocation topology
from view intent. A cube-compatible image is a square 2D array with complete
six-layer groups. It can expose:

- a cube-array sampling view covering complete cube groups; and
- a one-layer 2D attachment view for rendering an individual face.

OpenGL implements this with `GL_TEXTURE_CUBE_MAP_ARRAY` and
`glFramebufferTextureLayer`. Vulkan uses
`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`, explicit image-view types, and enables
the queried `imageCubeArray` feature. `RendererCapabilities::cubeMapArrays`
publishes the same semantic capability through the renderer snapshot used by
feature policy and Help > About diagnostics.

The color-attachment-to-sampled-read dependency is now explicit in the GHI.
Vulkan transitions every mip covered by a sampled view rather than only its
base mip. Repeated buffer-to-image updates establish a transfer-write
dependency, covering the main-thread handoff pattern used by dynamic and media
textures. OpenGL accepts cube arrays in upload, readback, mip generation, and
layered attachment paths.

## Exact native-peer fixture

The `r7_offscreen` fixture performs one representative frame:

1. upload an 8x8 dynamic texture;
2. initialize an 8x8 media texture and apply a four-column partial update from
   a six-pixel row pitch;
3. render six unique colors into six individual faces of one cube-compatible
   probe image;
4. generate the complete five-level probe mip chain;
5. sample every canonical cube direction at base and generated mip levels,
   plus both updated 2D textures, into a 64x64 result; and
6. read back and hash the exact RGBA8 result.

Vulkan 1.3 with Khronos synchronization validation, OpenGL 4.6, and the
OpenGL 4.1 shader fallback all produce 4,096 shaded pixels and SHA-256:

`a7672b031451f5ccb87aaa7ad8f5d1cfee57fea0ff9f75302a60720551377bf5`

The shader package is built offline for Vulkan GLSL, OpenGL 4.6, and OpenGL
4.1 and participates in the byte-determinism and reflection-rejection gate.
Its three reflected resources are the probe cube array, dynamic texture, and
media texture.

## Validation evidence

- GHI contract suite: **28/28 pass**.
- Vulkan R7 fixture: PASS with Khronos validation and synchronization
  validation enabled; only loader duplicate-layer notices were reported.
- OpenGL R7 fixture: PASS through both OpenGL 4.6 and OpenGL 4.1 package
  artifacts with the same exact hash.
- Shader package: deterministic and reflection mismatch rejection PASS.
- `llrender` Release build: PASS.

## Limits and next checkpoint

The native fixture proves the shared offscreen resource, view, upload,
synchronization, recursion, alpha-routing, and shader contracts. It does not
claim that production Firestorm probe, mirror, cube-snapshot, impostor, or
media draw code is already submitted to native Vulkan. Those production paths
continue to render through OpenGL while the peer backend is developer-gated.

R8 owns UI/HUD rendering, picking and selection, snapshot presentation and
readback integration, device-loss reporting, content-heavy region-entry
stress, full hardware coverage, and the explicit production-selection
decision. Completing R7 does not retire Mesa + Zink and does not change the
default renderer.
