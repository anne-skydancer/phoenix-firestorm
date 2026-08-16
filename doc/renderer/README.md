# Peer renderer architecture

This directory contains the contracts and design for adding a native Vulkan
renderer alongside the existing OpenGL renderer.

This is an additive peer-backend project. It is not a migration away from
OpenGL. OpenGL remains supported and selectable. Mesa + Zink remains a
separately scoped transitional OpenGL implementation and is neither the native
Vulkan backend nor the architectural reference for it.

- [Peer-backend architecture](peer-backend-architecture.md) defines the GHI
  seam, ownership rules, implementation increments, and acceptance gates.
- [GL coupling baseline](gl-coupling-baseline.json) anchors the source-level
  API-boundary ratchet.
- [R1 lifecycle checkpoint](r1-lifecycle-checkpoint.md) records the gated
  native-Vulkan presentation slice and its validation evidence.
- The accepted Windows renderer evidence is tagged
  `renderer-baseline-windows-r00-r14` on the test harness branch. The harness
  remains outside this production-renderer branch.
