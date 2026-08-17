# I1 live post-cull packet integration checkpoint

I1 proves that bounded scene data produced by the real viewer can cross the
GHI seam and be consumed by the retained native Vulkan device. Production
world, offscreen, HUD and UI rendering remains OpenGL.

## Contract

- `RenderVulkanDeveloperProbe` still owns the opt-in native Vulkan device.
- `RenderVulkanLivePacketProbe` additionally enables I1 packet sampling. It is
  inert when the coexistence device is absent.
- The existing rigid opaque post-cull producer supplies real viewer vertex,
  index, transform, frame, scene and occlusion data. Simulator messages,
  capability URLs, credentials and native graphics handles never enter the
  packet.
- Each accepted sample is schema-validated and assigned a SHA-256 identity.
- Native Vulkan receives one upload buffer and device-local vertex, index and
  transform buffers. A transfer-only GHI frame copies the payloads and retires
  all temporary handles through the configured in-flight window.
- I1 creates no image, image view, shader, pipeline, binding set, render pass,
  readback or presentation object. It records no draw call. OpenGL remains the
  only visible renderer and the active renderer snapshot is unchanged.
- A device-loss result disables further I1 sampling for that process. Other
  rejected samples are reported rather than weakening validation.

## Fixed safety ceilings

The live path is deliberately stricter than the packet file format:

| Resource | Per-sample maximum |
|---|---:|
| draws | 256 |
| vertices | 131,072 |
| indices | 393,216 |
| combined upload allocation | 16 MiB |

When only the runtime probe is active, the producer stops adding draws before
these element ceilings are exceeded and records that the capture was
budget-limited. The consumer independently enforces every ceiling as well as
the selected device's buffer and uniform-buffer limits.

`RenderVulkanLivePacketIntervalFrames` defaults to 300 production frames.
`RenderVulkanLivePacketMaxSamples` defaults to six successful samples per
process. Failed attempts are capped at four times the requested sample count,
which prevents a malformed live scene from becoming an unlimited retry loop.

## Automated evidence

- The transfer consumer is backend-neutral and covered by the validation
  device. It verifies three copy commands, deterministic packet identity,
  immediate GHI-handle invalidation and deferred native-resource retirement.
- The GHI contract suite passes **30/30**.
- The renamed Release viewer target links as `vulkanstorm-bin.exe`.
- The renderer API-boundary ratchet must remain at or below its accepted
  baseline; I1 introduces no native Vulkan call or Vulkan type above the
  backend boundary.

## Windows live exit gate

I1 closes after both System OpenGL and Mesa + Zink production-provider cells
complete an opt-in run with Khronos validation enabled and demonstrate:

1. at least one successful I1 transfer after login and region settling;
2. at least one successful I1 transfer after a completed teleport;
3. no Vulkan API, synchronization, lifetime or device-loss report;
4. unchanged Help > About identity for the visible OpenGL provider; and
5. normal logout with the coexistence device drained before window teardown.

No image comparison is expected at I1 because native Vulkan intentionally
produces no image. The next checkpoint may execute the captured opaque packet
into isolated offscreen Vulkan targets only after this transfer boundary is
stable.

## Windows live evidence

Both Windows provider cells completed on 2026-08-17 with Vulkanstorm
`0.1-26.08.81420`, an AMD Radeon RX 9070 XT, adapter zero, and Khronos
validation enabled.

### System OpenGL production renderer

- Help > About retained OpenGL 4.6 from Adrenalin 26.7.1 and reported
  `Rendering Provider: System OpenGL`.
- Four bounded packets transferred before teleport and two transferred after
  arrival. Successful samples reached up to 88,808 vertices and the fixed
  393,216-index ceiling without exceeding the allocation limits.
- A packet sampled during `TELEPORT_MOVING` contained no comparable draws and
  was rejected before resource creation. Sampling resumed successfully after
  `TELEPORT_NONE`.
- No Vulkan API, synchronization, lifetime or device-loss error was reported.
- Normal logout drained the retained Vulkan device before viewer-window
  destruction and ended with the normal `Goodbye!` marker.

### Mesa + Zink production renderer

- Help > About retained OpenGL 4.6 from Mesa 26.3 and reported
  `Rendering Provider: Mesa + Zink`.
- Two bounded packets transferred before teleport and at least two transferred
  after arrival. Successful samples reached the fixed 393,216-index ceiling
  while remaining below 3 MiB of combined upload data.
- The in-transition empty packet was again rejected before resource creation;
  post-arrival transfers resumed immediately.
- No Vulkan API, synchronization, lifetime or device-loss error was reported.
- Normal logout drained the retained Vulkan device before viewer-window
  destruction and ended with the normal `Goodbye!` marker.

The I1 Windows exit gate is therefore complete. Production rendering remained
OpenGL throughout both runs; native Vulkan performed transfer-only diagnostic
consumption and never created or presented an image.
