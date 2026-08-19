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

## I8c2 — shared-target material and terrain execution

I8c2 consumes the same coherent production frame after I8b residency and I8c1
target validation. One bounded GHI submission uploads transient material and
terrain geometry, indices, transforms, skin palettes, and uniform state. It
resolves immutable material and terrain images from the retained cache by
domain-qualified source identity; absent optional material maps use four
persistent semantic fallbacks, while draws with unresolved required resources
are explicitly deferred rather than rendered with stale data.

The existing Vulkan GLSL `r5_material_skin` and `i6_terrain` packages execute in
one shared render scope. Four G-buffer targets and reverse-Z depth are cleared
once, then rigid/rigged opaque PBR material draws and production terrain draws
write the same attachment set. The submission is bounded to 256 material draws,
128 terrain draws, and 64 MiB of transient upload data. All native resource and
descriptor lifetimes remain behind GHI.

Verification copies the four private attachments to executor-owned readback
buffers without waiting. A later OpenGL frame polls completion and records
SHA-256 identities and non-clear coverage. The validation backend checks both
draw streams, descriptors, target compatibility, readback completion, and clean
retirement but deliberately does not software-rasterize indexed draws; the live
native Vulkan gate therefore owns the stronger pixel-coverage criterion.

`RenderVulkanGBufferExecutionProbe` enables I8c2 and supersedes the earlier I8
developer gates. Lighting, shadows, sky, water, alpha, recursive/offscreen
views, HUD/UI, picking, surfaces, swapchains, and presentation remain excluded.
Visible production world and UI rendering remain on the selected OpenGL
provider.

The deterministic GHI suite remains **36/36 pass**. The isolated Windows live
gate also passed with Vulkan validation enabled beside both production OpenGL
providers. Every older Vulkan packet and offscreen probe was explicitly
disabled, visible rendering remained OpenGL, and both viewers shut down cleanly
without validation errors or device loss.

- System OpenGL first completed 5/5 general-scene samples, then completed a
  separate 6/6 rigged-PBR coverage run after the attachment was added. Every
  coverage sample executed two material draws, both rigged, together with 39
  to 64 PBR-terrain draws and no deferred work. All four G-buffer attachments
  contained rendered pixels; settled samples reached approximately 76,500
  non-clear pixels per attachment and were not capture-budget-limited. The
  first coverage sample uploaded 17 immutable images / 1,052,672 bytes;
  subsequent samples reused all 17 with zero image uploads.
- Mesa + Zink was positively identified as Mesa 26.3 Zink and completed 6/6
  samples after a rigged PBR attachment was added. Every sample executed two
  material draws, both rigged, together with 45 to 64 PBR-terrain draws and no
  deferred work. All four attachments contained rendered pixels; settled
  samples reached approximately 75,700 non-clear pixels per attachment and
  were not capture-budget-limited. The first sample uploaded 17 immutable
  images / 1,052,672 bytes; subsequent samples reused all 17 with zero image
  uploads.

These results close I8c2. They prove that rigid and rigged opaque-PBR material
and PBR-terrain draw streams can share the production frame's persistent native
G-buffer and depth targets. They do not yet prove lighting, shadows, surface
ownership, presentation, or visible Vulkan parity.

## I8c3 — shared-target shadow and lighting execution

I8c3 appends a second ordered GHI submission to the I8c2 G-buffer submission
without changing frame identity or attachment ownership. The executor renders
bounded opaque and alpha-masked material casters, including rigged casters,
into the I8c1 directional and projector shadow targets. It then samples the
shared G-buffer, reverse-Z depth, retained projector images, and native shadow
maps to execute deferred and additive projector lighting into the shared
lighting target. It creates no private duplicate G-buffer, depth, shadow, or
lighting images.

Directional and projector shadow images retain fixed semantic array slots.
Inactive shader bindings use a persistent cleared fallback depth image, so a
projector-only or partially populated shadow configuration never aliases a
directional slot or exposes an undefined sampled descriptor. Immutable
alpha-mask and projector textures resolve through I8b residency; missing
required images defer the affected work instead of importing OpenGL objects.
Geometry and uniform uploads remain bounded and transient.

`RenderVulkanLightingExecutionProbe` enables I8c3 and supersedes the earlier
I8 developer gates. Runtime acceptance requires the I8c2 and I8c3 SHA-256 frame
identities to match, a populated lighting target, at least one non-clear map in
the directional-shadow category, and at least one non-clear map in the
projector-shadow category. It deliberately does not require every cascade to
contain a caster because a valid camera and scene can leave individual maps
clear.

The deterministic GHI suite remains **36/36 pass**. Its combined production
frame executes four directional cascades, one projector shadow, three bounded
material casters including two rigged and one alpha-masked draw, one projector
light, and a shared lighting readback. The validation backend verifies target
semantics, resource bindings, ordered draw streams, frame identity, and clean
retirement; live native Vulkan owns pixel-coverage verification.

The isolated System OpenGL live gate completed 6/6 samples with Vulkan
validation enabled. All samples executed one directional light, 36 point
lights, eight projector lights, four directional maps, two projector maps, and
non-clear pixels in both shadow categories and the lighting target. The first
loading sample replayed 24 material casters. Settled samples replayed two
casters, both rigged, and produced approximately 6,600 lit pixels. Individual
clear cascades were accepted only because the directional category itself had
real coverage. Visible world and UI rendering remained System OpenGL; no
surface, swapchain, or presentation object was created.

Mesa 26.3 + Zink was positively identified and completed the same 6/6 gate.
Every sample executed one directional light, 36 point lights, eight projector
lights, all six shadow maps, and two material casters, both rigged. Lighting
coverage remained approximately 6,600 pixels; directional and projector-shadow
categories both contained rendered depth. All deferred caster counts remained
zero. Visible world and UI rendering remained Mesa + Zink OpenGL and the native
Vulkan target graph remained private.

## Remaining I8 work

After I8a, the assembled packet becomes the sole input to a private native
frame graph in incremental gates:

1. add sky and water execution to the shared private frame graph;
2. add alpha and recursive/offscreen views in separately testable slices;
3. complete UI0-UI6, including HUD/UI composition, snapshots, and picking;
4. run the mandatory legacy-`gGL`-to-GHI convergence track across those
   slices, then close the remaining production coupling inventory; and
5. permit selectable visible Vulkan presentation only after parity, recovery,
   and provider-matrix gates are satisfied.

The GHI convergence gate is P0 and blocks production selector approval. A
Vulkan session may not depend on ambient `gGL` state or a viewer-owned OpenGL
context; residual OpenGL implementation belongs inside the OpenGL peer, not in
shared renderer-facing code.

The architecture remains additive: native OpenGL and Mesa + Zink continue to be
usable OpenGL providers throughout I8, and no production route is retired by
this checkpoint.
