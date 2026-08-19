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
live acceptance gate requires the opaque stream plus all four G-buffer targets
to contain work.

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
