# Vulkan Production Readiness

## Executive status

The Vulkan GHI is a substantial pre-production backend, not yet a selectable
production renderer. It can create a Vulkan 1.3 device, compile and load SPIR-V
packages, allocate resources, record draws and transfers, execute several
production-derived scene slices offscreen, and exercise a Win32 swapchain in
lifecycle harnesses.

Visible viewer rendering remains owned by OpenGL. Vulkan is initialized as a
developer-gated sidecar after OpenGL, and its presentation implementation is not
connected to the production frame graph. Native Vulkan therefore cannot yet be
offered as a first-order peer to OpenGL.

## Current architecture

### Implemented

- Backend-neutral resource, pipeline, descriptor, render-scope, transfer,
  query, and command contracts in `indra/llrender/ghi`.
- OpenGL, validation, and Vulkan device factories.
- Vulkan 1.3 instance and device creation, graphics queues, dynamic rendering,
  frame fences, deferred destruction, barriers, descriptors, MRT, blending,
  stencil, instancing, indexed drawing, mip generation, readback, timestamp
  queries, and occlusion queries.
- Deterministic offline GLSL-to-SPIR-V packaging with reflection and content
  validation.
- Win32 swapchain creation, acquire/present, resize, minimize, vsync modes, and
  out-of-date recovery in the presentation harness.
- Private offscreen executors for production-derived opaque, rigged, terrain,
  environment, deferred-lighting, shadow, water, and alpha workloads.
- Contract and native lifecycle/resource/draw/replay harnesses.

### Partial

- Vulkan is Windows-only and disabled by default.
- The resource device and presentation device have separate ownership.
- Device loss is detected but does not recreate the Vulkan renderer.
- Synchronization is functional but uses coarse queues and idle waits in key
  lifecycle paths.
- Pipeline identities exist without persistent `VkPipelineCache` storage.
- Texture formats cover a limited uncompressed set.
- Compute metadata exists, but compute pipelines and dispatch are unsupported.
- Push-constant metadata exists without a command API.
- Renderer telemetry and production eligibility policy exist only in pieces.

### Missing from the production Vulkan path

- A native Vulkan renderer choice and restart/fallback behavior.
- Production swapchain presentation from GHI frame targets.
- Complete visible postprocessing and final compositing.
- UI, HUD, fonts, media surfaces, picking, and screenshots.
- Recursive views: mirrors, probes, cube snapshots, impostors, and previews.
- Appearance compositing and avatar baking.
- Linux WSI and backend qualification.
- Automated native Vulkan hardware CI and release qualification.

## Feature readiness matrix

| Area | Status | Production gap |
| --- | --- | --- |
| Device and resources | Partial | Unified ownership, memory budgets, recovery |
| Shader packages | Partial | Compute, push constants, broader parity |
| Opaque/PBR geometry | Partial | Visible production routing and parity |
| Rigged avatars | Partial | Visible routing, appearance and bake dependencies |
| Terrain | Partial | Visible routing and regression qualification |
| Deferred lighting | Partial | Production frame integration and parity |
| Shadows | Partial | Production frame integration and recursive cases |
| Sky/environment | Partial | Live resource ownership and parity |
| Water | Partial | Reflection, refraction, exclusions, visible output |
| Alpha/OIT/particles | Partial | Live routing, HUD/media phases, blend coverage |
| Postprocessing | Missing | Tone map, exposure, glow, SSR, AA, DoF, composite |
| UI/HUD/fonts/media | Missing | Backend-neutral rendering and texture interop |
| Picking/screenshots | Missing | Backend-neutral readback and synchronization |
| Recursive views | Missing | GHI ownership and nested frame scheduling |
| Appearance/baking | Missing | Backend-neutral texture-layer compositor |
| Presentation | Partial | Connection to the production GHI device/frame |
| User selection | Missing | Settings, restart, fallback, support policy |
| Linux | Missing | Surface creation, WSI, packaging, qualification |
| CI and telemetry | Partial | Hardware matrix, metrics, gates, soak tests |

## Production plan

### Phase 0: Baseline and contracts

1. Freeze representative OpenGL captures and performance baselines for scenes
   covering avatars, terrain, water, alpha, shadows, UI, media, and recursive
   views.
2. Define backend-neutral frame, surface, presentation, readback, and device-loss
   contracts. Include ownership and shutdown ordering.
3. Turn the GL-coupling inventory into a checked budget that cannot regress.
4. Register all deterministic GHI contract tests with CTest.

Exit gate: reproducible image, correctness, CPU/GPU time, memory, and coupling
baselines exist on supported hardware.

### Phase 1: Unified renderer lifecycle

1. Replace the Vulkan sidecar with one renderer-selected GHI device lifecycle.
2. Connect Vulkan presentation to the same physical/logical device and queues
   used for rendering.
3. Route OpenGL through the production GHI lifecycle first to preserve behavior
   while ownership changes.
4. Add backend-neutral resize, minimize, fullscreen, vsync, shutdown, and frame
   pacing behavior.
5. Add validation callbacks, structured diagnostics, adapter selection, and
   renderer identity telemetry.

Exit gate: OpenGL and Vulkan can each acquire, render, and present a controlled
production frame through the same lifecycle without parallel renderer devices.

### Phase 2: Visible world foundation

1. Route opaque static and rigged PBR geometry through GHI.
2. Route terrain, sky/environment, deferred lighting, and shadows.
3. Replace direct GL texture, framebuffer, uniform, and vertex-buffer ownership
   used by these stages with GHI resources and descriptors.
4. Add compressed formats, explicit resolves/blits, push constants where
   justified, and bounded asynchronous upload/readback scheduling.
5. Validate visual parity with captures and GPU-assisted validation enabled.

Exit gate: a representative world scene renders visibly through Vulkan with no
OpenGL rendering commands in migrated stages and meets agreed image tolerances.

### Phase 3: Transparency and environment completeness

1. Connect production reflection, refraction, and water-exclusion resources.
2. Qualify legacy alpha, PPLL, and depth-peel paths with live scene data.
3. Migrate particles, impostor alpha, custom blend modes, materials, emissive,
   and mask variants.
4. Define deterministic ordering and synchronization for transparent passes.

Exit gate: water, particles, alpha avatars, vegetation, and mixed transparent
content pass parity, artifact, and sustained-load tests.

### Phase 4: Frame completion

1. Migrate tone mapping, exposure, glow, SSR, anti-aliasing, depth of field,
   color correction, and final compositing.
2. Migrate recursive views, reflection probes, cube snapshots, impostors,
   previews, and mirrors using explicit nested-frame scheduling.
3. Add backend-neutral picking, screenshot, and asynchronous readback paths.

Exit gate: Vulkan owns every world-rendering stage from scene submission to the
presentable image, including recursive and readback workloads.

### Phase 5: UI, media, and appearance

1. Migrate UI geometry, clipping, fonts, icons, HUDs, and cursors to GHI.
2. Define media texture upload/interoperation and color-space behavior.
3. Migrate appearance texture compositing and avatar baking.
4. Remove OpenGL context requirements from viewer and UI initialization when
   Vulkan is selected.

Exit gate: login, preferences, inventory, chat, media, HUDs, avatar editing, and
appearance baking run without an OpenGL context in native Vulkan mode.

### Phase 6: Reliability and performance

1. Implement Vulkan device-loss teardown and reconstruction with a safe fallback
   to OpenGL after restart when recovery is impossible.
2. Persist pipeline caches with device, driver, shader, and build identities.
3. Add memory-budget monitoring, descriptor/pipeline churn metrics, frame-stage
   GPU timestamps, queue-stall telemetry, and bounded resource retirement.
4. Reduce coarse `vkDeviceWaitIdle` use and qualify frame pacing under resize,
   occlusion, teleport, and heavy streaming.
5. Add Linux surface/presentation support if Linux remains a release target.

Exit gate: crash-free soak, teleport, resize, minimize, suspend, driver-reset,
and memory-pressure tests pass within defined performance budgets.

### Phase 7: Qualification and product selection

1. Add native Vulkan CI on supported Windows and Linux vendor/driver matrices.
2. Run golden-image comparisons and performance regression checks on content-
   heavy scenes.
3. Persist production-eligibility evidence by adapter, driver, and feature set.
4. Add a restart-required `OpenGL | Vulkan` selector, with `Auto` selecting only
   qualified configurations.
5. Provide startup failure detection, actionable diagnostics, safe-mode override,
   and automatic fallback to OpenGL on the next launch.
6. Roll out behind release channels and telemetry gates before general default
   eligibility.

Exit gate: Vulkan passes the supported feature matrix, hardware qualification,
stability targets, and performance budgets and is supportable as a peer backend.

## Non-negotiable release criteria

- Vulkan can start and render without creating an OpenGL context.
- No known feature silently falls back to OpenGL inside a Vulkan session.
- All visible frame stages, UI, media, readback, and appearance workflows pass.
- Device loss and startup failure have tested recovery or fallback behavior.
- Validation layers report no errors in the qualification suite.
- GPU memory remains bounded during region changes and long sessions.
- Frame pacing and representative GPU performance are not materially worse than
  the agreed OpenGL baseline on supported hardware.
- Native Vulkan tests are mandatory in CI and release qualification.

## Primary evidence locations

- `indra/llrender/ghi/include` and `indra/llrender/ghi/core`
- `indra/llrender/ghi/backends/vulkan`
- `indra/newview/llghiruntime.cpp`
- `indra/newview/pipeline.cpp` and `indra/newview/pipeline.h`
- `indra/newview/llviewerwindow.cpp`
- `indra/newview/app_settings/settings.xml`
- `indra/newview/skins/default/xui/en/panel_preferences_graphics1.xml`
- `doc/renderer/gl-coupling-baseline.json`
- `doc/renderer/p0-ghi-state-convergence.md`
- `doc/renderer/ui-integration-plan.md`