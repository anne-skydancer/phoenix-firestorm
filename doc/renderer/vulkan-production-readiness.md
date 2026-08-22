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

### Development baseline inherited from prior work

The current branch is an advanced integration scaffold, not a new backend
implementation. Development must preserve and adopt the submitted work rather
than recreate it:

- GHI device, resource, pipeline, descriptor, command, transfer, query, shader
   package, and validation contracts are implemented.
- Native Vulkan resource execution and Win32 swapchain lifecycle are implemented
   and were independently exercised, but they currently own separate Vulkan
   instance/device domains.
- Coherent production-derived material, rigged, terrain, lighting, projector,
   shadow, environment, water, and alpha packets and private executors exist.
- Opaque/PBR, terrain, directional/projector lighting and shadows, sky, legacy
   alpha, PPLL, depth peeling, particles, emissive replay, and custom-factor
   particles have private OpenGL/Vulkan execution evidence.
- These results are private and non-presenting. Production startup still creates
   OpenGL first, and no viewer call site presents a GHI-rendered Vulkan frame.
- Recursive views, production reflection/exclusion resources, visible final
   composition, UI/HUD/media, picking, snapshots, and appearance remain outside
   Vulkan production ownership.

Status terms in this plan are cumulative:

- **Implemented:** code and contracts exist.
- **Validated private:** deterministic or production-derived execution passed in
   isolated harnesses without owning the visible viewer frame.
- **Production-owned:** the selected backend owns the live viewer operation.
- **Release-qualified:** supported hardware, correctness, performance, recovery,
   and operational gates pass.

Most existing world executors are **validated private**, not missing. Later
phases adopt them into production ownership and close residual parity evidence.

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

### Phase 0: GHI convergence and qualification

Phase 0 follows the complete P0a-P0g convergence sequence in
`p0-ghi-state-convergence.md`, not only baseline collection. It must:

1. Maintain the semantic ledger, remove proven dead state, and complete the
   backend-neutral state contract and OpenGL compiler.
2. Express every Vulkan-reachable production family through GHI: world,
   environment, alpha, recursive views, UI/interaction, and appearance/baking.
3. Remove OpenGL-owned behavior and resource lifecycle from shared production
   code, and confine OpenGL implementation to the native OpenGL backend and
   context bootstrap.
4. Qualify semantic, image, lifecycle-contract, performance, memory, provider,
   hardware, recovery, and coupling evidence.
5. Preserve representative OpenGL references and immutable evidence bundles.
6. Enforce the API-coupling budget and deterministic GHI tests in CI.

Phase 0 remains private and non-presenting where necessary. A headless complete-
frame harness may execute production semantics on one selected GHI device and
compare them with visible OpenGL references without changing viewer startup or
production presentation ownership. Those changes begin only in Phase 1.

Exit gate: every P0a-P0g row and P0.8 in the closure manifest is accepted; all Vulkan-
reachable production semantics replay through GHI; image, correctness,
lifecycle-contract, CPU/GPU time, memory, provider, hardware, recovery, and
coupling evidence passes; and production code has no OpenGL-owned rendering
behavior or resource lifecycle.

#### Phase 0 completion assessment

**Verdict: partially completed before the workstation reset; custom-factor
particle alpha was validated, while final alpha qualification and broader Phase
0 acceptance remained open.** The tests, image acquisitions, and iterative
corrections performed before the reset produced the current GHI/Vulkan code;
they must not be treated as absent merely because raw artifacts were lost. The
checkpoint documents also explicitly mark unclosed gates. Historical validation,
historical open work, and current-revision revalidation must therefore be
tracked separately.

#### P0 gate ledger

| Gate | Current status | Closure boundary |
| --- | --- | --- |
| P0a/P0b ledger and sanitation | Accepted | Revalidate the current merged revision and preserve the generated ledger. |
| P0c state-contract completion | Accepted on current revision | The registered deterministic `ghi-contract` CTest passes with nested-view packet positive and negative validation coverage. Preserve its immutable evidence manifest. |
| P0d OpenGL state compiler/cache | Accepted on current revision | Native OpenGL compiler/cache/sRGB validation and OpenGL 4.1/4.4 canonical-hash parity with Vulkan pass. Preserve their immutable evidence manifests. |
| P0e1 opaque production frame | Accepted for private, non-presenting execution | Replay the accepted packet on the current OpenGL/Vulkan peers and preserve current-revision evidence. |
| P0e2a/P0e2b environment packet/live assembly | Accepted | Revalidate codec and same-frame assembly while closing downstream water dependencies. |
| P0e2c sky executor | Accepted for private, non-presenting execution | Revalidate current peers; retain fail-closed HDRI behavior unless comparable decoded HDRI data is available. |
| P0e2d water executor | Provisional | Supply real same-frame reflection-color and exclusion resources from P0e4, above-water and underwater captures, qualified shading, and dual-peer image evidence. |
| P0e2e paired environment qualification | Explicitly unaccepted | Pass same-frame packet identity, nonzero visible water contribution, bounded peer comparison, lifecycle-contract checks, and residency evidence. |
| P0e3a alpha packet | Accepted | Revalidate codec and routing contracts. |
| P0e3b live alpha assembly | Implemented with live evidence; acceptance not recorded | Record explicit acceptance after current-revision category and packet checks. |
| P0e3c legacy alpha executor | Validated private; acceptance not recorded | Revalidate selected visible reference comparison, lighting/glow inputs, and route ownership. |
| P0e3d PPLL executor | Validated private; acceptance not recorded | Revalidate selected-method capture, bounded/overflow stress, visible reference, timing, and memory. |
| P0e3e depth-peel executor | Validated private; acceptance not recorded | Revalidate selected-method capture, tail/layer policy, visible reference, timing, and memory. |
| P0e3f alpha qualification | Explicitly unaccepted | Close content breadth, visible comparison, resize/relog/teleport/recovery, provider, performance, and memory evidence. |
| P0e4 recursive/offscreen views | In progress; P0e4a replay accepted on revision `e921ced8be25bd6fe55c2f28944276abed20a861`, expanded production capture and peer replay validated | The authenticated 23-pass packet SHA-256 `d7149541445ff4d118e300e29937d0767cfdd447b7101874125272469a243802` retains six ReflectionProbe, six Mirror, and six CubeSnapshot passes independently plus all five required single views. Packet-driven Vulkan validation and OpenGL peers cover all 1,472 pixels with matching normalized image SHA-256 `c88c79c1fe9b9ff495a4817c99eae2acf05ef879a150fad038e239383bb9fdb2`. Preserve this evidence in an immutable manifest, then acquire same-frame production/environment evidence, replace semantic clear imagery with real scene content, and export reflection/exclusion resources for P0e2. |
| P0e5 UI/HUD/interaction/snapshots | Open; no acceptance checkpoint | Execute UI0-UI6 semantics in a headless complete-frame compositor, including fonts, media, HUD ordering, picking, snapshots, DPI, and shipped skins. |
| P0e6 appearance/baking | Open; no acceptance checkpoint | Add backend-neutral layer/mask/composition/readback contracts and private peer bake execution. |
| P0f legacy confinement | Open; no acceptance checkpoint | Burn down migrated coupling and prove complete-frame Vulkan reachability without an OpenGL context. |
| P0g qualification | Open; no acceptance checkpoint | Produce one complete immutable eligibility manifest with no failed or pending P0 gate. |
| P0.8 production decoupling | Open; dependent on P0g qualification | Remove production-side `gGL`, legacy state-wrapper, direct OpenGL, native-handle, and resource-lifecycle ownership; amend the closure manifest before Phase 1. |

| Requirement | Historical Phase 0 status | Current evidence/revalidation status |
| --- | --- | --- |
| Representative OpenGL baselines | Partial. Production-derived packets, selected-method captures, deterministic fixtures, and private peer comparisons were acquired. The final selected visible OpenGL readback comparison and complete content-category coverage remained open. | Checkpoint records and exact fixture hashes remain. Restore the raw corpus where possible and reacquire the missing visible references. |
| Performance and memory baselines | Partial. Individual stress and timing evidence was recorded, but P0e3f explicitly left steady-state memory/GPU timing open. | Preserve surviving measurements and collect the missing current-revision distributions before accepting performance-sensitive lifecycle changes. |
| Backend-neutral lifecycle contracts | Partial but substantially exercised by validation and native harnesses. | Resize, explicit device recovery, unified production ownership, and visible presentation remained open; Phase 1 owns their completion. |
| Checked GL-coupling budget | Established and used during Phase 0. | The reflection-map query regression is backend-confined through the GHI query bridge, and the current boundary ratchet passes without increasing its baseline. |
| Deterministic GHI tests | Completed and repeatedly executed before reset. | `LL_TESTS=ON` now enables CTest globally and the registered `llrender_ghi_contract` test passes with `ghi-contract`, `deterministic`, and `qualification` labels. A complete `ALL_BUILD` qualification run remains blocked by the legacy isolated `llprimitive.cpp` test's unresolved `LLPrimTextureList` linkage; the renderer qualification target itself is green. |
| Supported-hardware evidence | Partial. The development hardware and OpenGL 4.1/4.4/Vulkan peers were exercised, but macOS OpenGL 4.1, Mesa+Zink, broader Windows coverage, and Linux qualification remained open. | Reconstruct the surviving machine manifest and publish a complete qualification matrix before production eligibility decisions. |

#### Alpha validation boundary

The code validates the recollection that a custom-alpha case passed, with an
important terminology limit. The accepted live asset was a particle emitter
using custom color factors `SOURCE_ALPHA + ONE`. Production capture classifies
it as `Particle`, not `CustomBlend`, because particle classification takes
precedence. Its packet was independently inspected and replayed by OpenGL 4.4
and Vulkan with four residual draws, four emissive replays, no deferral, clean
Vulkan validation, and exactly 8,489 modified pixels.

`AlphaSubmissionClass::CustomBlend` is not backed by a supported non-particle
content type in the current viewer. Ordinary object, legacy material, and glTF
material content selects alpha modes, not arbitrary source/destination blend
factors. `LLDrawInfo` defaults to standard source-alpha blending, and the only
runtime writer of alternate factors is the particle geometry path. Although
the alpha observer can classify any hypothetical non-particle draw with
alternate factors as `CustomBlend`, no reachable producer creates such a draw.
The class is therefore reserved routing/executor infrastructure, not a content
feature requiring production qualification today.

| Alpha area | Pre-reset evidence | Remaining boundary |
| --- | --- | --- |
| Additive custom-factor particle | Validated from a live, settled PPLL-selected capture and private OpenGL/Vulkan replay. | Visible selected-renderer readback and full production-route ownership were not validated. |
| Non-particle `CustomBlend` | Routing and executor behavior are contract-tested, but no viewer content model supports arbitrary non-particle blend factors. | No reachable runtime producer exists. Treat the class as reserved infrastructure unless a future content feature introduces one. |
| Non-Add blend operations | Packet and executor support `Subtract`, `ReverseSubtract`, `Minimum`, and `Maximum`. | Live capture records factors but leaves operations at `Add`; no live evidence validates these operations. |
| Ordinary particles and legacy sorted alpha | Validated through production-derived private replay. | Visible GHI/Vulkan ownership and final visual qualification remained open. |
| PPLL | Selected-method live capture and private OpenGL/Vulkan execution validated, including bounded and overflow stress. | Selected visible OpenGL comparison, steady-state memory/GPU timing, resize, and recovery remained open. |
| Depth peeling | Selected-method live capture and private OpenGL/Vulkan execution validated with bounded comparator tolerances. | Selected visible comparison, GPU timing, resize, and recovery remained open. |
| Masks | Captured and counted with their established G-buffer/depth owner. | They were not redrawn by the alpha executor; complete visible-frame ownership remained open. |
| Final P0e3f qualification | Not accepted. | Content breadth, visible-output comparison, performance/memory, resize, device recovery, macOS, and Mesa+Zink evidence remained open. |

#### P0 closure plan

**P0 is a hard prerequisite. Phase 1 production-lifecycle work must not begin
until the P0 closure manifest records every P0a-P0g gate and P0.8 accepted.** Validation
may use headless/private selected-device harnesses; it must not take visible
production ownership early.

##### P0.0: Restore the qualification foundation

1. Recover pre-reset packets, images, traces, timing, memory, and machine
   manifests where available. Reacquire only missing artifacts or evidence for
   historically open gates.
2. Define one versioned result manifest containing revision, packet/artifact
   hashes, scene/content identity, camera/avatar state, settings, resolution,
   warmup/sample window, adapter, driver, tool versions, and pass/fail criteria.
3. Move the reflection-map query regression below GHI so the coupling ratchet
   passes without increasing its baseline.
4. Enable `LL_TESTS` in a qualification build. Register deterministic and native
   fixtures with `ghi-contract`, `deterministic`, `opengl`, `vulkan`, `hardware`,
   and `qualification` labels and explicit unavailable-hardware skip behavior.
5. Add `master` to pre-commit push coverage and require the coupling check.

Pass: coupling and deterministic tests are green, evidence storage is durable,
and every later gate can emit the common manifest format.

##### P0.1: Revalidate accepted foundations

1. Revalidate P0a-P0d and record explicit P0c/P0d acceptance.
2. Replay P0e1 and P0e2c accepted bytes on OpenGL 4.1/4.4 and Vulkan with clean
   validation, nonzero required coverage, matching semantic structure, and
   bounded image comparisons.
3. Revalidate P0e3a routing/codec and record explicit P0e3b packet/category
   acceptance on the current revision.

Pass: accepted historical slices have current-revision manifests; no completed
packet/executor work is reimplemented.

##### P0.2: Implement P0e4 recursive/offscreen semantics

1. Add a versioned nested-view packet and production observer for reflection and
   hero probes, mirrors, six-face cube snapshots, impostors, previews, and
   pre-water alpha.
2. Replay bounded nested schedules privately on both peers using existing GHI
   cube-array, offscreen-pass, query, and readback capabilities.
3. Prove recursion bounds, pass order, face/mip order, generation identity,
   exclusions, nonempty production evidence, and bounded peer images.
4. Export real reflection-color and water-exclusion resources for P0e2.

Pass: every production nested-view class has semantic and image evidence, and
the generated reflection/exclusion dependencies satisfy the water executor.

##### P0.3: Close P0e2d/P0e2e environment and water

1. Capture paired same-frame production/environment packets for one visibly
   above-water and one underwater scene using P0e4 resources.
2. Require matching frame/packet and resource generations, real reflection and
   exclusion inputs, nonzero `water-modified`, identical pass structure, bounded
   peer image comparison, clean validation, and stable resource residency.
3. Qualify the provisional water model against visible System OpenGL or replace
   the approximation required for bounded parity.

Pass: both scenes pass `compare_ghi_production_frame.py` with complete manifests
and P0e2d/P0e2e are explicitly accepted.

##### P0.4: Close P0e3f alpha qualification

1. Reuse selected legacy, PPLL, depth-peel, ordinary-particle, and additive
   custom-factor particle captures; reacquire only missing content classes.
2. Add cards/decals, glass, plants, appliers, rigid/rigged legacy and PBR, masks,
   full-bright, and emissive coverage where supported. Non-particle
   `CustomBlend` is not a gate because no producer exists.
3. Compare selected visible OpenGL readback with private peer output and close
   the provisional lighting/glow input gap.
4. Record no-deferral route evidence, PPLL allocation/overflow, depth-peel
   layer/tail policy, GPU timing, steady-state memory, resize, relog, teleport,
   and injected recovery results on required providers.

Pass: `compare_ghi_production_alpha.py` and lifecycle/performance manifests pass
for every supported method and content class; P0e3b-f are explicitly accepted.

##### P0.5: Implement P0e5 UI/HUD/interaction/snapshots

1. Reuse the R8 interaction fixture and implement UI0-UI6 production-semantic
   packet/replay for geometry, scissor, clipping, fonts/emoji, atlases, HUD
   order, media updates, selection IDs, pick depth, snapshots, and framing
   guides.
2. Exercise every shipped skin and supported DPI/UI scale in a headless complete-
   frame compositor. P0 validates complete semantics and images; visible window
   presentation remains Phase 1.
3. Record input/atlas generations, color/ID/depth outputs, hashes, readback
   latency, GPU timing, and memory.

Pass: deterministic and bounded peer comparisons cover UI, HUD, media, picking,
and snapshots with no native API leakage above GHI.

##### P0.6: Implement P0e6 appearance and baking

1. Add backend-neutral layer, mask, composition, morph-mask, upload payload, and
   asynchronous readback contracts.
2. Capture identical appearance inputs and execute private OpenGL/Vulkan peers
   through outfit change, rebake, relog, teleport, resize, and recovery.
3. Compare baked images and morph masks exactly or with documented bounded
   criteria and preserve upload payload hashes, timing, and memory evidence.

Pass: appearance/baking has no renderer-facing OpenGL dependency and P0e6 is
explicitly accepted.

##### P0.7: Close P0f confinement and P0g qualification

1. Regenerate the legacy-state and API-boundary ledgers. Remove migrated
   ownership; approve only structurally unreachable exceptions.
2. Run a headless complete-frame Vulkan reachability harness covering world,
   recursive views, environment, alpha, UI/media, readback, and appearance
   without creating an OpenGL context.
3. Execute the provider/hardware matrix, image/correctness suite, lifecycle
   fault injection, device-loss report, region-entry/teleport soak, timing,
   memory, and pipeline/residency stability checks.
4. Emit one immutable P0 closure manifest with no failed or pending gate.

Pass: every Vulkan-reachable path traverses GHI, remaining OpenGL is confined or
approved unreachable, and P0g records all semantic, visual, lifecycle,
performance, provider, recovery, and coupling evidence as accepted.

##### P0.8: Complete production decoupling

1. Remove production-side `gGL` draw assembly, matrix, binding, dynamic-state,
   and lifecycle ownership from shared renderer, world, recursive/offscreen,
   environment, alpha, UI, and appearance paths.
2. Replace live `LLGLState` wrappers and state bundles with GHI pipeline,
   binding, pass, draw-data, and lifecycle contracts. Confine direct OpenGL
   calls and native handles to the OpenGL backend and context bootstrap only.
3. Regenerate the legacy-state and API-boundary ledgers with no production
   exceptions, then rerun complete-frame OpenGL and Vulkan qualification,
   including no-OpenGL-context Vulkan reachability.
4. Amend the immutable P0 closure manifest with production-decoupling evidence
   and require P0.8 acceptance before Phase 1 begins.

Pass: production code has no OpenGL-owned rendering behavior or resource
lifecycle; backend selection changes only the GHI implementation.

##### P0 dependency order

```text
P0.0 foundation -> P0.1 revalidation -> P0.2 recursive/offscreen
                                      -> P0.3 environment/water
                    P0.4 alpha --------------------------+
                    P0.5 UI/interaction -----------------+-> P0.7 confinement/qualification -> P0.8 production decoupling
                    P0.6 appearance/baking --------------+
```

P0.4, P0.5, and P0.6 may proceed in parallel after P0.0/P0.1. P0.2 to P0.3 is
the serial critical path because production water requires nested-view
reflection and exclusion resources.

### Phase 1: Unified renderer lifecycle

Phase 1 changes lifecycle and presentation ownership only. World rendering,
UI, and postprocessing remain outside this phase. Vulkan stays developer-gated
until later parity and qualification gates.

#### P1.1: Unified lifecycle contract

1. Introduce one `RendererLifecycle` owner in GHI core. It owns the selected
   `Device`, presentation surface, queues, frame contexts, completion state, and
   destruction order.
2. Replace independent surface creation with device-bound surface creation.
3. Define backend-neutral operations for acquire, acquired color target access,
   begin/end frame, submit, present, resize, suspend, display change, and
   shutdown.
4. Define asynchronous readback tickets and completion values rather than
   exposing implicit waits.
5. Define lifecycle states and transitions for uninitialized, ready, suspended,
   device-lost, shutting-down, and stopped states.
6. Specify invalidation and reconstruction semantics for device-owned handles.

Primary ownership: `indra/llrender/ghi/include/llghidevice.h`,
`llghicommand.h`, `llghipresentation.h`, `llghiproductioncontract.h`, and new
GHI core lifecycle files.

Deliverable: the validation backend exhaustively tests ordering and failure
semantics without a native graphics API.

##### Immediate development objective

The next code objective is the smallest validation-only lifecycle vertical
slice. It deliberately avoids viewer startup, native window changes, and world
rendering until ownership semantics are executable and deterministic.

Implement one `RendererLifecycle` that supports only:

```text
initialize -> acquire -> begin/end -> submit -> present -> shutdown
```

The acquired frame exposes a generation-checked backend-neutral color target
owned by the same device as the lifecycle. Resize to zero suspends acquisition;
restore creates a new target generation. Completion serials are monotonic, and
surface resources are destroyed before the device.

Acceptance tests must prove:

1. Exactly one device owns the surface and every acquired frame target.
2. A valid lifecycle produces a deterministic semantic trace.
3. Double acquire, present-before-submit, use-after-present, stale target after
   resize, and calls after shutdown fail without mutating state.
4. Zero extent suspends acquisition and restore invalidates the old generation.
5. Injected acquire failure, out-of-date surface, and device loss produce typed
   lifecycle states and preserve teardown order.
6. Completion serials increase monotonically and shutdown invalidates every
   outstanding frame handle.
7. The validation test is registered with CTest labels `ghi-contract`,
   `deterministic`, and `lifecycle`.

Only after this slice passes should P1.2 route production OpenGL through the
owner. Vulkan rendering/presentation device unification remains P1.4, after the
OpenGL compatibility checkpoint proves the contract against production startup.

#### P1.2: OpenGL production lifecycle adapter

1. Route production OpenGL creation, frame boundaries, presentation, resize,
   suspend, display change, vsync, and shutdown through `RendererLifecycle`.
2. Preserve the existing OpenGL context, provider selection, feature policy,
   settings identity, and visible output.
3. Remove direct application ownership only after equivalent lifecycle events
   are observable through the new owner.
4. Compare the controlled-frame image and lifecycle trace against the Phase 0
   baseline.

Primary ownership: OpenGL GHI backend, `indra/newview/llghiruntime.cpp`,
`llappviewer.cpp`, `llviewerwindow.cpp`, and platform window event forwarding.

Deliverable: default OpenGL behavior is unchanged, but its lifecycle is managed
through the backend-neutral owner.

#### P1.3: Backend-aware window startup

1. Resolve the requested backend and physical adapter before creating any
   render-owned resource or API context.
2. Preserve the existing Win32 no-WGL window path for Vulkan.
3. Forward resize, minimize/restore, display topology, fullscreen, DPI, and
   vsync events to the lifecycle owner.
4. Ensure explicit Vulkan startup failure is structured and never silently
   creates OpenGL. Keep `Auto` on OpenGL until Vulkan is production-eligible.

Primary ownership: `indra/newview/llappviewer.cpp`, `llviewerwindow.cpp`,
`indra/llwindow/llwindowwin32.cpp`, settings, and renderer policy code.

Deliverable: process-level smoke tests prove that OpenGL creates its expected
context and Vulkan creates no OpenGL context.

#### P1.4: Unified Vulkan rendering and presentation device

1. Refactor Vulkan presentation to consume the selected Vulkan device, physical
   adapter, graphics/present queues, allocator, completion timeline, and
   retirement domain.
2. Remove presentation-owned instance, physical device, logical device, command
   pool, and frame synchronization objects.
3. Make swapchain images available as GHI frame targets with explicit state
   transitions and ownership.
4. Implement acquire, controlled clear/draw, submit, present, out-of-date
   recreation, resize, suspend, and shutdown on the shared device.
5. Keep two bounded frames in flight and avoid `vkDeviceWaitIdle` during steady
   state.

Primary ownership: `indra/llrender/ghi/backends/vulkan/llghivulkandevice.cpp`,
`llghivulkanpresentation.cpp`, and shared Vulkan backend internals.

Deliverable: runtime assertions and validation-layer runs show exactly one
Vulkan instance/device ownership domain for rendering and presentation.

#### P1.5: Controlled production frame

1. Route one deterministic diagnostic frame through production window startup,
   GHI recording, acquired swapchain target, submission, and presentation.
2. Exercise clear and one simple pipeline-backed draw without invoking world or
   UI rendering.
3. Record semantic trace, output image/hash, frame serials, CPU/GPU timing, and
   memory observations using the Phase 0 result format.
4. Run identical lifecycle scenarios for OpenGL and Vulkan: startup, resize,
   minimize/restore, display change, fullscreen, vsync change, and shutdown.
5. Inject invalid adapter/ICD selection, acquire failure, out-of-date swapchain,
   and device-loss reporting paths.

Deliverable: both backends independently present the same controlled semantic
frame through the same frontend lifecycle, without a hybrid frame.

#### P1.6: Identity, diagnostics, and policy

1. Publish one backend-neutral renderer identity and capability snapshot before
   graphics policy is applied.
2. Route logs, Help > About, viewer statistics, recommended settings, and crash
   diagnostics through the same formatter.
3. Include adapter/API/driver identity, frame/completion serials, current debug
   scope, validation messages, and typed fault classification.
4. Preserve graphics settings by physical GPU identity across backend changes.
5. Keep Vulkan behind an explicit developer setting; do not expose the normal
   `OpenGL | Vulkan` selector in Phase 1.

Deliverable: OpenGL and Vulkan diagnostics identify the same physical GPU and
accurately distinguish API, provider, driver, and capabilities.

#### Phase 1 dependency order

```text
P0 closure -> P1.1 -> P1.2 -> P1.3 -> P1.4 -> P1.5 -> P1.6
                          |                |
                          +----------------+
```

P1.1 begins only after the immutable P0 closure manifest passes with P0.8
accepted. P1.4 must not
begin until the OpenGL adapter proves the lifecycle contract in production
startup. P1.5 requires both native adapters. Diagnostic integration in P1.6
may be developed incrementally but closes last.

#### Phase 1 migration constraints

- Exactly one active graphics device and presentation ownership domain.
- Backend selection occurs before render-owned resources are created.
- Vulkan mode creates no hidden OpenGL context.
- No hybrid production frame and no per-feature fallback between APIs.
- Explicit Vulkan failure is reported; there is no mid-session API switch.
- OpenGL remains independently releasable after every increment.
- Native API types and calls remain below their GHI backend boundaries.
- Frames in flight, asynchronous completion, and retirement remain bounded.

#### Phase 1 exit gate

Phase 1 closes only when OpenGL and Vulkan independently:

1. Select one adapter and create one rendering/presentation ownership domain.
2. Create a platform window without unintended API initialization.
3. Acquire, render, submit, and present the same controlled production frame.
4. Handle resize, minimize/restore, display change, fullscreen, vsync, and
   orderly shutdown through the shared lifecycle.
5. Publish consistent identity, validation, timing, memory, and fault evidence.
6. Pass deterministic CTest and native hardware qualification with no parallel
   renderer device, no Vulkan-session OpenGL context, and no coupling growth.
7. Preserve the Phase 0 OpenGL correctness and performance baselines within
   reviewed tolerances.

### Phase 2: Visible world foundation

Phase 2 adopts the P0-qualified I8/P0e1/P0e2 executors into visible production
ownership; it does not reimplement or qualify their semantics.

1. Connect existing opaque static and rigged PBR executors to the acquired
   production frame.
2. Connect existing terrain, sky/environment, deferred-lighting, projector, and
   shadow executors in their established pass order.
3. Replace the remaining direct GL texture, framebuffer, uniform, and
   vertex-buffer ownership at the production call sites with existing GHI
   resources and descriptors.
4. Add compressed formats, explicit resolves/blits, push constants where
   justified, and bounded asynchronous upload/readback scheduling.
5. Validate visual parity with captures and GPU-assisted validation enabled.

Exit gate: a representative world scene renders visibly through Vulkan with no
OpenGL rendering commands in migrated stages and meets agreed image tolerances.

### Phase 3: Transparency and environment completeness

Phase 3 gives visible production ownership to the P0-qualified P0e2/P0e3
executors. All environment, water, alpha, particle, timing, memory, provider,
and recovery qualification gates are already closed in P0.

1. Connect production reflection, refraction, and water-exclusion resources to
   the visible lifecycle using the P0-qualified nested-view graph.
2. Route existing legacy alpha, PPLL, depth-peel, particle, emissive, and mask
   semantics through the visible lifecycle without changing their qualified
   packet or executor behavior.
3. Do not gate on non-particle `CustomBlend` unless a future content producer is
   introduced; custom-factor particles remain the supported residual case.
4. Define deterministic ordering and synchronization for transparent passes.

Exit gate: water, particles, alpha avatars, vegetation, and mixed transparent
content pass parity, artifact, and sustained-load tests.

### Phase 4: Frame completion

1. Migrate tone mapping, exposure, glow, SSR, anti-aliasing, depth of field,
   color correction, and final compositing.
2. Adopt the P0-qualified recursive-view schedule for reflection probes, cube
   snapshots, impostors, previews, and mirrors into visible ownership.
3. Add backend-neutral picking, screenshot, and asynchronous readback paths.

Exit gate: Vulkan owns every world-rendering stage from scene submission to the
presentable image, including recursive and readback workloads.

### Phase 5: UI, media, and appearance

1. Adopt P0-qualified UI geometry, clipping, fonts, icons, HUDs, and cursors into
   visible ownership.
2. Adopt qualified media texture upload/interoperation and color-space behavior.
3. Adopt P0-qualified appearance texture compositing and avatar baking.
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