# P0 GHI state convergence

P0 replaces renderer-facing ambient `gGL` and `LLGLState` ownership with
explicit GHI pipeline, dynamic-state, binding, pass, and draw descriptions.
Valid OpenGL state calls remain implementation details of the native OpenGL
peer. P0 does not replace or deprecate OpenGL, retire Mesa + Zink, or expose
Vulkan as a production backend.

## Ordered checkpoints

1. **P0a — reachability and semantic ledger.** Classify every legacy state
   surface above the backend seam, then add opt-in runtime evidence before any
   dormant candidate is removed.
2. **P0b — dead-code sanitation.** Remove only proven unreachable code in
   independently reversible changes. Debug-only validation remains until the
   GHI validator supersedes it.
3. **P0c — state-contract completion.** Close demonstrated gaps in the existing
   `PipelineDesc` and command contracts. Do not create a parallel state model.
4. **P0d — OpenGL 4.1 state compiler.** Make the OpenGL GHI peer the single
   authoritative native-state cache. Optional newer entry points are
   capability-gated optimizations, not requirements.
5. **P0e — production migration.** Migrate production feature families in
   bounded slices, preserving the visible OpenGL path until each slice passes
   its deterministic and live gates.
6. **P0f — legacy confinement.** Retire migrated `gGL`/`LLGLState` ownership and
   confine residual native OpenGL implementation below the backend boundary.
7. **P0g — qualification.** Require semantic, visual, lifecycle, performance,
   provider-matrix, and coupling acceptance before selector approval.

## P0a static ledger

`scripts/code_tools/audit_legacy_gl_state.py` generates the reviewed static
ledger at `doc/renderer/p0-legacy-gl-state-ledger.json`. It inventories code in
`llappearance`, `llrender`, `llui`, `llwindow`, and `newview`, excluding only
the native OpenGL and Vulkan backend implementation directories.

The ledger groups each source occurrence by symbol, semantic classification,
disposition, subsystem owner, and file. Its direct-GL scan deliberately records
viewer-owned `gl*`-shaped functions as false positives rather than silently
mistaking them for native entry points.

Run:

```text
python scripts/code_tools/audit_legacy_gl_state.py --check
```

Regeneration is an architecture-review action:

```text
python scripts/code_tools/audit_legacy_gl_state.py --write-ledger
```

The existing API-boundary ratchet prevents growth. The P0 ledger supplies the
additional semantic classification needed for measured burn-down. Neither
tool proves runtime reachability.

### Accepted P0a/P0b checkpoint

The initial classified ledger recorded 4,452 `gGL` member uses, 554 references
to the broader legacy-state symbol set, and 808 direct GL-shaped calls, with no
unclassified symbol. The first sanitation slice removed only declarations,
implementations, and inert construction sites with no executable caller:

- the unused texture-state reset, user clip-plane, and GL sync-fence helpers;
- three unused proposed `LLGLEnable` replacements and `LLGLSObjectSelect`;
- four unreferenced `LLRender` access/diagnostic methods; and
- fixed-function `LLGLSSpecular`, whose only construction supplied zero
  shininess and could not execute its material-state calls.

After regeneration the ledger contains 4,441 `gGL` uses, 516 broader state
references, and 796 direct GL-shaped calls. The narrower API-boundary metric
contains 408 legacy state-wrapper uses. All remaining state-wrapper symbols
are classified as live migration work rather than deletion candidates.

The Release `llrender` library and complete `vulkanstorm-bin.exe` target build,
the API-boundary ratchet passes, and the deterministic GHI contract suite
passes 36 of 36 tests. The obsolete pre-rename `firestorm-bin.vcxproj` remains
stale in an existing build directory and is not a valid build target; project
regeneration and all subsequent verification use `vulkanstorm-bin.vcxproj`.

### P0c explicit raster-state contract

The first state-contract completion slice adds the demonstrated raster-state
gaps to `PipelineDesc` rather than introducing GL-shaped GHI commands:

- fill, line, and point polygon modes;
- polygon depth-bias enable, constant factor, and slope factor; and
- line width, bounded by an explicit device limit.

The OpenGL peer compiles those fields to OpenGL 4.1 state. The Vulkan peer
compiles the same fields to static Vulkan rasterization state and enables the
optional `fillModeNonSolid` and `wideLines` device features only when exposed.
Both peers publish `nonSolidFill`, `wideLines`, and `maxLineWidth` capabilities,
and unsupported descriptors fail during pipeline creation. The pipeline-cache
identity includes every new field and canonicalizes signed zero.

Legacy point size and texture coordinate generation calls are not modeled as
virtual state. Their eventual production slices must express the same
rendering intent through shader inputs, shader logic, and explicit draw data.
This prevents obsolete fixed-function vocabulary from becoming part of the
permanent GHI interface.

The only executable fixed-function alpha-test enable was attached to an
untextured alignment-box draw and had no shader alpha-test contract. Three
legacy clip-plane enables likewise had no shader writing `gl_ClipDistance`.
Those four removed-mode enables were sanitized as inert OpenGL-core state,
rather than preserved as false GHI capabilities.

The regenerated ledger now contains 4,441 `gGL` uses, 512 broader state
references, and 796 direct GL-shaped calls, with no unclassified symbol. The
Release renderer library and complete viewer executable build, the boundary
ratchet passes, and the expanded deterministic GHI contract suite passes 37 of
37 tests.

### P0d OpenGL state compiler and cache

The OpenGL peer owns a native pipeline-state cache that compiles only changed
GHI fields into OpenGL calls. Rebinding the same pipeline becomes a no-op;
switching pipelines applies only changed program, vertex-array, cull, winding,
polygon, depth-bias, line, depth, stencil, blend, and color-mask state.
Repeated binding sets with identical dynamic offsets and repeated viewport and
scissor descriptions are also suppressed. Vertex and index input compilation
is retained in each GHI-owned vertex-array object and rebuilt only when its
explicit buffer binding changes.

The cache is valid only within one GHI rendering scope. `beginRendering()` and
`endRendering()` are explicit legacy-interoperation boundaries and invalidate
it, because production legacy rendering can still mutate the shared OpenGL
context between scopes. Failed native state compilation also invalidates the
cache. This preserves one trusted owner at a time until P0e removes the need
for the temporary legacy boundary.

### Additional reachability sanitation

A second reachability pass retired the old `LLPostProcess` subsystem. It had no
call to `apply()` anywhere in the tree; startup only allocated it, shutdown
deleted it, and GL teardown invalidated it. Its constructor explicitly stated
that the implementation did nothing until rewritten for the then-current
shader system. Removing it also removes its compatibility-only attribute-stack
calls without affecting the active deferred post-processing pipeline.

The same pass replaced a fixed-function face-color call with the existing
buffered `LLRender` color path, replaced a client-side physics-hull draw with
the existing `LLVertexBuffer::drawElements` path, and removed an unused
compatibility texture-residency probe that was compiled only by a dormant debug
metric. The ledger is now 4,423 `gGL` uses, 512 broader state references, and
779 direct GL-shaped calls, with no unclassified symbol.

The last executable fixed-function calls belonged to three unreferenced
pre-shader terrain methods. The active terrain entry point unconditionally
uses `renderFullShader()`; no caller remained for the two- and four-texture-unit
fallbacks or `renderSimple()`. Retiring those dormant methods removes all
texture-generation state and leaves no executable call classified as obsolete
fixed-function code. The current ledger is 4,322 `gGL` uses, 508 broader state
references, and 721 direct GL-shaped calls.

The face-color repair deliberately converted one raw fixed-function call in
`lldrawpool.cpp` to the existing shader-buffered `gGL` color path. The API
ratchet's per-file allowance for that file is therefore 22 rather than 21;
this reviewed one-use increase is paired with removal of the raw call and does
not raise the global facade ceiling. All subsequent P0 slices must burn this
temporary facade ownership down through semantic GHI state.

The 29 entries classified as legacy extension aliases are declarations in the
macOS compatibility header, not executable calls. They remain necessary to
build the OpenGL 4.1 peer against Apple's legacy header surface and are not a
reason to expose extension-shaped operations in GHI. The ten project-symbol
false positives are likewise retained only so the deliberately broad scanner
cannot silently lose them.

## P0e production migration order

No further legacy-state deletion is permitted merely because a function looks
old. All remaining wrapper symbols have source-level callers and require
runtime evidence before any reachability claim. P0e therefore
migrates coherent feature families, preserving a fully visible OpenGL frame
until the selected family passes both native peers.

1. **P0e1 — opaque production frame.** Adopt the already packetized rigid and
   rigged material, PBR terrain, shadow, and deferred-lighting work. Replace
   its ambient matrix, raster, binding, and draw ownership with the existing
   production frame packet and shared GHI targets. This is the first visible
   adoption candidate because I8c3 already has deterministic and private-live
   coverage on both peers.
2. **P0e2 — sky, clouds, water, and remaining environment.** Add explicit
   environment resources, passes, and composition without importing OpenGL
   texture or framebuffer names. Preserve reverse-Z and reflection/refraction
   semantics.
3. **P0e3 — alpha and particles.** Express legacy-sorted alpha, full-bright
   alpha, alpha masks, PPLL, and depth peeling as explicit route and pass data.
   Particles retain their legacy alpha-routing policy, but neither peer may
   interpret that policy as an OpenGL call path.
4. **P0e4 — recursive and offscreen views.** Adopt mirrors, hero/reflection
   probes, cube snapshots, impostors, previews, and pre-water alpha with their
   established transparency exclusions intact. Each nested view receives its
   own pass state and resources.
5. **P0e5 — UI, HUD, interaction, and snapshots.** Execute UI0-UI6, including
   clipping, picking, selection, media, snapshot framing guides, and readback.
   The visible Vulkan selector remains unavailable until this slice closes.
6. **P0e6 — appearance and baking.** Remove the remaining OpenGL assumptions
   from avatar baking, layer composition, and readback, then prove lifecycle
   behavior through teleport, relog, resize, and device recovery.

The static ledger provides the starting size of each ownership domain:

| Owner | `gGL` | state wrappers | direct GL-shaped references |
|---|---:|---:|---:|
| World renderer | 1,690 | 245 | 246 |
| Renderer core | 792 | 89 | 386 |
| UI, HUD, and interaction | 1,068 | 111 | 13 |
| Offscreen and recursive | 370 | 28 | 42 |
| Environment | 135 | 17 | 5 |
| Appearance and baking | 123 | 5 | 5 |
| UI core | 108 | 5 | 7 |
| Alpha and particles | 36 | 8 | 0 |
| Window and context | 0 | 0 | 17 |

Counts are coupling indicators, not estimates of engineering effort. A single
resource-lifetime or presentation reference can be more consequential than
many immediate-mode debug draws. P0e progress is accepted by semantic route
coverage and per-slice gates, not by raw count alone.

### P0e1 opaque production-frame convergence plan

P0e1 is the first production migration slice because the opaque world path is
already the best-validated shared GHI contract and the existing I8c3 work has
private live coverage on both peers. It deliberately keeps the visible OpenGL
frame authoritative until the complete opaque production route passes the same
structural, parity, and lifecycle gates on Vulkan.

1. **P0e1a — semantic frame packet.** Capture the production opaque world as a
   single bounded packet that includes rigid and rigged material draws, terrain
   and light inputs, shared target dependencies, semantic pass selection, and
   decode-resource identity without exposing viewer pointers, native GL names,
   or Vulkan handles.
2. **P0e1b — live assembly and validation.** Observe the actual main-view
   deferred frame, reject cross-frame or out-of-epoch work, and assert that the
   same frame carries the required opaque, material, and terrain components.
   This is the acceptance gate for frame assembly and deterministic identity.
3. **P0e1c — shared G-buffer ownership.** Reuse the accepted I8c1/I8c2 target
   topology and explicit GHI image ownership so opaque, material, terrain, and
   deferred-lighting work all write to the same hardware-owned attachments.
4. **P0e1d — opaque execution.** Execute the rigid/rigged material and terrain
   draws against shared G-buffer targets with explicit depth, blend, topology,
   and binding state. The visible OpenGL frame remains canonical until the
   Vulkan opaque execution reproduces the same semantic pass structure and
   bounded output.
5. **P0e1e — deferred-lighting boundary.** Insert the accepted lighting state
   and explicit shadow-projection dependencies into the same production frame
   without leaking rendered-resource ownership into the viewer layer.
6. **P0e1f — qualification.** Replay the captured packet on isolated OpenGL and
   Vulkan peers, compare pass structure and bounded image outputs under the
   existing tolerance model, then verify the live lifecycle without presenting
   the Vulkan path.

The accepted production opaque contract is intentionally additive. It does not
replace world rendering or UI rendering on OpenGL, and it does not open the
Vulkan selector. P0e1 proves that the backend-neutral production frame can be
assembled, transferred, targeted, and executed without ambient `gGL` ownership
inside the renderer-facing pipeline.

The critical execution rule is that opaque-pipeline ownership remains attached
only to the GHI packet, pass, and target contracts. Any step that still reaches
into `gGL` state, mutable OpenGL object names, or implicit viewport/bind state
is treated as a failed migration and is intentionally deferred to P0f.

The first implementation slice should therefore focus on the explicit opaque
frame packet, the G-buffer target ownership, and the opaque draw submission path
that already has deterministic and private-live coverage in the project's I8
checkpoints. Once those pass in both peers, the same discipline repeats for
P0e2, P0e3, and the later nested-view and UI slices.

### P0e2 environment convergence plan

P0e2 is divided into five independently reversible gates. The visible
OpenGL environment remains authoritative until the final gate passes on both
native peers.

1. **P0e2a — semantic packet.** Capture the complete viewer-computed
   atmosphere, cloud, celestial, and water inputs in one versioned,
   backend-neutral packet. Encode view policy, shared-target dependencies,
   transforms, decoded asset content, and water geometry explicitly.
2. **P0e2b — live assembly.** Populate that packet at the established viewer
   observation seam, reusing decoded-texture observations and rejecting
   cross-frame or cross-epoch assemblies.
3. **P0e2c — sky executor.** Execute HDRI or atmosphere, sun, moon, stars, and
   clouds into shared production targets with explicit reverse-Z depth and
   blend policy.
4. **P0e2d — water executor.** Execute above-water or underwater composition
   from explicit lighting, depth, exclusion, reflection, and refraction
   dependencies. Reflection generation itself remains a P0e4 nested-view
   responsibility.
5. **P0e2e — qualification.** Replay the same captured environment bytes on
   isolated OpenGL and Vulkan peers, compare pass structure and bounded image
   results, then run live lifecycle checks without presenting the Vulkan
   result.

The P0e2a contract is `EnvironmentScenePacket`. It deliberately does not reuse
the earlier fixture-only `EnvironmentState`: that sketch omitted most of the
production EEP/HDRI state, decoded asset identity, celestial and cloud routes,
water geometry, and attachment dependencies. Environment textures carry
source and content identities rather than viewer pointers or graphics-API
handles. Water consumes semantic shared-graph dependencies rather than an
OpenGL framebuffer name. Main and nested view kinds are encoded now so P0e4
can reuse the contract, but P0e2 does not execute recursive views.

P0e2a and P0e2b are accepted. The packet codec is deterministic, validates all
enumerations, finite state, unique semantic bindings, decoded texture content,
mutually exclusive routes, water geometry bounds, and explicit attachment
dependencies, and fails closed on malformed or trailing data. The old
`llghiworldcontract.h` declarations were removed: repository-wide reachability
showed that its terrain, lighting, environment, and pass-order sketches had no
production consumer; four assertions merely tested their own constants.

The live observer is dormant unless `VULKANSTORM_GHI_P0E2_CAPTURE` names an
output file. `VULKANSTORM_GHI_P0E2_WARMUP_SECONDS` controls the 120-second
default settle interval from 0 through 3600 seconds. It starts at the main-view
deferred boundary, observes state and asset identity in the actual sky and
water passes, and finalizes after post-deferred rendering. It reuses bounded
decoder-time pixels and performs no OpenGL texture readback. HDRI identity is
preserved but remains non-comparable until decoded HDRI source data is exposed
above `LLImageGL`.

The first real-grid P0e2b gate captured frame 7026 at 2560 by 1350. Independent
decode reported pass mask `0x75` (EEP atmosphere, sun, stars, clouds, and water
surface), all five shared water dependencies, ten of ten comparable textures
containing 520,192 decoded bytes, and seven water draws with 45,824 vertices
and 68,736 indices. The 2,263,896-byte packet identity was
`5e0ff8dced980203a4cdfe286e6dd9b5f47e418cc823c6806296dd48e5bc28ab`.
The viewer auto-logged in and exited cleanly; visible rendering remained native
OpenGL throughout.

### P0e2c — shared-target sky executor

`EnvironmentScenePacket` version 2 adds the exact CPU-observable sky geometry
that the production viewer submitted: dome strips, indexed sun or moon quads,
and the randomized star vertex/color stream. Each draw carries a semantic kind,
primitive topology, index range, and explicit model transform. The packet also
records whether the production sky pass writes its color through the emissive
G-buffer. Validation requires matching geometry for every active celestial
route and rejects invalid topology, range, transform, or bounded-resource data.
No OpenGL buffer, vertex-array, texture, framebuffer, or program name crosses
the seam.

One deterministic shader package implements atmosphere, sun, moon, stars,
clouds, and the reserved HDRI route for OpenGL 4.1/4.4 and Vulkan GLSL. The
shared production environment executor appends those routes to the existing
four G-buffer attachments and reverse-Z depth attachment after opaque geometry
and before deferred lighting. Its GHI pipelines explicitly describe depth,
blend, topology, vertex layout, textures, samplers, and bindings. Vulkan device
creation opportunistically enables the Vulkan 1.3
`shaderDemoteToHelperInvocation` feature when exposed because the pinned
glslang toolchain may lower ordinary GLSL `discard` to that capability.

A fresh real-grid packet-v2 capture recorded frame 4355 at 2560 by 1350 with
22,628 sky vertices, 41,063 indices, three geometry draws, ten comparable
textures, and the existing seven water draws. The 3,243,024-byte packet identity
is `2a7ca6f02acc1c4595b8b3272b072c1e5dc2cb80da210dfcaab9099c4e394d37`.
Its active sky work is one atmosphere, sun, star, and cloud execution; moon is
correctly inactive for the captured environment.

The same packet executes successfully in the isolated Vulkan and OpenGL peers
against the shared production targets. Both report sky coverage
`87380,0,87387,50853` and the same pass structure. Vulkan completes with
Khronos validation enabled and no validation error. Native hashes are retained
as evidence but are not required to be identical across APIs. The readback
gate sizes every host buffer before access, preventing an empty-span overrun
that the first real execution exposed. The deterministic GHI contract suite
passes 38 of 38 tests.

P0e2c is accepted as private, non-presenting execution. HDRI remains fail-closed
until a comparable decoded source is available, water execution remains P0e2d,
and P0e2e still must perform same-frame capture alignment, bounded image
comparison, lifecycle qualification, and steady-state resource-residency
optimization. Visible world and UI rendering remain OpenGL.

### P0e2d — explicit water execution boundary

The shared production water executor consumes the recorded water geometry,
normals, and environment state together with explicit shared-graph lighting,
depth, reflection-color, and water-exclusion image views. Generation identity
is checked before submission, and a missing, invalid, or stale dependency
rejects the execution. The executor does not inspect a viewer render target or
import an OpenGL texture, framebuffer, buffer, vertex-array, or program name.

One deterministic shader package supplies OpenGL 4.1 and Vulkan GLSL variants.
Both variants preserve the production reverse-Z occlusion direction explicitly
while avoiding a Vulkan attachment-feedback hazard: the completed scene depth
is sampled as a dependency rather than simultaneously attached for depth
testing. The output is a separate RGBA16F composition target, so this private
gate cannot alter the visible OpenGL frame.

The current isolated harness supplies one-pixel reflection and exclusion
fixtures solely to qualify resource binding, generation checks, draw
submission, copying, and readback. Those fixtures are not production
reflection or exclusion data and cannot support a visual-parity claim. Against
the existing real-grid capture, both native peers execute all seven recorded
water draws and report `water-modified=0`: no recorded water fragment survives
depth in that camera. Vulkan validation reports no error. The differing
OpenGL input/output hashes arise from copying the floating-point target and do
not constitute evidence that water was visible; the explicit modified-pixel
comparison is authoritative for this gate.

P0e2d therefore establishes the fail-closed GHI water seam but remains
provisional. Acceptance requires a same-frame, visibly above-water capture and
an underwater capture, real reflection-color and exclusion resources from the
P0e4 nested-view graph, replacement or bounded qualification of the provisional
water shading model against production atmosphere/reflection behavior, and
dual-peer image comparison. Visible world and UI rendering remain OpenGL.

### P0e2e — paired qualification gate

When both production-frame and P0e2 environment output variables are set, the
environment observer now uses the production-frame settle gate instead of its
independent timer. It records on the same candidate frame and writes only after
the production-frame file succeeds with that exact frame identity. A missing
production capture keeps the environment observer waiting; any cross-frame
pair fails closed and is not written. Independent environment-only capture
retains its own configurable warmup behavior.

The isolated peer harness rejects mismatched frame identities before creating a
graphics device. The comparison driver accepts the optional environment file,
passes both immutable packets to the OpenGL and Vulkan peers, verifies both
packet hashes and structural evidence, compares environment and modified-water
coverage under the existing bounded tolerance, and rejects captures in which
water changed no pixels. This converts the previously manual same-frame rule
into an executable gate.

P0e2e is not yet accepted. It still requires paired real-grid above-water and
underwater captures, and the water pass remains dependent on P0e4 production
reflection and exclusion resources. The existing frame 4655/frame 4355 files
are intentionally rejected as a cross-frame pair rather than reused as parity
evidence.

The first synchronized run captured both packets at frame 18246. The production
packet identity is
`6a702bcb6713c2879c6d8fa21e2013aeaeb8583b22cbb108e2e6d7ba67eeb64f` and
the environment identity is
`f10917e34b08fdc3aa81f7f20b94577f04f95f9eb60829329d80c3bc81cc7a03`.
Both peers execute identical environment structure and coverage, but both
report `water-modified=0`; the home camera contains recorded water geometry
without a visible surviving fragment. The gate correctly refuses to treat
this coordination proof as visible-water qualification.

### P0e3 alpha convergence plan

P0e3 is split so that production routing policy cannot be confused with an
algorithm fixture:

1. **P0e3a — semantic packet.** Preserve production sort order, material and
   skin inputs, view phase, selected method and bounded policy, standard,
   custom-blend, and particle classification, independent color/alpha blend
   state, full-bright and emissive intent, and minimum-alpha behavior.
2. **P0e3b — live assembly.** Observe main post-water alpha without changing
   visible ownership. Capture particles and custom blends as explicit residual
   work and retain the established exclusions for pre-water, HUD, impostor,
   reflection, cube-snapshot, dynamic-texture, and media views.
3. **P0e3c — shared legacy execution.** Execute sorted and residual work on
   GHI targets so a `LegacyResidual` route no longer means permission to issue
   OpenGL calls. Replay each emissive contribution exactly once.
4. **P0e3d — PPLL execution.** Replace the fixture geometry with production
   standard-blend submissions, bounded overflow handling, exact-layer limits,
   and deterministic resolve evidence. The existing OpenGL 4.4 gate remains.
5. **P0e3e — depth-peeling execution.** Execute production standard-blend
   submissions with the accepted four-layer, two-millisecond defaults and a
   bounded legacy tail.
6. **P0e3f — qualification.** Compare identical packets on both peers across
   cards, decals, glass, plants, appliers, rigged PBR, full-bright alpha,
   emissive replay, particles, custom blends, and lifecycle transitions.

P0e3a is accepted. `AlphaScenePacket` embeds the established material/skin
packet rather than defining a competing material model. Its per-draw policy is
one-to-one with the material draw stream, and vector order is the captured
legacy sort order. Validation fails closed on cross-frame identities, missing
geometry, mismatched skin ownership, invalid route/material classification,
unbounded PPLL or peeling settings, non-finite alpha thresholds, malformed
blend state, truncation, and trailing data. The binary representation is
deterministic and has no graphics-API handle. The contract suite passes 39 of
39 tests. No production traversal or visible rendering ownership changes in
this gate.

P0e3b live assembly is implemented. `LLGHIAlphaCapture` owns a narrowly scoped,
main-view observation
lifecycle around `renderGeomPostDeferred`; it never extends the R5 material
observer through post-deferred rendering and never creates a second GHI
OpenGL device. The existing material capture owner exposes one sequential,
bounded packet-assembly session so alpha capture reuses the same decoded
texture, material, skin, vertex, index, and draw construction. Assembly has
independent scene and resource epochs and therefore does not perturb R5 or
production-frame epoch tracking.

When `VULKANSTORM_GHI_P0E3_CAPTURE` names an output file, decoder-time texture
observation is armed from process startup. Capture waits 120 seconds by
default; `VULKANSTORM_GHI_P0E3_WARMUP_SECONDS` accepts a bounded override from
zero through 3600 seconds. No persistent setting is added. The observer first
records the already-culled alpha-mask render maps as `Mask`, then performs one
non-drawing walk of the actual post-water rigged and rigid alpha group order
before any selected PPLL or depth-peeling replay. That walk records particles,
custom blends, standard blends, full-bright and independent emissive intent,
actual color factors, the alpha pool's `ZERO` / `ONE_MINUS_SOURCE_ALPHA`
separate alpha factors, and the effective minimum-alpha threshold. The normal
OpenGL traversal remains the only visible renderer.

The owner is admitted only for the world camera, main render target, and
non-HUD, non-impostor, non-reflection, non-cube-snapshot path. Pre-water alpha
is not collected into the exact-method packet. Dynamic-texture and media
offscreen rendering do not enter this owner. These exclusions preserve the
existing routing contract; P0e4 and P0e5 must supply their explicit nested and
media view ownership rather than having P0e3 infer it from ambient OpenGL
state. The accepted depth-peeling defaults are now four layers and a
two-millisecond CPU submission budget.

`llrender_alpha_packet_inspector` independently decodes a saved packet and
reports identity, extent, policy/material draw counts, class and route counts,
rigged/full-bright/emissive coverage, decoded texture bytes, and bounded
geometry. Contract coverage includes deterministic round trip, cross-frame
identity rejection, route/material disagreement, production separate-alpha
factors, malformed blend state, bounded policy rejection, truncation, and
trailing-data rejection. A real packet still must demonstrate standard and
residual routes, particles, custom blend work, rigid and rigged alpha, legacy
and PBR materials, masks, full-bright work, and emissive replay intent where
the scene contains them. Missing scene categories must be reported rather than
fabricated. Shared legacy execution remains P0e3c work; `LegacyResidual` in
this packet is routing data and grants no backend permission to issue OpenGL
calls.

The first real-grid packet recorded frame 3931 at 2560 by 1350 after the full
120-second settle period. Independent decode reports 147 alpha policies and
147 material draws, 195,084 bounded vertices, 913,119 indices, 70 textures
containing 2,894,224 decoded bytes, 21 masks, 126 standard blends, 36 rigged
draws, 23 full-bright draws, and 21 independent emissive intents. Material
coverage is 144 legacy and three PBR draws; 69 of 70 texture resources are
comparable. All 126 standard blends preserve the active production color
factors and separate `ZERO` / `ONE_MINUS_SOURCE_ALPHA` alpha factors. Its
21,471,776-byte identity is
`fbb791dc1806fb73b99b34d29027489f4716691951c521d962affb18ecba479e`.
The packet is non-transient and records the selected legacy-sorted method.
The visible System OpenGL viewer remained responsive throughout capture.

That scene contained no particle or custom-blend submission, so it provides no
`LegacyResidual` coverage. P0e3b category qualification therefore remains
open until a settled scene deliberately supplies particles and a second
independent decode confirms their route. Current production assignment sites
leave non-particle draw-info on standard factors; only particle draw-info sets
custom factors, and particle classification intentionally takes precedence.
The separate `CustomBlend` route remains contract-tested but has no live
non-particle producer to capture. No validation rule is weakened to compensate
for the missing scene content.

A second settled real-grid capture deliberately placed a visible particle
emitter in view. Frame 3580 contains 46 policy/material draw pairs, 134,248
vertices, 616,200 indices, 16 masks, 28 standard blends, and two real particle
draws. Independent routing reports two `LegacyResidual` entries, with all
other blended work remaining `LegacySorted` for the selected method. The
packet also includes 36 rigged draws, 42 legacy and four PBR draws, three
full-bright and three emissive intents, and 45 of 46 comparable textures with
1,995,152 decoded bytes. Its 14,734,812-byte identity is
`92f1b53a398f2b17e0a84e62dd1c1239f72c74f9e6a7c6aa3c8fe2914ce2e47a`.
The packet is non-transient and the visible System OpenGL viewer remained
responsive throughout capture.

### P0e3c — shared legacy alpha execution

`ProductionAlphaExecutor` consumes the immutable `AlphaScenePacket` without
viewer objects or native graphics handles. It loads the private shared
RGBA16F lighting and reverse-Z depth attachments, preserves the packet's
blended draw order, compiles every captured independent color/alpha factor and
operation into an explicit GHI pipeline, and executes standard and residual
routes on both native peers. Alpha masks remain with their existing
G-buffer/depth owner and are counted rather than redrawn. Each blended draw
with emissive intent is replayed once in a separate alpha-only additive pass.
Neither `LegacySorted` nor `LegacyResidual` permits a backend to call OpenGL.

The production shader package accepts the established material/skin vertex
contract, decoded base-color and emissive resources, minimum-alpha threshold,
full-bright intent, explicit model and skin transforms, and bounded directional
lighting. OpenGL 4.1, OpenGL 4.4, and Vulkan variants share reflected bindings
and deterministic packaging. The executor fails closed on stale target or
lighting generations, malformed packets, singular transforms, invalid skins,
unresolved alpha-consumed textures, unsupported routes, and draw, geometry,
texture, or upload-budget excess. It reports route and defer-reason counts,
packet identity, modified pixels, and private-target hash.

Both real P0e3b packets replay successfully through isolated peers. Frame
3931 executes all 126 standard draws, retains 21 masks upstream, replays 19
blended emissive intents, defers no draw, and modifies 21,281 pixels on both
OpenGL profiles and Vulkan. Frame 3580 executes all 28 standard draws and both
particle residual draws, retains 16 masks, replays two blended emissive
intents, and defers no draw. OpenGL modifies 1,155 pixels and Vulkan 1,156,
within the explicit four-pixel/0.1-percent comparator bound. OpenGL 4.1 and 4.4
produce identical hashes and coverage for each packet; Vulkan validation is
clean. Native hashes are retained as evidence and are not required to be
bit-identical across APIs.

This remains private, non-presenting execution. The current alpha lighting is
a bounded explicit directional approximation because `AlphaScenePacket` does
not embed the complete production light/environment record; visual
qualification against the selected OpenGL renderer remains P0e3f work. The
packet also preserves legacy emissive replay intent but not a separate
per-vertex emissive magnitude, so P0e3c proves one replay owner and count while
final glow-value parity may require a contract extension before P0e3f. PPLL
capture/resolve remains P0e3d, depth peeling remains P0e3e, and custom-blend
live coverage remains unavailable until production has a non-particle custom
blend producer. No visible renderer selection or ownership changes in this
checkpoint.

### P0e3d — production PPLL capture and resolve

The production alpha shader and executor now add a bounded PPLL route for
eligible main post-water standard blends. Material, texture, skin,
minimum-alpha, full-bright, transform, and lighting inputs are identical to
the shared legacy executor. Masks remain with the G-buffer owner; particles
and any custom blend remain legacy residual work after exact resolve; each
emissive intent still has one replay owner. Head pointers and counters reset
through existing backend-neutral buffer transfers, so no GL-shaped clear
operation was added to GHI.

PPLL availability requires storage-image atomics, two storage buffers, a
usable policy allocation, and the exact-method shader profile. OpenGL exact
execution requires the packaged OpenGL 4.4 artifact. A separate
`p0_alpha_legacy` package retains storage-free OpenGL 4.1/macOS fallback, where
the same requested PPLL packet routes standard work to legacy sorting. Vulkan
uses the Vulkan 1.3 artifact and passes with validation enabled. The exact
package records compact 16-byte nodes, a per-pixel head image, allocation and
overflow counters, the packet's 4-to-32 exact-layer limit, depth sorting under
reverse-Z, weighted tail composition, and straight-alpha overflow fallback.

Because both archived P0e3b captures recorded `LegacySorted` as their selected
method, isolated `--ppll` replay changes only the decoded requested-method
field and reports both the immutable source identity and the effective packet
identity. The larger frame-3931 packet routes all 126 standard draws to PPLL,
retains 21 masks, replays 19 emissive intents, and defers nothing. OpenGL and
Vulkan modify the same 21,320 pixels, allocate 131,772 and 131,777 fragments
respectively from a 2,097,152-node pool, and overflow zero fragments. The
frame-3580 particle packet routes all 28 standard draws to PPLL, preserves both
particle residual draws, retains 16 masks, replays two emissive intents, and
defers nothing. Both exact peers modify 2,749 pixels; allocation differs by
three fragments and overflow is zero. OpenGL 4.1 executes all exact candidates
through its explicit sorted fallback.

Two deterministic 64-by-64 transforms of the particle packet exercise bounds
without being represented as captured runtime evidence. With eight nodes per
pixel and four exact layers, both peers allocate 27,000 fragments, overflow
zero, exceed the 16,384-fragment global four-layer bound, and therefore execute
the weighted-tail path. With one node per pixel, both allocate 27,000 fragments
into 4,096 nodes and report exactly 22,904 overflow fragments. Both stress
routes preserve particle residual work, modify all 4,096 pixels, and converge
with clean Vulkan validation.

P0e3d remains private and non-presenting. Native hashes are evidence rather
than a bit-identity requirement. A fresh viewer capture with PPLL actually
selected, steady-state memory/performance qualification, and comparison to
the selected visible renderer remain P0e3f work. Depth peeling remains P0e3e.

### P0e3e — production bounded depth peeling

The production executor now routes eligible main post-water standard blends
through a storage-free depth-peel package shared by OpenGL 4.1, OpenGL 4.4,
and Vulkan. The package preserves the production material, texture, skin,
minimum-alpha, full-bright, transform, lighting, particle residual, and
once-only emissive ownership established by P0e3c. It samples the completed
shared opaque depth rather than attaching it while sampling, and owns one
bounded private depth image per selected layer so exact replay never reads an
overwritten ping-pong result.

Each pass selects the next reverse-Z layer into depth only. After selection,
work beyond the last selected layer executes once in captured legacy order,
then retained exact layers replay farthest-to-nearest over that tail. Particles
and custom blends remain residual routes, masks remain upstream, and emissive
intent replays once after alpha composition. The packet's validated policy
bounds layers from 1 through 32; accepted defaults remain four layers and a
two-millisecond CPU submission budget. Result evidence records selected layer
count, budget exhaustion, tail execution, and pixels modified before exact
replay.

The frame-3580 particle capture routes all 28 standard draws through four
layers on all peers, preserves both residual particle draws and two emissive
replays, defers nothing, and renders a nonempty filtered tail. OpenGL modifies
1,155 final pixels and Vulkan 1,156. A deterministic 64-by-64 layered transform
retains four exact layers plus a 3,721-pixel tail and modifies all 4,096 pixels;
OpenGL 4.1, OpenGL 4.4, and Vulkan produce identical hashes with validation
clean.

The larger frame-3931 capture executes all 126 standard draws and 19 emissive
replays with no deferral. OpenGL completes four layers within two milliseconds;
Vulkan completes two and reports budget exhaustion. Both render a nonempty
filtered tail and modify the same 21,281 final pixels, although tail coverage
and hashes differ because the accepted budget deliberately permits a
backend-local number of exact layers. The comparator requires strict route,
identity, no-deferral, policy, tail-presence, and budget consistency, and keeps
tight pixel tolerances whenever layer counts match.

P0e3e remains private and non-presenting. The archived captures selected legacy
sorting, so isolated `--peel` replay changes only requested policy and reports
source and effective identities separately. A fresh viewer capture with depth
peeling selected, GPU timing/performance qualification, and selected-renderer
visual comparison remain P0e3f work.

### P0e3f — qualification progress

Fresh System OpenGL sessions captured both exact methods from an alpha-rich
real-grid scene after the full 120-second settle period. The comparator's
`--require-captured-method` gate requires the source-file SHA-256 to equal the
effective packet SHA-256, preventing post-decode policy mutation from being
mistaken for live selected-method evidence. Both packets are main post-water,
non-transient, independently decodable, and contain real bounded geometry,
rigid and rigged work, legacy and PBR materials, masks, standard blends,
full-bright and emissive intent, and one particle residual route.

The PPLL-selected frame 7084 packet is 18,791,708 bytes with identity
`4896efc7ef9a926069f36e824bea1885a3a49bd4f288b9cecf050e62b64ad137`.
It contains 86 policy/material draw pairs: 32 masks, 53 standard blends, and
one particle. OpenGL 4.4 and Vulkan route all 53 standards through PPLL,
retain the particle residual, replay two blended emissive intents, defer no
draw, allocate 268,165 and 268,178 fragments respectively from a 2,097,152-node
pool, overflow zero, and modify the same 189,863 pixels. OpenGL 4.1 retains
the exact candidates through its explicit sorted fallback. The login followed
a teleport into the qualification scene; capture occurred only after mesh
download quiescence and reports `transient_load=0`.

The depth-peel-selected frame 4283 packet is 17,995,612 bytes with identity
`3c579dee4d2a81a34af5b0408df153016845aa43569e28cd97d9fd7330f059ff`.
It contains 88 policy/material pairs: 36 masks, 51 standard blends, and one
particle. OpenGL 4.1 and 4.4 complete four layers within the accepted
two-millisecond CPU submission budget; Vulkan completes two and reports budget
exhaustion. Every peer retains the particle residual, replays two blended
emissive intents, defers no draw, renders a nonempty filtered tail, and modifies
143,997 or 144,003 final pixels within the bounded comparator tolerance. Vulkan
validation is clean. This session also proves clean relog assembly after the
PPLL capture.

A dedicated in-world particle emitter now supplies real custom-factor evidence
without fabricating `CustomBlend` classification. It uses the Second Life
particle blend equation `SOURCE_ALPHA + ONE`; the uploadable source is
`doc/renderer/p0e3-custom-blend-particles.lsl`. A settled, non-transient
PPLL-selected frame 6535 capture has identity
`775b8f437d81411b43d5f645c629de926fcc182b98ae26bb43019d906e40e33a`.
Its four draws are all full-bright, emissive particle residuals. Two retain the
requested additive equation and two retain ordinary production factors. The
packet inspector's independent gates require both nonstandard particle factors
and the exact `SOURCE_ALPHA + ONE` color pair; the archived ordinary-particle
capture fails the latter gate as expected.

This residual-only packet exposed a package compatibility edge: the exact PPLL
shader reflects binding group 3 even when routing produces zero exact draws.
The shared executor now binds a one-pixel head image, one inert node, and one
counter for that case instead of allocating the full PPLL pool or dropping the
captured method. OpenGL 4.4 and Vulkan validation report PPLL available with
zero exact draws, allocation, or overflow; OpenGL 4.1 retains its explicit
legacy fallback. All peers preserve four residual routes, replay four emissive
intents, defer nothing, and modify exactly 8,489 pixels. The strict comparator
requires residual-only composition, preserved source/effective packet identity,
zero exact work, and the expected per-profile capability distinction.

P0e3f is not yet accepted. Captured class names do not independently prove
every requested content category such as decals, plants, glass, or appliers.
Selected visible OpenGL output has not been read back and compared to private
peer output; steady-state memory/GPU timing, resize, explicit device recovery,
macOS OpenGL 4.1, and Mesa+Zink provider evidence remain open. Native hashes
remain evidence only: PPLL atomic append order and budget-local peel layer
counts are not cross-API bit-identity contracts.

### P0e1a — coherent generic opaque adoption

The production-frame contract is version 2 and includes the existing rigid
generic-opaque stream alongside material, terrain, and lighting streams. All
four child packets must have the same frame identity and source extent. The
frame cannot validate without non-empty generic opaque, material, and terrain
draw streams or without the corresponding explicit pass bits.

The shared production G-buffer executor consumes the generic opaque stream
before PBR material and terrain draws in the same rendering scope, using the
same four G-buffer attachments and reverse-Z depth attachment. It uses the
established backend-neutral R4 shader package, explicit transform binding,
vertex/index buffers, and culled or double-sided GHI pipelines. The executor
does not import an OpenGL buffer, texture, framebuffer, or shader name.

Viewer-side capture now retains the same-frame rigid opaque component for I8
frame assembly. A missing, stale, or cross-frame opaque component rejects the
whole observation instead of silently producing a partial opaque frame. The
transfer and execution budgets account for opaque draws and geometry, and the
live acceptance gate requires the opaque stream plus all three geometry
G-buffer targets to contain work. The emission target may remain clear when a
valid scene contains no emissive contribution.

P0e1a is a private, non-presenting adoption checkpoint. It establishes a
coherent opaque input and execution graph but does not yet reroute the visible
OpenGL frame or expose Vulkan selection. Generic opaque shadow-caster parity,
remaining opaque pool coverage, target integration, and live dual-provider
qualification remain P0e1 work.

### P0e1b — legacy opaque material convergence

The live material route now admits opaque legacy diffuse, full-bright, shiny,
bump, normal/specular, emissive-material, rigid, and rigged passes in addition
to opaque glTF PBR. It reuses `MaterialScenePacket`; no second texture,
material, or skin contract was added. Alpha-mask and blend passes remain
excluded from the receiver route until P0e3, although the established shadow
capture continues to preserve alpha-masked caster data.

Runtime geometry requirements now follow material semantics. Lit geometry
requires normals, a normal-map binding requires tangents, and textured work
requires texture coordinates; full-bright or untextured work is not rejected
for attributes it cannot consume. CPU-observed diffuse, normal, and legacy
specular resources continue through the existing residency cache without
importing an OpenGL texture name.

The shared G-buffer executor accepts both material models. Legacy base color,
specular color and gloss, environment intensity, normal mapping, and
full-bright intent receive an explicit packed representation. The deferred and
projector shader packages select a legacy Blinn-style lighting equation from
that model marker while retaining the metallic/roughness equation for PBR.
Full-bright legacy work emits its sampled base color and contributes neither
diffuse nor specular direct light. The OpenGL 4.1, OpenGL 4.4, and Vulkan GLSL
dialects carry the same contract.

The validation frame now executes a legacy opaque material alongside rigid and
rigged PBR, terrain, directional/projected lighting, and opaque/masked shadow
casters. The deterministic shader packer, 37-test GHI contract suite, Release
renderer library, and complete `vulkanstorm-bin.exe` build pass. This remains
a private offscreen adoption checkpoint: visible-target integration and live
dual-provider qualification still remain P0e1 work.

### P0e1c — single ownership of simple opaque work

The R4 generic-opaque stream is now a complementary untextured rigid fallback,
not a second rendering owner. Textured `PASS_SIMPLE` work routes only through
the legacy material contract introduced in P0e1b. The draw-pool traversal
passes its actual textured/untextured intent to capture instead of inferring
ownership from whether a default viewer texture object happens to exist.

`OpaqueGBuffer` remains an explicit scheduled pass, but its draw stream may be
empty. Frame validation and G-buffer execution require material and terrain
work and safely accept zero fallback vertices, indices, transforms, and draws.
The live gate now requires executable legacy and PBR material coverage rather
than using a duplicate simple draw as a proxy for legacy coverage. Contract
tests execute both a populated fallback and the normal zero-fallback case.

This closes duplicate opaque ownership in the private production graph. The
remaining P0e1 gates are isolated OpenGL-peer execution, cross-peer semantic
comparison, and live qualification before any visible OpenGL draw family can
be redirected.

### P0e1d — isolated native-peer replay gate

Production-frame qualification does not create a second GHI OpenGL device on
the viewer's live context. Doing so would allow the isolated peer to change
program, vertex-array, framebuffer, texture, buffer, and raster state behind
the production renderer's legacy caches. Instead, the viewer serializes one
immutable, backend-neutral `ProductionFramePacket` after a configurable scene
settle interval. The visible world and UI remain on their selected OpenGL
provider throughout capture.

Set `VULKANSTORM_GHI_PRODUCTION_FRAME_CAPTURE` to the desired `.llghif` path
before starting the viewer. This one setting arms the otherwise developer-only
assembly route without changing persistent graphics settings. Capture waits
120 seconds by default so decoded scene resources can settle, ignores
incoherent observations, writes the first complete bounded frame once, records
whether collection reached a capture budget in the log, and then disarms. A
dense scene may legitimately reach a collection bound; replay must compare the
same recorded subset rather than treating that safety limit as invalid input.
Tests may override the wait from 0 through 3600 seconds with
`VULKANSTORM_GHI_PRODUCTION_FRAME_CAPTURE_DELAY_SECONDS`.

The capture request also arms the existing bounded decoder-observation cache
from process startup. This preserves small, backend-neutral texture samples
while normal J2C decoding owns them; it neither reads texture pixels back from
OpenGL nor retains native texture names. Without those observations a late
capture could contain valid asset metadata but no executable peer texture
resources.

Two standalone executables consume the exact saved bytes:

- `llrender_opengl_production_frame_harness` creates an isolated WGL context
  and executes the OpenGL 4.1-capable shader artifacts;
- `llrender_vulkan_production_frame_harness` creates no window or OpenGL
  context and executes the Vulkan GLSL artifacts, optionally with validation;
  and
- `scripts/renderer/compare_ghi_production_frame.py` runs both, requires the
  same packet identity, extent, draws, residency, lights, shadow routes, and
  active maps, then applies explicit absolute and relative tolerances to
  G-buffer, lighting, and shadow coverage. Native pixel hashes are reported
  as evidence but are not required to be bit-identical across APIs.

The comparator rejects an empty required geometry G-buffer or lighting target;
the legacy-specular and emission targets may be clear when the captured scene
has no contribution for them. It
requires covered work in each active shadow category, not every individual
cascade, because a valid camera can leave one cascade clear. This preserves
the corrected I7 acceptance rule while still detecting a missing directional
or projector-shadow implementation.

The first real-grid gate captured frame 20966 after the 120-second settle
period. Its 7,525,836-byte packet contains 3,599,360 decoded texture bytes, 29
captured material draws, 64 terrain draws, four directional shadow cascades,
and 36 local lights. The executable subset on both peers is identical: 28
material draws (all legacy in this scene), five rigged material draws, 64
terrain draws, and 29 shadow casters. The packet identity is
`5154c81d7a5abe1368685ebd096f7ae69b9020b1587c06b8cb94b2d871b0ad82`.

Real replay exposed three missing OpenGL-peer cases that the synthetic
fixtures had not reached. The OpenGL 4.1 state compiler now accepts a
depth-only rendering scope, creates a pipeline with a depth attachment and no
color attachment, and reads a complete depth mip into a verification buffer.
Depth-only scopes explicitly select `GL_NONE` draw/read buffers, and reused
framebuffers detach stale color attachments. These are valid modern OpenGL
operations hidden entirely below the GHI boundary.

With Vulkan validation enabled, both peers passed. G-buffer base/normal and
lighting coverage differ by one pixel; specular coverage and every directional
shadow-map coverage count are identical. The emission target is clear on both
because the captured executable subset has no emissive contribution. No
Vulkan validation error was reported. P0e1 is therefore accepted for private,
non-presenting production execution; visible world ownership remains OpenGL
until the later integration and selector gates authorize exposure.

## State-ownership invariant

During incremental conversion there must not be two independently trusted
OpenGL state caches. Before a legacy caller and the GHI OpenGL peer can share a
context, the caller must either route state through the common owner or enter a
reviewed interoperation boundary that invalidates the GHI cache.

A temporary compatibility adapter may collect legacy operations into pending
semantic state, but every submitted draw must snapshot that state explicitly.
The adapter must not become a permanent collection of GL-shaped virtual calls.

## Compatibility requirements

- The common OpenGL renderer remains compatible with OpenGL 4.1 core.
- PPLL retains its separate OpenGL 4.4 capability gate.
- macOS retains depth peeling and the OpenGL 4.1 shader path.
- Particles may retain legacy alpha-routing policy, but the final Vulkan route
  may not interpret "legacy" as permission to call OpenGL.
- Mirrors, hero/reflection probes, cube snapshots, HUDs, impostors, and
  pre-water alpha retain their established alpha-routing exclusions.

## Per-slice gate

Each migrated production slice must:

- pass the deterministic GHI contract suite and API-boundary ratchet;
- produce the same semantic work on the OpenGL and Vulkan peers;
- remain clean under OpenGL debug checks and Vulkan validation;
- preserve System OpenGL and Mesa + Zink output and lifecycle behavior;
- avoid steady-state pipeline compilation or increased redundant state calls;
- reduce or explicitly account for its legacy coupling inventory; and
- remain independently revertible until live parity is accepted.

P0 closes only when every Vulkan-reachable production render path is expressed
through GHI and any remaining OpenGL implementation is confined to the native
OpenGL peer or is a reviewed, structurally unreachable exception.
