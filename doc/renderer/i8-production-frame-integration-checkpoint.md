# I8 production-frame integration checkpoint

I8 replaces the independent developer probes with one coherent, bounded
production-frame path. It remains private and non-presenting while the viewer's
visible world and UI continue to use the selected OpenGL provider.

## I8a — same-frame assembly and native transfer

I8a observes the material, terrain, and lighting seams already established by
I5, I6, and I7 during the same main-view deferred frame. It assembles them into
one versioned `ProductionFramePacket` with:

- a production frame identity and a monotonically increasing assembly epoch;
- source extent and an explicit material, terrain, shadow, deferred-lighting,
  and projector pass mask;
- the complete bounded material, terrain, and lighting child packets, retaining
  their independent scene and resource epochs; and
- a typed resource inventory covering geometry, draws, material and terrain
  resources, skins, projector images, and decoded texture bytes.

All child packets must carry the same production frame identity and source
extent. Material G-buffer, terrain G-buffer, and deferred lighting are mandatory
for this gate. Projector and shadow pass declarations are accepted only when the
corresponding lighting resources and shadow state exist. The packet contains no
viewer pointer, OpenGL object name, Vulkan handle, capability URL, credential,
or GPU readback.

`RenderVulkanFrameAssemblyProbe` enables the developer-only gate and requires
`RenderVulkanDeveloperProbe`. Earlier packet/offscreen probes should be disabled
for an I8a run; I8a takes capture precedence if settings are accidentally mixed.
The runtime accepts at most 256 material draws, 128 terrain draws, 262,144
vertices, 786,432 indices, 4,096 unique resources, 32 MiB of decoded texture
data, and a 64 MiB encoded frame.

The consumer deterministically serializes the entire frame, computes its
SHA-256 identity, and copies it to one backend-local buffer in a single GHI
frame. It creates no image, shader pipeline, surface, swapchain, window, or
presentation operation. It therefore proves whole-frame ownership and transfer
only; retained resource residency and frame-graph execution belong to later I8
sub-gates.

## Deterministic gate

The 36-test GHI contract suite constructs a combined material, terrain, and
lighting frame and verifies:

- typed geometry and resource accounting;
- exact deterministic encode/decode and SHA-256 identity;
- rejection of cross-frame components, invalid pass/resource dependencies, and
  truncated data;
- bounded transfer into backend-local storage and clean deferred retirement;
- format-correct sRGB mip filtering in the validation backend; and
- explicit shadow descriptor rebinding after every GHI pipeline selection.

All 36 tests pass. The Release viewer builds successfully as
`build-vc170-64/newview/Release/vulkanstorm-bin.exe`. Enabling every legacy
viewer test still exposes unrelated pre-existing test drift; the dedicated GHI
contract target is unaffected.

## Live exit gate

The live gate passed on Windows with native Vulkan validation enabled beside
both production OpenGL providers. Both runs used the same projected-lighting
and PBR-terrain scene, kept visible rendering on OpenGL, and completed five
same-frame samples before automatic logout.

- System OpenGL completed 5/5 samples. The settled frames contained 24 material
  draws, 64 terrain draws, 9,798 vertices, 37,806 indices, 49 unique resources,
  21 material textures, 11 terrain textures, one projector texture, and
  1,488,944 decoded bytes. All six pass bits were declared and the complete
  encoded frame occupied 2,207,656 bytes.
- Mesa + Zink was positively identified as Mesa 26.3 Zink and completed 5/5
  samples. Its settled frames contained 24 material draws, 64 terrain draws,
  9,694 vertices, 37,650 indices, 52 unique resources, 21 material textures, 11
  terrain textures, one projector texture, and 1,488,944 decoded bytes. All six
  pass bits were declared and the complete frame occupied 2,199,824 bytes.
- Every frame received its own SHA-256 identity and monotonically increasing
  assembly epoch. Capture was deliberately budget-limited while remaining
  within the transfer contract.
- Both private Vulkan devices retired normally and both processes reached
  `Goodbye!`. Neither run reported a Vulkan validation error, device loss,
  surface, swapchain, or presentation operation.

These results close I8a. They prove coherent same-frame assembly and bounded
native ownership transfer; they do not yet claim persistent resource residency
or execution of the assembled graph.

## I8b — retained decoded-texture residency

I8b replaces I8a's temporary monolithic destination buffer with a retained GHI
cache for immutable decoded images. Material, terrain, and projector textures
use domain-qualified logical source identities; native image allocations are
deduplicated by exact content identity, dimensions, and format. Sharing is
therefore safe across source domains without conflating different layouts or
linear/sRGB interpretation.

Each logical source has an explicit generation. Re-observing unchanged content
reuses the existing native image and generation. A new content identity for the
same source advances its generation and creates or reuses the matching native
content allocation. The lookup contract exposes a GHI image/view binding plus
format, extent, mip count, generation, and content identity for later I8 frame-
graph execution; no native Vulkan type crosses the seam.

Uploads are batched into one staging buffer and one GHI frame. Three-channel
input is expanded to RGBA, sRGB luminance/alpha input is expanded without losing
alpha semantics, and complete mip chains are generated natively. The default
cache is bounded to 1,024 allocations, 4,096 logical sources, 512 MiB resident,
and 32 MiB uploaded per accepted frame. Least-recently-used content is evicted
under pressure, while content unused for 120 assembly epochs is stale. Destroyed
GHI handles become invalid immediately and native allocations retire behind the
device's in-flight window.

Animated skin palettes, transient vertex/index streams, per-frame light state,
and render targets are deliberately excluded. They are mutable frame data and
would become stale if keyed as immutable assets. Geometry and shared target
ownership begin in I8c.

`RenderVulkanTextureResidencyProbe` enables the I8b developer gate and
supersedes the I8a transfer consumer while retaining the same coherent-frame
capture and validation rules. It creates no shader pipeline, render target,
surface, swapchain, window, or presentation operation.

The deterministic GHI suite remains **36/36 pass** and now verifies first-use
upload, unchanged-content cache reuse, logical generation advancement, bounded
LRU eviction, native-handle replacement, explicit cache shutdown, and deferred
native retirement.

### Live I8b exit gate

The live gate passed on Windows with Vulkan validation enabled beside both
production OpenGL providers.

- System OpenGL completed 5/5 coherent updates. The first frame uploaded 33
  unique images totaling 1,726,528 bytes. Four legitimate content changes on
  the second loading frame advanced four logical-source generations and grew
  the retained set to 37 images / 1,955,904 bytes. Frames three through five
  were 33/33 cache hits with zero uploads and stable residency.
- Mesa + Zink was positively identified as Mesa 26.3 Zink and completed 5/5.
  The first frame requested 30 logical sources that deduplicated to 29 exact
  contents totaling 1,712,192 bytes. Frames two through five were 29/29 cache
  hits with zero uploads, no generation change, and stable residency. This
  directly exercises safe cross-source content sharing.
- Both runs kept visible world/UI rendering on OpenGL, explicitly retired the
  retained cache, shut down the private Vulkan device, and reached `Goodbye!`.
  Neither reported a Vulkan validation error, device loss, surface, swapchain,
  or presentation operation.

These results close I8b. Later frame-graph work may resolve the retained GHI
bindings by logical source and generation; it does not need to recreate or
reupload unchanged decoded assets.

## I8c1 — shared private frame-target ownership

I8c1 establishes the attachment topology that the executable production frame
graph will use. One GHI owner allocates four G-buffer images, a depth image, an
HDR lighting image, and the directional/projector shadow images declared by the
coherent frame pass mask. It exposes only typed GHI image and view handles;
backend-native objects remain confined to the Vulkan implementation.

The private extent preserves the production camera aspect ratio while remaining
bounded to 512 by 512 pixels, 262,144 pixels, and 64 MiB by default. An exact
extent/pass topology match reuses every allocation and retains its generation.
A resize or shadow-topology change allocates a complete replacement first,
advances the generation, then retires the old views and images through the
device's deferred-destruction contract. Allocation failure cannot partially
replace the active set.

`RenderVulkanFrameGraphProbe` enables this developer-only gate and supersedes
the I8b and I8a gates. It retains the I8b immutable texture cache and coherent
I8a frame capture, but records no render pass, draw, surface, swapchain, window,
or presentation command. Visible world and UI rendering therefore remain on
the selected OpenGL provider.

The deterministic GHI suite remains **36/36 pass** and verifies initial target
allocation, exact-topology reuse, bounded resize, generation advancement,
explicit shutdown, and deferred native retirement. The Release Vulkanstorm
viewer builds successfully and the render-API boundary ratchet reports no new
OpenGL or Vulkan coupling above the backend seam.

### Live I8c1 exit gate

The isolated live gate passed on Windows with Vulkan validation enabled beside
both production OpenGL providers. Every older Vulkan packet/offscreen probe was
explicitly disabled for these runs.

- System OpenGL completed 5/5 coherent updates. The first frame allocated 12
  images at 512 by 270 pixels totaling 8,294,400 bytes and retained target
  generation 1. The first update uploaded 33 resident images / 1,941,568 bytes;
  updates two through five reused all 33 images with zero upload and reused the
  complete target topology.
- Mesa + Zink was positively identified as Mesa 26.3 Zink and completed 5/5
  coherent updates. It produced the same 12-image, 512 by 270, 8,294,400-byte
  target set, stable generation, 33 resident images, and four exact target and
  texture-residency reuses. Its lower visible OpenGL frame cadence only spread
  the 600-frame samples farther apart.
- Both runs explicitly retired the shared targets and retained cache, shut down
  the private Vulkan device, and reached `Goodbye!`. Neither reported a Vulkan
  validation error, device loss, legacy probe execution, surface, swapchain, or
  presentation operation.

These results close I8c1. They prove production-frame attachment ownership and
lifetime, not native rendering into those images; material and terrain G-buffer
execution begins in I8c2.

## Remaining I8 work

After I8a, the assembled packet becomes the sole input to a private native
frame graph in incremental gates:

1. execute material and terrain G-buffer passes into shared private targets;
2. execute native shadow, deferred-lighting, and projector passes from the same
   graph and frame identity;
3. add sky, water, alpha, recursive/offscreen views, HUD/UI composition, and
   picking in separately testable slices; and
4. permit selectable visible Vulkan presentation only after parity, recovery,
   and provider-matrix gates are satisfied.

The architecture remains additive: native OpenGL and Mesa + Zink continue to be
usable OpenGL providers throughout I8, and no production route is retired by
this checkpoint.
