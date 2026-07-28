# H3 — uniform → std140-UBO → SPIR-V transform proof (standalone vkharness)

**Status:** plan settled 2026-07-28, no code yet. Implement in `indra/rust/vkharness`
(its own Cargo project, outside the viewer build). Seeds `rhi_wgpu`.

## 1. What H3 proves — and explicitly does not

**Proves:** the transform `{tier-tagged loose uniforms → std140 UBO → shaderc
SPIR-V (Vulkan target) → wgpu/Vulkan}` is **behavior-preserving** on real viewer
shaders, validated **mechanically**. This is the de-risk the viewer-side UBO redo
is *not allowed to proceed without*.

**Why it must exist (the burn):** the prior viewer-side attempt migrated a single
uniform (`sun_up_factor`) to a FrameUBO — it **built green, linked clean, loaded
without error, and still rendered wrong** (day/night lighting broke) → reverted
(`pre-reset-backup` `6f1b172026` / `5cac95a3a5`). "It compiled" proved nothing.
H3 is the proving ground that catches that exact failure class **in isolation,
where it cannot break the viewer's shadows.**

**Does NOT:** reproduce the "dapple." That was decoupled on evidence — the source
shader was never localized (softenLight was only a *chosen test vehicle*,
llshadermgr.cpp:480 comment), AMD driver **26.7 fixed the broad case**, and a
GL-front-end bug cannot appear under Vulkan anyway (Zink already renders clean).
H3 is shader-agnostic transform work; the dapple is a separate, driver-fixed
footnote with a residual content-specific tail (appliers / dark skins / older
mesh bodies) that gets its own native-GL-vs-Zink diff *if ever* chased.

## 2. Validation definition (locked)

**"Byte-exact" = behavioral** (per the user): *do what the GLSL does, but in
SPIR-V.* The transform may change the **plumbing** (where uniforms come from, what
IR runs) but never the **computation**.

- **Structural divergence = FAIL.** Wrong value delivered, wrong branch, wrong
  math → the shader stopped doing what the GLSL does. (This is the FrameUBO burn;
  the gate must catch it dead.)
- **Sub-ULP FP drift between toolchains = PASS.** FMA contraction / reassociation
  is floating point, not different behavior. Any drift found is logged as a
  *finding* (it localizes where FP diverges — useful for the full port), not a
  failure.

**Gate = `memcmp` of the rendered framebuffer against a trusted reference.**

**The oracle = Zink** (for the complex case): Zink runs the viewer's loose-uniform
GLSL by translating GLSL→SPIR-V→Vulkan, correctly (no dapple, it's Vulkan). Our
harness runs loose→UBO→SPIR-V→wgpu/Vulkan. Same backend ⇒ **Vulkan-vs-Vulkan**,
no GL-vs-Vulkan FP confound ⇒ byte-exact is genuinely achievable, with residual
sub-ULP drift inside the PASS tolerance. Same bit-exact-oracle discipline that
carried rust-j2c, with Zink playing the role C++-Grok played there.

## 3. Measured landscape (survey 2026-07-28 — plan is data-grounded, not assumed)

- **665 value-uniform decls → 225 distinct.** Heavy head (top 11 names = 42% of
  all decls, overwhelmingly matrices) + long tail (143 names in exactly 1 shader).
- **4 existing std140 UBOs** (GLTF ×3, ReflectionProbes). Everything else loose.
- **181 sampler decls = a SEPARATE descriptor track** (not UBOs).
- **Tiers are real** in the frequency data: transform (matrices) / frame-global /
  lights (11 array uniforms) / material-tail.
- **softenLight full surface** (incl. shadowUtil + deferredUtil + reflectionProbe):
  ~13 distinct textures + ~40 value uniforms. The heaviest shader — great tier
  coverage, expensive input rig.

## 4. Staging (mechanism first — because softenLight's rig is a project)

### H3a — mechanism proof (small, known-answer, no oracle)
A minimal fragment shader: 2-5 value uniforms (spanning types: `float`, `int`,
`vec3`, `mat4`), ≤1 texture, output **hand-computable** from the inputs. Transform
its uniforms → one std140 UBO, `shaderc`→SPIR-V (Vulkan target, **keep**
DescriptorSet/Binding — opposite of the GL strip), wgpu pipeline with the UBO,
render 1×1 or small target, read back, **compare to the arithmetic answer**.
- Proves: UBO **value delivery is correct** (kills the burn class cheaply),
  SPIR-V compiles + binds, wgpu UBO pipeline works, std140 layout/offsets right.
- No Zink, no synthetic G-buffer. This is the rigorous, cheap foundation.

### H3b — the sun keystone (softenLight, Zink golden)
The tier-spanning proof on the real heavy shader.
- Build a **deterministic synthetic-input generator**: G-buffer (4 rects w/ fixed
  PBR-plausible data), 6 shadow maps, reflection-probe cube arrays, `lightMap`/
  `lightFunc`, and the ~40 uniform values — all seeded, reproducible, **shared**
  between harness and reference capture (the key design risk, §6).
- Transform **all** softenLight value-uniforms into tiered UBOs (FrameUBO /
  local block / shadow-array block); samplers → descriptors. `shaderc`→SPIR-V →
  wgpu render.
- Reference: **Zink golden** — softenLight rendered via correct GL-on-Vulkan on
  the *same* synthetic inputs, captured once. `memcmp`.
- Proves the transform across frame-global + per-frame + local + shadow-array
  tiers on a genuine engine shader.

### H3c — transform tier (tiny geometry shader)
softenLight is fullscreen → **no `modelview_projection_matrix`**, and that's the
single biggest tier by decl count. One minimal object shader (a triangle with the
transform matrices) proves the **TransformUBO ring-buffer** pattern H3b can't
reach. Small; closes the last tier.

## 5. Tier → UBO mapping (what the harness proves, that the viewer sweep generalizes)

| tier | examples | UBO strategy | lesson |
|---|---|---|---|
| frame-global | `sun_dir`,`sun_up_factor`,`screen_res`,`sky_hdr_scale` | one **FrameUBO**, filled by **per-frame gather** | NOT setter-interception (the burn) |
| per-draw transform | `modelview*`,`projection`,`normal_matrix` | **TransformUBO** ring buffer | H3c |
| per-material / local tail | the 143 single-file names | per-shader **local block** | mechanical-but-individual |
| lights / arrays | `light_*[]`,`shadow_matrix[6]`,`matrixPalette[]` | light block / special | array std140 stride care |
| samplers (181) | all `sampler*` | **descriptor sets** — separate track | not a UBO problem |

Prove one representative of each tier → the rest of the tier is a probe-tagged,
mechanical sweep in the viewer (that's `fs/uniforms` → `fs/ubos`).

## 6. Open design risks (flagged, resolved at implementation)

1. **H3b input-sharing:** the deterministic synthetic-input generator must feed
   harness and Zink-reference **byte-identical** inputs, or the memcmp is
   meaningless. Likely a seeded generator emitting raw texel/uniform buffers both
   sides load. Solve before H3b render, not during.
2. **GLSL→SPIR-V route in-harness:** H2 used `shaderc`. Confirm the UBO-rewritten
   GLSL compiles clean for Vulkan target with DescriptorSet/Binding **kept**
   (the GL-target strip does NOT apply here — Vulkan needs them).
3. **FP tolerance:** aim byte-exact; if a structural-looking delta appears,
   bisect (FMA? precision qualifier? layout offset?) before declaring pass/fail.

## 7. Fences

No viewer integration. No dapple. No perf work. One shader at a time. `wgpu` not
`ash` (ports straight into `rhi_wgpu`). Nothing here is throwaway.

## 8. Definition of done

H3a known-answer exact · H3b softenLight ≡ Zink golden (structural) · H3c
transform-tier proven. Then: the tier patterns are validated in isolation and the
viewer-side `fs/uniforms`/`fs/ubos` sweep may begin — the thing the prior attempt
had no right to do without.
