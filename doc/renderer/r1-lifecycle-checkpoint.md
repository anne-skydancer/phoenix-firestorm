# R1 lifecycle checkpoint

Date: 2026-08-16  
Branch: `render/ghi`

This checkpoint is an additive native-Vulkan presentation slice. It does not
replace OpenGL and it does not route viewer world or UI rendering to Vulkan.
The production viewer continues to use its OpenGL peer while the incomplete
Vulkan peer remains gated by `USE_VULKAN_GHI=OFF` by default.

## Implemented

- Platform-window creation is independent from WGL-context creation. A real
  Win32 window can now be owned by an external renderer without loading
  `opengl32.dll` or selecting a WGL pixel format.
- Backend-neutral presentation creation, clear/present, resize, suspend, and
  shutdown contracts expose no Vulkan or Win32 types.
- The Vulkan backend privately owns its instance, physical/logical device,
  graphics/present queues, Win32 surface, swapchain, image views, per-frame
  acquire/fence state, and per-swapchain-image render-complete semaphores.
- The peer OpenGL adapter implements the same clear/present, resize, suspend,
  identity-publication, and teardown contract around a current WGL context.
- Vulkan publishes the shared renderer identity/capability snapshot consumed by
  renderer policy and diagnostics.
- Windows OpenGL and Vulkan snapshots use the same DXGI/Vulkan adapter LUID,
  allowing graphics settings to persist across API/provider changes on one
  physical GPU while retaining a reliable GPU-change key.
- Standalone OpenGL and Vulkan R00 lifecycle harnesses exercise create,
  repeated clear/present, resize, minimize/suspend, restore, and clean teardown.
  The Vulkan harness also supports Khronos validation and an expected-
  initialization-failure mode.

## Verification evidence

- Default Vulkan-disabled `llrender` build: PASS.
- Vulkan-enabled full Release viewer build: PASS.
- GHI contract tests: 5/5 PASS.
- R00 normal lifecycle on AMD Radeon RX 9070 XT: PASS.
- R00 OpenGL peer lifecycle on the same adapter: PASS.
- OpenGL/Vulkan physical-device identity: MATCH; both resolve to
  `luid:d2bf010000000000`.
- R00 lifecycle with `VK_LAYER_KHRONOS_validation`: PASS with no validation
  messages after synchronization repair.
- R00 forced invalid ICD: explicit `vkCreateInstance` failure, PASS; no silent
  OpenGL fallback.
- Renderer API boundary ratchet: PASS with zero growth in every category and no
  direct Vulkan calls or Vulkan types outside backend directories.

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

## Not yet claimed

R1 is not complete. Remaining work includes viewer-owned developer lifecycle
selection, semantic feature masking without OpenGL globals, display-change
coverage, and verifying that Help -> About and recommended-settings UI consume
a live Vulkan snapshot inside the viewer.
