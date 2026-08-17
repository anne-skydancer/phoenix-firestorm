# I0 native Vulkan runtime integration checkpoint

I0 is the first viewer-process integration checkpoint after the completed GHI
R0-R8 foundation. It does not select native Vulkan for production rendering.

## Contract

- Production world, offscreen and UI rendering remains OpenGL.
- `RenderGLBackend` continues to select the OpenGL provider independently:
  native OpenGL, Mesa + Zink, or Auto.
- `RenderVulkanDeveloperProbe` is an opt-in developer setting. When enabled in
  a build compiled with `USE_VULKAN_GHI=ON`, the viewer creates a native Vulkan
  GHI device after OpenGL initialization and retains it for the process lifetime.
- The coexistence device never publishes the active renderer snapshot. About,
  feature policy and persisted renderer identity continue to describe the
  production OpenGL renderer.
- Shutdown drains and destroys the Vulkan device before the OpenGL context and
  native window are destroyed.
- Creation failure is non-fatal and leaves the usable OpenGL viewer unchanged.

`RenderVulkanAdapterIndex` selects the temporary zero-based Vulkan adapter for
this checkpoint. Stable physical-device matching across APIs is required before
native Vulkan can become a user-facing production choice.

`RenderVulkanValidation` enables Khronos API and synchronization validation for
the coexistence device. It is deliberately independent of production settings.

## Exit gate

I0 closes when the default OpenGL path remains unchanged and an opt-in run can
hold the native Vulkan device through login, region entry, teleport, idle and
clean shutdown without validation errors, device loss or OpenGL regression.

The next checkpoint consumes live post-cull viewer packets on this retained
device without presenting them. Native Vulkan presentation remains prohibited
until complete production integration and the existing R8 eligibility policy
are satisfied.

## Windows live evidence

### Mesa + Zink production renderer

On 2026-08-17, the Release configuration completed an opt-in Windows run with
Khronos validation enabled on adapter zero:

- Mesa + Zink remained the visible OpenGL provider and Help > About reported it
  as the active renderer throughout the run.
- The native Vulkan GHI coexistence device initialized on the same AMD GPU and
  retained the expected attachment, sampled-image, storage-buffer, timestamp,
  storage-atomic and cube-array capabilities.
- Login, region settling and two completed teleports produced no GHI device
  loss or integration failure.
- Normal logout drained and destroyed the Vulkan device before OpenGL and the
  viewer window were destroyed, followed by a normal viewer shutdown.

### Native OpenGL production renderer

On 2026-08-17, the Release configuration completed the corresponding opt-in
Windows run with Khronos validation enabled on adapter zero:

- System OpenGL remained the visible provider and Help > About reported AMD
  OpenGL 4.6 from Adrenalin 26.7.1 as the active renderer throughout the run.
- The native Vulkan GHI coexistence device initialized on the same AMD GPU and
  retained the same device and driver cache domains as the Mesa + Zink cell.
- Login, region settling and three completed teleports produced no Vulkan
  validation error, GHI device loss or integration failure.
- Normal logout drained and destroyed the Vulkan device after OpenGL cleanup
  began but before the viewer window was destroyed, followed by the normal
  `Goodbye!` termination marker.

Both Windows OpenGL-provider coexistence cells therefore satisfy the I0 exit
gate. Production rendering remained OpenGL in every test.
