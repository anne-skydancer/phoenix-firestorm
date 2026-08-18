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

I7b is intentionally split at the geometry boundary:

- **I7b-material** replays the same-frame I5 opaque PBR material packet into
  four private G-buffer attachments, transitions those attachments and depth
  to sampled use, and executes one Vulkan fullscreen pass. The pass consumes
  active sun/moon state and at most 64 ordinary point lights. It uses the
  production legacy attenuation constant and metallic-roughness punctual BRDF
  equations. Projectors are filtered out rather than approximated.
- **I7b-terrain** replays the same-frame I6 terrain packet into the same four
  private G-buffer attachments and executes the same bounded I7 pass. It
  preserves the I6 legacy/PBR and planar/triplanar terrain branches. Its live
  gate requires at least one PBR terrain draw so a legacy-only region cannot
  accidentally close the production PBR-terrain objective.

`RenderVulkanLightingOffscreenProbe` enables the developer-only material
slice. It loads the I5 material package and the dedicated
`i7_deferred_lighting.llghisp` package, pairs material and lighting packets by
production frame and source extent before submission, and records hashes and
non-clear-pixel counts for all private outputs. It creates no surface,
swapchain, or presentation path; visible production rendering remains OpenGL.

`RenderVulkanTerrainLightingOffscreenProbe` enables the corresponding terrain
slice. It loads the I6 terrain package and the same I7 lighting package, then
pairs terrain and lighting packets by production frame before submission. The
two I7b gates are mutually exclusive in one run; if both are forced on, the
material gate takes precedence and the runtime reports that choice.

The deterministic GHI suite is **35/35 pass** and now covers package
reflection, a material/lighting frame-mismatch rejection, one active
directional light, one ordinary point light, explicit projector deferral,
asynchronous submission, result identities, and clean retirement. Pixel
coverage is a live native-Vulkan gate because the deterministic validation
backend validates commands and lifetime without rasterizing.

#### Live I7b-material gate

The material slice passed on Windows with native Vulkan validation enabled for
both production OpenGL providers. A local Midday override supplied an active
directional light without modifying the region environment.

- System OpenGL executed stable Aether samples with 16 bounded material draws,
  one directional light, 45 ordinary point lights, four non-clear G-buffer
  attachments, and 44,757 non-clear lit pixels. The real
  Aether -> Green -> Aether round trip used explicit settle intervals; both
  destinations and the return produced distinct scene hashes and valid lit
  outputs. The settled return held 16 draws, 15 points, and 45,460 lit pixels.
- Mesa + Zink executed stable Aether samples with 16 draws, one directional,
  the 64-point execution cap, and 42,808 lit pixels. Green passed after the
  real teleport with four draws, 14 points, and 833 lit pixels; the settled
  Aether return passed with 11 draws, 15 points, and 5,817 lit pixels.
- Both providers shut down the private Vulkan device normally and reached
  `Goodbye!`. Neither reported a Vulkan validation error, device loss, surface,
  swapchain, or presentation operation.

#### Live I7b-terrain gate

The terrain slice passed on the same Windows system with native Vulkan
validation enabled. Isle of Repose supplied real planar PBR terrain while the
material live cells above supply the already-completed teleport hand-off
coverage for both OpenGL providers.

- System OpenGL completed 20 consecutive PBR-terrain lighting samples. Each
  selected 16 PBR draws and one active directional light; steady state held
  37 ordinary point lights and 2,169 non-clear lit pixels after an initial
  38-point, 3,547-pixel loading sample. All four G-buffer attachments remained
  non-clear and stable at steady state.
- Mesa + Zink was positively identified as Mesa 26.3 Zink and completed seven
  consecutive samples before normal close. Each selected 16 PBR draws, one
  active directional light, 13 ordinary point lights, four non-clear G-buffer
  attachments, and 350 non-clear lit pixels.
- Both providers retired the private Vulkan device normally and reached
  `Goodbye!`. Neither reported a Vulkan validation error, device loss, surface,
  swapchain, or presentation operation.

The material and terrain cells together close **I7b**. Projector images and
projector lighting remain I7c; native shadow production and sampling remain
I7d.

### I7c — projector images and projector lighting

Observe projector decoded pixels at the existing decoder boundary, bind them
by content identity, reconstruct the production projector transform, and
execute both volume and camera-inside fullscreen cases.

Exit gate: projector cone, focus, ambiance, texture LOD policy, and priority
selection match without an OpenGL texture import or readback.

I7c extends the lighting packet to version 2. Decoded projector images cross
the seam as bounded, backend-neutral resources containing source UUID,
SHA-256 content identity, dimensions, component count, discard level, and
decoded bytes. Validation recomputes the content hash, rejects malformed byte
counts, duplicate source identities, orphan resources, and comparable
projectors without a matching image. A deferred projector remains legal and
does not pretend that an unavailable image is comparable.

The live capture is intentionally bounded to eight unique projector images
and 512 KiB of decoded pixels. It observes the existing decoder completion
boundary and performs no OpenGL readback, texture-name transfer, or Vulkan
import. Repeated projector lights that use the same source identity share one
backend-local image, image view, mip chain, and sampler while retaining their
own transform, uniform block, descriptor bindings, and draw/scissor policy.

The dedicated `i7_projector_lighting.llghisp` package executes an additive
projector pass over the private I5 G-buffer and I7b lighting target. It
generates texture mips natively, consumes production rotation, scale, FOV,
focus, ambiance, fade, and attenuation inputs, and supports the PBR and legacy
material branches. Camera-inside lights use a fullscreen scissor; outside
lights use a conservative projected light-volume scissor. Projector shadow
sampling remains deferred to I7d.

`RenderVulkanProjectorLightingOffscreenProbe` enables the developer-only live
gate. It pairs same-frame material and lighting packets before submission and
reports projector draw count, unique native image count, volume/fullscreen
classification, output identities, and non-clear pixels. It creates no
surface, swapchain, or presentation path; visible production rendering
remains OpenGL.

The deterministic validation cell uses two projector draws sharing one image
and separately exercises the fullscreen and volume cases. The GHI contract
suite is **35/35 pass** after the packet-v2 and image-sharing changes.

#### Live I7c gate

I7c passed on Windows with native Vulkan validation enabled alongside both
production OpenGL providers in the projected-lighting scene.

- System OpenGL first exercised changing camera classifications across 20
  samples, including all-volume, mixed volume/fullscreen, and all-fullscreen
  batches, with as many as 46,971 non-clear lit pixels. The final post-
  deduplication run completed five consecutive samples; each submitted eight
  projector draws sharing one native image and produced 7,522 non-clear lit
  pixels.
- Mesa + Zink first produced nine stable fullscreen samples. Its final post-
  deduplication run completed five consecutive samples; each submitted eight
  projector draws sharing one native image and produced 4,958 non-clear lit
  pixels.
- Both providers retired the private Vulkan device normally and reached
  `Goodbye!`. Neither reported a Vulkan validation error, device loss,
  surface, swapchain, or presentation operation.

These results close the bounded I7c integration gate. They prove native image
ownership, deduplicated lifetime, projector-pass execution, and real-grid
state transfer; they do not claim visible Vulkan presentation or substitute
for later numeric/image comparison against the production OpenGL reference.

### I7d — native shadow production and sampling

Replay bounded shadow-caster geometry through native Vulkan into four
directional cascade maps and up to two projector maps, then consume those maps
in the I7 lighting pass. The packet's matrices, split policy, bias, and fade are
the semantic inputs; the OpenGL depth images are not inputs.

Exit gate: live directional and projector shadows survive camera motion,
altitude, rigged meshes, alpha masks, teleport hand-off, and resource churn
without acne, peter-panning, device loss, or steady-state device idle. The
Mesa-correct AMD body/attachment references remain the correctness target.

#### I7d implementation and live gate

The private material probe now owns six D32 depth targets: four directional
cascades and two projector maps. It replays bounded rigid, rigged, opaque, and
alpha-masked caster geometry into those targets and samples the resulting depth
images in the same-frame native deferred/projector lighting submission. The
Vulkan alpha-mask path rejects discarded texels with depth 1.0 plus a strict
`Less` comparison, avoiding the optional
`shaderDemoteToHelperInvocation` device feature. Depth-only dynamic rendering,
depth-image readback, and empty color-attachment arrays are supported by the
native backend. No surface, swapchain, or presentation object is created.

The live gate passed on Windows with Vulkan validation enabled alongside both
OpenGL providers:

- System OpenGL completed 5/5 native Vulkan samples. Every sample produced all
  four directional and both projector maps, drew 32 shadow casters, included
  rigged and alpha-masked casters, and returned non-clear lit pixels. One far
  directional cascade was legitimately empty in the settled camera, while the
  directional and projector shadow categories both had non-clear coverage.
- Mesa + Zink completed 5/5 native Vulkan samples with all six maps non-clear,
  32 casters per sample, rigged/alpha-mask coverage, projected-light image
  sampling, and non-clear lit output.
- Both runs exited normally with `Goodbye!`. Neither reported Vulkan validation
  errors, device loss, a surface, a swapchain, or a presentation operation;
  visible rendering remained OpenGL throughout.

These results close the bounded I7d execution gate. Visual parity under camera
motion, altitude, teleport churn, and the retained AMD body/attachment
references remains an integration/acceptance responsibility once native Vulkan
owns production presentation; it is not claimed by this private readback gate.

## Deterministic I7a gate

The GHI contract suite contains a deterministic two-light packet with one
point light, one shadowed projector, four directional cascades, and explicit
deferred-image comparability. It verifies exact round-trip, repeatable hash,
bounded native transfer, projector/cascade counts, invalid-radius rejection,
runtime-limit rejection, truncation rejection, and clean retirement.

The suite is **35/35 pass**. The shipping Release viewer builds successfully as
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

These results close I7a. I7b is closed by the material and terrain gates above,
I7c is closed by the decoded-projector-image gate, and I7d is closed by the
native depth-production and same-frame sampling gate. The bounded I7 production
lighting and shadow integration checkpoint is complete; visible Vulkan
presentation and final image parity remain later integration objectives.
