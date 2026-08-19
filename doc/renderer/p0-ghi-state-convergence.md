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
