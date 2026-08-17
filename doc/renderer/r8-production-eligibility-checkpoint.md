# R8 interaction and production-eligibility checkpoint

Date: 2026-08-17  
Branch: `render/ghi-r8-production-eligibility`

R8 adds the final renderer-facing contracts needed before production
qualification. It does not route production world or UI rendering away from
OpenGL, expose Vulkan in the production selector, change the default backend,
or retire Mesa + Zink.

## OpenGL target decision

The primary packaged OpenGL shader target for Windows and Linux is OpenGL 4.4,
not OpenGL 4.6. OpenGL 4.4 supplies the direct-GL requirement used by the
current PPLL implementation (`glClearTexImage`) while avoiding an unnecessary
4.6 floor. The packaged OpenGL 4.1 profile remains the macOS artifact and the
explicit compatibility fallback. Historical evidence that records an actual
AMD OpenGL 4.6 driver context remains factual; it no longer names the packaged
shader target.

The target profile, manifest key, package directory, GLSL version declaration,
pipeline-cache identity, capability threshold, native fixture labels, and
documentation all use the OpenGL 4.4 contract. Shader packages remain byte
deterministic after the profile change.

## R8 interaction fixture

The shared 64 x 64 fixture exercises one capability-first workload through the
OpenGL and Vulkan peers:

- an opaque background followed by source-over UI and HUD overlays;
- three explicit scissor regions and independent per-attachment blend state;
- an RGBA8 snapshot target, R32UInt selection-ID target, and R32Float pick-depth
  target;
- exact ordering samples for background, ordinary UI, and HUD selection;
- complete snapshot, selection, and pick-depth readback; and
- a backend timestamp interval around the render work.

HUD alpha remains on the legacy alpha route. The fixture does not move HUD
content into PPLL or depth peeling.

Exact peer references are:

| Output | SHA-256 |
|---|---|
| snapshot | `fa447dfa6cc02e12a93098086b57ef4ba64024b62e7aaafcb447f69f5cddf0cd` |
| selection IDs | `fd3304ed7606bde9eaa118143f4333f9907ad721ca5ed73cfb2606351421c9ea` |
| pick depth | `89be3f6f79d4bb0ef7325b995f5fbf9ec10c9d73499e7addaef2256c3efa0be2` |

All peers select exactly 1,920 pixels. OpenGL 4.4 and the OpenGL 4.1 fallback
match Vulkan bit-for-bit.

## Fault and eligibility policy

`DeviceFaultReport` converts the backend-neutral status vocabulary into
retryable, device-recreation-required, or fatal reports while preserving the
backend, operation, frame serial, and diagnostic detail. A `DeviceLost` status
always requests device recreation; it is not silently converted into a backend
switch.

Production eligibility is an evidence policy with fixed gates for R00-R14,
required ledger parity, Windows coverage, Linux coverage, performance,
renderer-boundary integrity, device-loss reporting, and content-heavy region
entry. Passing every evidence gate still does not expose Vulkan: a separate
explicit production approval is required. Any pending or failed gate makes
production selection false.

## Local verification

- GHI contract suite: **29/29 pass**.
- R8 shader package: deterministic and reflection-mismatch rejection pass.
- Vulkan interaction stress: **120/120 pass** with Khronos validation and
  synchronization validation; all three exact hashes remain stable. The
  largest recorded timestamp interval was 6,480 ns on this run.
- OpenGL 4.4 interaction stress: **120/120 pass**; all three exact hashes remain
  stable. The largest recorded timestamp interval was 2,960 ns on this run.
- OpenGL 4.1 fallback: exact peer pass for all three outputs.

The Vulkan loader emitted only the already-known duplicate AMD switchable-
graphics and OBS layer notices; no API, synchronization, layout, descriptor,
or lifetime validation error was reported.

## Eligibility state

This branch is an R8 engineering candidate, not a production-approved Vulkan
renderer. The following release evidence remains open:

- required baseline-ledger rows are not all recorded as `PARITY`;
- the Linux hardware matrix is not captured;
- broader Windows hardware coverage and performance qualification are not
  complete;
- a native device-loss recovery exercise is not captured;
- content-heavy live region-entry qualification is not captured through a
  production Vulkan route; and
- explicit production approval has not been granted.

Those gaps are intentionally represented as pending evidence rather than
inferred from a synthetic fixture. Until they close and a separate release
decision is made, production world/UI/offscreen rendering remains OpenGL and
Mesa + Zink remains available.
