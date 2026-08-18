# I7 production lighting and shadow integration checkpoint

I7 integrates the viewer's production deferred-lighting inputs and execution
with the native Vulkan peer. Visible rendering remains OpenGL until the whole
production frame is eligible; each I7 sub-gate uses only private, non-presenting
Vulkan resources.

## Sub-gates

### I7a — live lighting state and bounded Vulkan transfer

I7a observes the real main-view state after `setupHWLights()` and applies the
same production local-light eligibility rules used by
`LLPipeline::renderDeferredLighting()`:

- sun and moon direction, linear color, active state, and ambient color;
- view/projection matrices, camera origin, and source extent;
- nearby-light priority limit, attachment-light policy, minimum radius/color,
  and camera-frustum rejection;
- point-light position, rendered radius, linear color, and deferred falloff;
- projector rotation, scale, FOV/focus/ambiance, texture asset identity,
  selected shadow slot, and fade;
- directional cascade count, six shadow transforms, clip planes, directional
  bias, and projector shadow bias/offset; and
- separate scene and resource epochs.

The packet contains no viewer pointer, OpenGL name, Vulkan handle, capability
URL, credential, or GPU readback. OpenGL shadow and projector image pixels are
explicitly marked deferred. They are not read back, imported, or treated as
comparable native inputs.

`RenderVulkanLightingPacketProbe` enables the developer gate. The runtime
limits a sample to 256 local lights and 1 MiB, serializes it deterministically,
and copies it into backend-local Vulkan storage in one GHI frame. It records no
draw, creates no image/surface/swapchain, and cannot present.

### I7b — directional and point-light execution

Pair a same-frame I5/I6 private G-buffer with the I7 lighting packet and run
sun/moon plus bounded point lights into a private lighting attachment. Pairing
must happen before either packet is submitted so camera, resource, and scene
epochs match exactly. The OpenGL G-buffer is never sampled or copied.

Exit gate: directional and point-light output is nonzero, stable at steady
state, changes with real light motion/environment changes, and matches the
OpenGL semantic reference within the declared numeric tolerance.

### I7c — projector images and projector lighting

Observe projector decoded pixels at the existing decoder boundary, bind them
by content identity, reconstruct the production projector transform, and
execute both volume and camera-inside fullscreen cases.

Exit gate: projector cone, focus, ambiance, texture LOD policy, and priority
selection match without an OpenGL texture import or readback.

### I7d — native shadow production and sampling

Replay bounded shadow-caster geometry through native Vulkan into four
directional cascade maps and up to two projector maps, then consume those maps
in the I7 lighting pass. The packet's matrices, split policy, bias, and fade are
the semantic inputs; the OpenGL depth images are not inputs.

Exit gate: live directional and projector shadows survive camera motion,
altitude, rigged meshes, alpha masks, teleport hand-off, and resource churn
without acne, peter-panning, device loss, or steady-state device idle. The
Mesa-correct AMD body/attachment references remain the correctness target.

## Deterministic I7a gate

The GHI contract suite contains a deterministic two-light packet with one
point light, one shadowed projector, four directional cascades, and explicit
deferred-image comparability. It verifies exact round-trip, repeatable hash,
bounded native transfer, projector/cascade counts, invalid-radius rejection,
runtime-limit rejection, truncation rejection, and clean retirement.

The suite is **34/34 pass**. The shipping Release viewer builds successfully as
`build-vc170-64/newview/Release/vulkanstorm-bin.exe`. Enabling all legacy
viewer tests continues to expose the pre-existing `llurlmatch_test.cpp`
signature drift; the dedicated GHI contract target is unaffected.

## Live I7a exit gate

System OpenGL and Mesa + Zink must each produce stable initial lighting packets
and a post-teleport packet with correct scene/resource epoch behavior. At least
one scene must contain a local point light and one must contain a projector.
When shadows are enabled, the packet must report the configured cascade count
and explicit deferred-image comparability. Native transfer, normal logout, and
private-device retirement must complete without Vulkan validation or device-
loss errors.

The live cells passed on Windows with Vulkan validation enabled:

- System OpenGL produced stable empty-scene packets, then survived a teleport
  into a projector-rich scene. The settled packet held resource epoch 9 with
  33 local lights, 16 projectors, four shadow cascades, and a 4,420-byte
  backend-local upload.
- Mesa + Zink loaded from the staged private runtime and produced a stable
  initial packet with 41 local lights, 20 projectors, four shadow cascades,
  and a 5,316-byte upload. After teleport, the destination settled at resource
  epoch 4 with seven local lights, no projectors, four cascades, and a
  1,508-byte upload.
- Scene epochs advanced per accepted sample. Resource epochs changed only when
  the eligible light/resource set changed and stabilized after region load.
- Neither provider reported a Vulkan validation error or device loss. Both
  normal logout paths retired the private native Vulkan device and reached
  `Goodbye!`.

These results close I7a only. I7b through I7d remain required before the I7
production lighting and shadow objective can close.
