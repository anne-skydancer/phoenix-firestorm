# R2 resource checkpoint — contracts and validation

Date: 2026-08-16  
Branch: `render/ghi-r2-resources`

R2 adds backend-neutral resource and transfer semantics without routing viewer
world or UI rendering away from OpenGL. This first checkpoint defines and tests
the contract before either native resource peer is accepted.

## Contract decisions

- Buffers have explicit device-local, upload, or readback memory classes.
- Host writes are valid only for upload buffers. Host reads are valid only for
  readback buffers after their producing frame completes.
- Buffer and image transfers are recorded commands inside a frame and outside
  a rendering pass.
- Image views own an explicit format, aspect, mip range, and array-layer range.
- Mip generation names its exact subresource range.
- Timestamp queries use typed pools. Result reads are nonblocking
  by default and return `NotReady`; a wait must be explicitly requested.
- Resource destruction invalidates the generational handle immediately while
  the backend retains the native allocation through the configured in-flight
  frame window. `waitIdle()` explicitly drains deferred retirement.
- Parent resources must outlive dependent views and pipelines.
- Existing semantic trace opcode values remain stable. R2 operations append new
  opcodes rather than renumbering the R0/R1 command stream.

## Validation model

The non-rendering validation backend maintains deterministic CPU backing for
staging buffers and image subresources. It executes buffer copies,
buffer-to-image uploads, image-to-buffer readbacks, and byte-UNorm mip
generation so fixtures can verify exact output rather than only call success.

The model rejects:

- missing transfer usage;
- empty, overflowing, or out-of-bounds regions;
- invalid image aspects, mip levels, layers, extents, row pitches, and views;
- host access to the wrong memory class or during an active frame;
- stale handles and parent destruction before dependent objects;
- index-buffer misalignment and 16/32-bit indexed-draw overruns;
- query reuse without reset and nonblocking reads before availability.

## Scope and trade-offs

- Validation mip generation currently supports byte-UNorm color formats. Other
  formats fail explicitly as `Unsupported`; they are not silently processed
  with incorrect numeric or sRGB averaging.
- The existing production OpenGL renderer remains untouched. Native OpenGL and
  Vulkan R2 resource implementations follow only after these fixtures pass.
- R2 does not introduce shader bindings or diagnostic geometry; those remain
  R3 responsibilities.

## Verification evidence for this slice

- GHI contract tests: 12/12 PASS. They cover upload/copy/readback, mip
  generation, image-view lifetime, timestamp queries, deferred destruction,
  16/32-bit indices, and representative color/depth formats.
- Legacy R0 semantic trace hash: unchanged and PASS.
- Vulkan-enabled full Release viewer build: PASS; production rendering remains
  OpenGL-only.
- Renderer API-boundary ratchet: PASS with no new native API leakage.
- Both native resource peers remain pending; therefore this checkpoint does not
  claim R2 completion.
