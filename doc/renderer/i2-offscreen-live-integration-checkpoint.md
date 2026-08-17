# I2 asynchronous offscreen live integration checkpoint

I2 proves that a bounded post-cull packet produced by the real viewer can be
executed by the retained native Vulkan device. It renders only into private
offscreen attachments. Production world, offscreen, HUD and UI rendering
remains OpenGL.

## Contract

- `RenderVulkanOffscreenPacketProbe` opts into I2 and requires the existing
  `RenderVulkanDeveloperProbe` coexistence device. When enabled it supersedes
  I1's transfer-only consumer for sampled packets.
- The I1 post-cull packet and safety ceilings remain the input contract. I2
  adds shader, pipeline, binding, draw, attachment-copy and readback execution.
- One sample may be in flight. Submission returns immediately; later OpenGL
  frames poll the readbacks with `AvailableOnly` behavior. `NotReady` defers
  completion without sleeping, spinning, waiting for a fence, or idling a
  queue.
- The isolated target is 256 by 256 pixels with four opaque G-buffer color
  attachments and the device's advertised preferred depth/stencil format.
  Every completed attachment receives a SHA-256 identity and non-clear-pixel
  count.
- The R4 Vulkan GLSL package is generated deterministically and staged under
  `app_settings/ghi_shaders`. Runtime code never embeds a build-machine path.
- I2 creates no window surface, swapchain, or presentation object and issues no
  presentation command. OpenGL remains the only visible renderer and Help >
  About continues to identify the selected OpenGL provider.
- Device loss disables further live sampling for the process. All other
  failures are reported without switching or disturbing the production
  renderer.

## Automated exit gate

1. The GHI contract suite passes **31/31**, including submission exclusivity,
   asynchronous polling, four attachment hashes, and retirement cleanup.
2. The renderer API-boundary ratchet remains at or below its accepted baseline.
3. The Release viewer links as `vulkanstorm-bin.exe`, and the generated R4
   package is present beside it under `app_settings/ghi_shaders`.
4. Source and staged settings XML parse successfully; the viewer manifest is
   syntactically valid.

## Windows live exit gate

Run one System OpenGL production-provider cell and one Mesa + Zink cell with
Khronos Vulkan validation enabled. Each cell must demonstrate:

1. at least one completed I2 sample after login and region settling;
2. at least one completed I2 sample after a completed teleport;
3. four nonempty attachment hashes and nonzero color coverage for drawable
   samples;
4. no Vulkan API, synchronization, lifetime, device-loss, surface, swapchain,
   or presentation error;
5. unchanged Help > About identity for the visible OpenGL provider; and
6. normal logout with offscreen resources retired before the Vulkan device and
   viewer window are destroyed.

I2 is complete only after both provider cells pass. It does not make native
Vulkan selectable for production rendering.

## Windows live evidence

Both Windows provider cells completed on 2026-08-17 with Vulkanstorm
`0.1-26.08.81421`, an AMD Radeon RX 9070 XT, native Vulkan adapter zero, and
Khronos validation enabled. The production renderer remained OpenGL.

### Mesa + Zink production renderer

- Six I2 samples completed. Samples before teleport reached 91,688 vertices
  and 393,150 indices; samples after `TELEPORT_NONE` reached 130,956 vertices.
- Every drawable sample produced four nonempty attachment hashes. Observed
  coverage ranged from 14,449 to 45,329 non-clear pixels per attachment.
- At least two samples completed after arrival. No Vulkan API, synchronization,
  lifetime, device-loss, surface, swapchain, or presentation error appeared.
- Normal logout retired the offscreen resources and native Vulkan device before
  the normal `Goodbye!` marker.

### System OpenGL production renderer

- The final cell used session-only limits of 20 samples at a 600-frame interval
  so the faster OpenGL provider could leave a practical manual-teleport window.
  Saved defaults remain six samples at a 300-frame interval.
- Seven samples completed before teleport. Three samples completed after
  `TELEPORT_NONE`, including packets at the 393,216-index ceiling.
- Every drawable sample produced four nonempty attachment hashes. Stable
  post-arrival coverage reached 61,750 non-clear pixels per attachment.
- No Vulkan API, synchronization, lifetime, device-loss, surface, swapchain, or
  presentation error appeared. Normal logout again retired the native Vulkan
  device before the `Goodbye!` marker.

The I2 Windows live exit gate is complete. Native Vulkan executed real viewer
geometry only in isolated offscreen targets; it did not render or present any
visible viewer content.
