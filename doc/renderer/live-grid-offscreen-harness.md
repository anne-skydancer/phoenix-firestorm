# Live-grid offscreen renderer verification harness

Date: 2026-08-16  
Status: accepted architecture; implementation follows the production GHI seam

## Objective

Verify Native OpenGL, Mesa+Zink, and the peer Vulkan backend against the same
live Second Life scene data without presenting rendered frames. The harness
must exercise real login, simulator/capabilities traffic, interest-list
updates, asset delivery and decoding, scene construction, shader selection,
and rendering.

It is not the existing `HeadlessClient` mode. That mode suppresses rendering;
this harness suppresses presentation while preserving rendering.

## Process architecture

```text
Second Life grid
       |
       v
Live ingest/controller
  login + capabilities + UDP/event queues
  object/avatar/terrain/environment state
  asset fetch + Grok J2C + mesh/material decode
  deterministic camera + frame/epoch coordinator
       |
       | versioned backend-neutral resource and frame packets
       +-------------------+-------------------+
       v                   v                   v
Native OpenGL worker   Mesa+Zink worker    Vulkan worker
hidden WGL context     Mesa opengl32/Zink  offscreen device
offscreen targets      hidden WGL context  no surface/swapchain
       |                   |                   |
       +-------------------+-------------------+
                           v
                  Comparator/telemetry
             hashes + images + timings + errors
```

Separate renderer workers are mandatory on Windows because Native OpenGL and
Mesa+Zink select different `opengl32.dll` implementations at process load.
They also isolate driver crashes and make backend-specific validation and
profiling possible without contaminating the controller.

## Frame protocol

Every accepted frame has:

- session, region, camera, scene-epoch, resource-epoch, and monotonically
  increasing frame identifiers;
- immutable resource create/update/retire messages identified by semantic
  handles and content hashes;
- renderer settings and feature/capability decisions used to build the frame;
- one backend-neutral GHI command stream plus shader semantic/toolchain hashes;
- an explicit completeness state: warming, comparable, invalidated, or timed
  out.

Workers acknowledge the highest complete resource epoch before rendering a
frame. The controller compares only outputs produced from the same frame and
resource epoch. Teleports, region crossings, shader rebuilds, and large asset
bursts open a new warming epoch; they never silently compare incomplete output.

## Output and comparison

The minimum output set is final linear/HDR color before presentation, depth,
and an object/material ID diagnostic attachment. Optional pass outputs can be
enabled when reducing a failure.

- Hash exact integer/ID outputs and operations explicitly defined as bit-exact.
- Preserve raw images for mismatches and record per-tile/max error for bounded
  floating-point comparisons.
- Read back asynchronously at a configurable cadence. Normal frames may use
  GPU-side reduction hashes; full images are collected periodically and on the
  first mismatch.
- Record CPU frame-build time, GPU pass timestamps, upload volume, pipeline
  cache state, resource residency, shader identities, GL errors, Vulkan
  validation messages, device loss, and worker heartbeat.

No output log may contain account credentials, session tokens, capability URLs,
private chat, inventory names, or unrestricted raw network payloads.

## Reliability and control

- A controller watchdog restarts a failed worker without logging the ingest
  agent out and marks the affected interval incomparable.
- Backpressure is explicit: the controller may drop superseded non-comparable
  frames, but never mutate an accepted frame packet.
- Login and teleport settling are state-based: region handshake, capabilities,
  scene/resource epoch stability, and worker acknowledgements. Fixed sleeps are
  only timeouts, not readiness criteria.
- A test run declares its region, camera path, graphics profile, duration,
  comparison cadence, tolerated incompleteness, and expected feature paths.

## Delivery order

1. Keep the R3 synthetic fixture as the fast backend contract gate.
2. Define a serializable GHI resource/command packet after the GHI seam is
   stable enough to avoid encoding viewer globals.
3. Add presentation-suppressed OpenGL and Vulkan worker executables.
4. Add the live ingest/controller and deterministic camera/epoch coordinator.
5. Add Native OpenGL versus Mesa+Zink comparison first.
6. Add Vulkan as its implemented feature ledger reaches each comparable pass.
7. Add GPU-side hashes, mismatch capture, performance telemetry, and CI/lab
   orchestration.

The design should be revisited once the renderer can express mirrors, hero
probes, cube snapshots, HUDs, impostors, pre-water alpha, and alpha methods
through explicit pass metadata. Until then, those paths must be reported as
uncomparable rather than approximated.
