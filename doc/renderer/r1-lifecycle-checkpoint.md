# R1 lifecycle checkpoint — complete

Date: 2026-08-16  
Branch: `render/ghi`

This checkpoint is an additive native-Vulkan presentation slice. It does not
replace OpenGL and it does not route viewer world or UI rendering to Vulkan.
The production viewer continues to use its OpenGL peer while the incomplete
Vulkan peer remains gated by `USE_VULKAN_GHI=OFF` by default.

Production world/UI rendering remains OpenGL throughout all intermediate GHI
increments. Mesa + Zink remains available as the transitional OpenGL
correctness path; its retirement cannot be considered until complete native-
Vulkan parity and production eligibility have been verified.

## Implemented

- Platform-window creation is independent from WGL-context creation. A real
  Win32 window can now be owned by an external renderer without loading
  `opengl32.dll` or selecting a WGL pixel format.
- Backend-neutral presentation creation, clear/present, resize, suspend, and
  shutdown contracts expose no Vulkan or Win32 types. Display-topology changes
  have their own platform-neutral notification and force Vulkan surface-
  capability/swapchain re-evaluation.
- The Vulkan backend privately owns its instance, physical/logical device,
  graphics/present queues, Win32 surface, swapchain, image views, per-frame
  acquire/fence state, and per-swapchain-image render-complete semaphores.
- The peer OpenGL adapter implements the same clear/present, resize, suspend,
  identity-publication, and teardown contract around a current WGL context.
- Vulkan publishes the shared renderer identity/capability snapshot consumed by
  renderer policy and diagnostics.
- Renderer feature masks now consume semantic snapshot facts rather than
  `gGLManager`: vendor, physical memory, sampled-image/varying limits, and
  backend-defined baseline/advanced pipeline levels. The Intel pre-Haswell
  compatibility override remains intact inside the OpenGL owner.
- The existing OpenGL throughput probe is explicitly limited to OpenGL
  snapshots. A Vulkan lifecycle snapshot receives a conservative developer
  class until a future semantic GHI benchmark exists.
- Help -> About fields, renderer-change notifications, GPU persistence, logs,
  and support diagnostics derive their renderer names from the same canonical
  snapshot/support formatter.
- Windows OpenGL and Vulkan snapshots use the same DXGI/Vulkan adapter LUID,
  allowing graphics settings to persist across API/provider changes on one
  physical GPU while retaining a reliable GPU-change key.
- Standalone OpenGL and Vulkan R00 lifecycle harnesses exercise create,
  repeated clear/present, resize, display change, minimize/suspend, restore,
  and clean teardown. The Vulkan harness also supports Khronos validation and
  an expected-initialization-failure mode.

## Verification evidence

- Default Vulkan-disabled `llrender` build: PASS.
- Vulkan-enabled full Release viewer build: PASS.
- GHI contract tests: 6/6 PASS, including canonical Vulkan support/About data
  and semantic feature-level facts.
- R00 normal lifecycle on AMD Radeon RX 9070 XT: PASS.
- R00 OpenGL peer lifecycle on the same adapter: PASS.
- OpenGL/Vulkan physical-device identity: MATCH; both resolve to
  `luid:d2bf010000000000`.
- R00 lifecycle with `VK_LAYER_KHRONOS_validation`: PASS with no validation
  messages after synchronization repair.
- R00 forced invalid ICD: explicit `vkCreateInstance` failure, PASS; no silent
  OpenGL fallback.
- Renderer API boundary ratchet: PASS with zero growth in every category, three
  fewer direct GL calls, and no direct Vulkan calls or Vulkan types outside
  backend directories.
- Vulkan support remains disabled by default through `USE_VULKAN_GHI=OFF`.
  Enabling it builds only the developer lifecycle peer/harness; normal viewer
  world and UI rendering remains unconditionally OpenGL.

Observed native snapshot:

```text
Vulkan 1.4.349 (AMD Radeon RX 9070 XT - Native Vulkan)
stable-device-id=luid:d2bf010000000000
driver=AMD proprietary driver 26.7.1 (LLPC)
```

Observed peer OpenGL snapshot:

```text
OpenGL 4.6.0 Compatibility Profile Context 26.7.1.260716
    (AMD Radeon RX 9070 XT - System OpenGL)
stable-device-id=luid:d2bf010000000000
```

## R1 scope boundary

R1 is complete. It does not expose Vulkan as a normal viewer rendering choice:
doing so before world/UI parity would violate the production release invariant.
The selected Vulkan lifecycle is exercised by the developer-gated harness, and
its canonical support/About fields are verified without routing viewer draws to
it. Resource ownership, upload/readback, and semantic performance probes begin
in later increments.
