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
- The accepted Windows renderer evidence is tagged
  `renderer-baseline-windows-r00-r14` on the test harness branch. The harness
  remains outside this production-renderer branch.
