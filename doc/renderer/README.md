# Peer renderer architecture

This directory contains the contracts and design for adding a native Vulkan
renderer alongside the existing OpenGL renderer.

This is an additive peer-backend project. It is not a migration away from
OpenGL. OpenGL remains supported and selectable. Mesa + Zink remains a
separately scoped transitional OpenGL implementation and is neither the native
Vulkan backend nor the architectural reference for it.

Production world/UI rendering stays on OpenGL throughout development. Mesa +
Zink retirement is a separate post-parity decision; native OpenGL remains a
supported peer and recovery path.

- [Peer-backend architecture](peer-backend-architecture.md) defines the GHI
  seam, ownership rules, implementation increments, and acceptance gates.
- [GL coupling baseline](gl-coupling-baseline.json) anchors the source-level
  API-boundary ratchet.
- [I6 production terrain integration checkpoint](i6-production-terrain-integration-checkpoint.md)
  records the private native Vulkan terrain contract and live gate.
- [R1 lifecycle checkpoint](r1-lifecycle-checkpoint.md) records the gated
  native-Vulkan presentation slice and its validation evidence.
- [R2 resource checkpoint](r2-resource-checkpoint.md) records the neutral
  resource/transfer contract, validation model, and native-peer entry gate.
- [R3 pipeline checkpoint](r3-pipeline-checkpoint.md) records offline shader
  packaging, reflected bindings, deterministic draw parity, and cache identity.
- [R4 opaque-world checkpoint](r4-opaque-world-checkpoint.md) records the
  incremental G-buffer and opaque-geometry gates beginning with R4a.
- [R5 material, terrain, and lighting checkpoint](r5-material-lighting-checkpoint.md)
  records material/skin observation plus exact terrain, lighting/shadow, and
  sky/water native-peer contracts.
- [R7 offscreen and recursive checkpoint](r7-offscreen-checkpoint.md) records
  recursive-view policy, cube-array topology, dynamic/media update semantics,
  and exact offscreen native-peer evidence.
- [I0 runtime integration checkpoint](i0-runtime-integration-checkpoint.md)
  begins real-viewer integration by retaining an opt-in native Vulkan device
  beside the unchanged production OpenGL renderer.
- [I1 live packet integration checkpoint](i1-live-packet-integration-checkpoint.md)
  transfers bounded real-viewer post-cull geometry through that retained
  device without drawing or presenting it.
- [I2 asynchronous offscreen live integration checkpoint](i2-offscreen-live-integration-checkpoint.md)
  executes bounded real-viewer opaque geometry into isolated native Vulkan
  attachments and polls their hashes without presenting them.
- [I3 live rigid-material integration checkpoint](i3-live-material-integration-checkpoint.md)
  adds a tested geometry-bearing material packet and asynchronously executes
  bounded real-viewer rigid opaque PBR draws without presenting them.
- [I4 material-transform integration checkpoint](i4-material-transform-integration-checkpoint.md)
  separates object and skin transforms, applies inverse-transpose normals and
  executes independent per-map UV transforms on the live rigid PBR path.
- [I5 production skin integration checkpoint](i5-production-skin-integration-checkpoint.md)
  carries live variable-size palettes and canonical rigged geometry through
  the same private Vulkan material execution path.
- The accepted Windows renderer evidence is tagged
  `renderer-baseline-windows-r00-r14` on the test harness branch. The harness
  remains outside this production-renderer branch.
