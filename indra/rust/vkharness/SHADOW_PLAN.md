# Engine Shadow System — Design Plan

Standalone Vulkan/Rust engine (`vkharness`). Greenfield shadow maps designed so the
legacy Firestorm/LL sun-shadow dropout **cannot arise by construction** — not patched.

**Kept API-neutral by design** (pure technique + math, no wgpu-isms): once proven here it
**backports to the legacy GL renderer** — conditional on M4 proving it renders consistently
and well under any condition (user, 2026-07-30). Prove it where it's safe, port it where it
ships. The earlier 3 in-place GL patches regressed for lack of a proven design; this removes
that.

## 1. The bug we are designing against (proven, not hypothesized)

Legacy sun (directional) shadows **cut out across sustained camera-angle ranges and
flicker on small pans** (on/off, not a fade). Diagnosis, confirmed on screen via
`RENDER_DEBUG_SHADOW_FRUSTA` (`r:\pictures\debug images\{Correct,Incorrect} shading frusta`):

- Cascade coverage tints the ground **everywhere** — coverage is never missing.
- The receiver picks a cascade one way (shader: receiver view-depth vs split planes).
- The caster is rendered into cascades **another** way (C++: a separate per-cascade
  view-depth *slice* cull with 0.75/1.25 overlap).
- When those two criteria disagree, the receiver samples a shadow map the **caster was
  culled from → no shadow.** The bug lives in the *seam* between them.
- Compounding it: the cascade frusta are **fitted to the camera view**, so they sweep and
  re-fit every frame → the small-pan flicker.

Every prior patch failed because it touched one side of the seam (3 projection tweaks
regressed; a min-lit-across-cascades shader tweak failed because the caster is absent from
*all* covering cascades at the bad angles).

## 2. Design invariants (each kills one failure mode)

1. **ONE cascade partition, single source of truth.** The same split boundaries decide
   *both* which cascade a receiver samples *and* which casters render into each cascade.
   Selection and inclusion cannot drift, because they read the same array.
2. **Rotation-invariant, texel-snapped cascade bounds.** Bound each cascade with a
   **sphere** (radius depends only on slice near/far + FOV → constant; orientation cannot
   change it → no sweep on pan) and **snap** the light-space origin to shadow-map texels
   (→ no edge crawl on translate).
3. **Caster inclusion extended toward the light.** Render every caster between the light
   and a cascade's volume, not just those inside the view slice → no valid caster culled.
4. **Blend band across cascade seams** (polish, once 1–3 hold).

## 3. Concrete design

### Cascade partition (the single source of truth)
- `N` cascades (default 4). Split distances over `[near, shadow_distance]` via a
  practical-split blend `d_i = lerp(uniform_i, log_i, λ)`, `λ≈0.5`. This `splits[]` array
  is THE partition — nothing else defines a cascade boundary.
- **Selection (main pass):** receiver → cascade index by its view-space depth vs `splits[]`.
- **Inclusion (shadow pass):** a caster renders into cascade `i` iff its world bounds
  intersect cascade `i`'s **caster volume** (§ below), which is derived from the *same*
  `splits[]`. ⇒ any receiver selected into `i` has all its casters present in `i`.

### Stable per-cascade frustum
1. Take the view-frustum slice corners between `splits[i-1]` and `splits[i]` (world space).
2. **Bound with a sphere** — `center` = slice midpoint, `radius` = max corner distance.
   Radius is view-orientation-independent → ortho extent is constant as the camera turns.
3. Light-space ortho: view from `-lightDir` at `center`; half-extent = `radius`.
4. **Texel snap:** `texel = 2·radius / shadow_res`; round the ortho origin's light-space
   x/y to `texel` multiples → shadow texels lock to stable world positions.

### Caster volume (toward-light extension)
- Cascade `i`'s caster volume = its bounding sphere swept along `-lightDir` toward the
  light out to `max_caster_dist` (or scene bounds). Depth-clamp ("pancake") so off-slice
  casters are captured without spending depth precision on them.

### Shader sampling
- Pick cascade from `splits[]` (identical to inclusion). Sample with **PCF**.
- **Blend band:** within a small overlap near each split, sample both adjacent cascades and
  lerp → no hard seam, robust to residual edge cases.
- **Bias:** normal-offset bias + slope-scaled depth bias (NOT a min-lit hack — that
  amplified acne in the legacy experiment). Tune to kill acne without peter-panning.

## 4. Explicitly NOT doing (learned from the dead ends)
- No separate caster cull with a different boundary than the selection splits (that *is*
  the seam).
- No min-lit-across-cascades blend (fails when caster absent from all; amplifies acne).
- No patching a view-fitted projection (widening overlap / extending far both regressed).
  This is greenfield; we replace the fitted frustum with the sphere-bounded stable one.

## 5. Milestones (each gated before the next)
- **M0 — depth-only shadow.** One ortho light, one shadow map, no cascades. *Gate:* a cube
  casts a correctly-positioned hard shadow on a plane vs a hand-computed reference.
- **M1 — PCF + normal-offset bias.** *Gate:* soft edges, no acne on a slope, no peter-panning.
- **M2 — single STABLE cascade** (sphere-bound + texel-snap the one ortho). *Gate:* slow
  camera pan → frame-diff the shadow mask → **edge does not crawl/shimmer** (sub-texel).
- **M3 — N cascades, unified partition + debug frusta overlay** (draw the frustum
  wireframes, our own `RENDER_DEBUG_SHADOW_FRUSTA`). *Gate:* frusta **do not sweep** on
  rotation; cascade transitions blend seamlessly.
- **M4 — THE continuity gate.** Scripted slow z-pan / 360° orbit around a caster (the exact
  legacy failure scenario). Measure the per-pixel shadow mask frame to frame; **assert zero
  shadowed-area discontinuity** — no cut, no flicker. This automates your spec.

- **M5 — (conditional) backport to the legacy GL renderer.** Triggered by M4 green. Port the
  *proven* design into the GL CSM (`generateSunShadow` + the shadow shaders). Because the
  design is API-neutral and already validated, this is a principled port, not the guesswork
  that regressed the 3 prior patches. Scope/timing decided when M4 lands.

## 6. Acceptance
"Done" (engine) = **M4 green**: across a full orbit and a 1°-incremental z-pan on a legacy-style
scene, shadowed area is continuous frame to frame. No cut. No flicker. By construction, not
by tuning.

## 7. Open decisions
**Provisional defaults (signed off 2026-07-30; these bind at M3, and M0–M2 don't use
cascades so they don't block starting): `N=4` cascades, `2048²` per cascade, `λ=0.5`,
SDSM deferred.** Revisit when M3 lands.
- `N` cascades, per-cascade shadow-map resolution, `shadow_distance` — final quality/perf call at M3.
- Split blend `λ` (uniform↔log).
- Later, optional: **SDSM** (fit splits to the depth histogram) for tighter cascades —
  parked; fixed practical splits first, since stability comes from sphere+snap, not the
  split scheme.
- wgpu specifics: depth format + comparison sampler for PCF (wgpu has native comparison
  samplers — good), and whether M0 reuses the existing `vkharness` render setup or a fresh
  pass.
