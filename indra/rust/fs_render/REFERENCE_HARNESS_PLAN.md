# Reference harness — apples-to-apples OGL↔VLK verification

> Goal: self-verify (no SL login) that the Vulkan port (`fs_render`) reproduces the SL OpenGL pipeline,
> step by step, **headlessly** and **apples-to-apples**. The oracle is a SECOND wgpu Rust engine that
> faithfully reproduces the OGL pipeline. Canonical method throughout: (a) what does OGL do for X?
> (b) how does VLK reproduce it accurately? — see [[canonical-ogl-to-vlk-method]]. Built ON the map in
> RENDER_STRATIGRAPHY.md (every pass/RT/uniform/shader is already located).

## The two engines (one contract)
- **`fs_ogl_ref`** (NEW crate, sibling of `fs_render`) — the ORACLE. Verbatim OGL: it runs the **actual
  OGL GLSL shaders** (from `firestorm-upstream/indra/newview/app_settings/shaders/`, compiled GLSL→SPIR-V
  via the proven mechanical transforms — loose→std140 UBO, combined→separate samplers, SPIRV passthrough;
  see [[wgpu-shader-transforms]]) wired into the **real pass structure / RT layout / magnitudes** the
  stratigraphy mapped. Faithful by construction — the shipping shaders on wgpu, not a reinterpretation.
- **`fs_render`** (the port) — the clean, idiomatic wgpu engine. Must match the oracle pixel-for-pixel.

Both are headless (wgpu device, NO surface — render to an offscreen target + readback to PNG) and consume
the **same canned typed scene** (camera + EEP sky + terrain + materials + meshes + UI fixtures). Because
both are wgpu, there is NO GL-vs-Vulkan backend noise: a pixel difference is a real port divergence.

## Why this beats the alternatives
- No LLPipeline extraction (no untangling gAgent/LLWorld/LLViewerObject from the render code).
- Headless by construction (wgpu needs no window; GL needed a WGL window + had no offscreen path).
- Apples-to-apples: identical backend isolates pipeline-logic differences.
- The oracle runs the REAL shaders → trustworthy by construction, not a reproduction that could lie.

## Verify loop (per step)
1. Reproduce the step in `fs_ogl_ref` (real shaders + pass + magnitudes, from the stratigraphy).
2. Implement / confirm the step in `fs_render` (clean).
3. Run BOTH headless on the same fixtures → PNGs.
4. **Diff** (per-pixel Δ + PSNR + max-Δ; "verifiably identical" = within a tight threshold, ideally
   bit-exact where the math allows). A tiny `fsdiff` tool.
5. If identical → the VLK step is proven faithful → **retire that layer's null-GL crutch** (the viewer
   stops feeding/tapping GL for it; the engine owns it).

## Oracle trustworthiness
- Primary: runs the ACTUAL OGL shaders → faithful by construction.
- Backstop: a one-time live-session capture (an existing/occasional real OGL snapshot) cross-checks
  `fs_ogl_ref` per step; thereafter the reference is trusted headless. Minimizes logins to rare backstops,
  never routine verification.
- Gotcha ([[depth-zero-to-one-broke-shadows]]): always diff build-output SPIR-V vs source shader; naga
  PANICS on real SPIR-V → use SPIRV_PASSTHROUGH.

## Canned scene fixtures (deterministic, per step's needs)
- skeleton: clear + one fullscreen textured quad through G-buffer→resolve→present.
- atmospherics / HDR / day-cycle: sky-only + a flat lit ground (fixed EEP sky params; time-swept for
  day-cycle).
- sky/clouds/water: sky dome + a water plane fixture.
- terrain: a fixed region heightmap + detail/PBR material set.
- meshes: a synthetic static mesh + a rigged mesh + palette.
- UI: a fixed UI batch list (with scissor rects).

## Step order (yours) + what each proves
1. **skeleton** — stand up BOTH engines headless + the `fsdiff` tool; verify frame scaffolding (RT set,
   pass order, present) matches on a trivial baseline. Foundation; first real-shader diff is step 2.
2. **atmospherics** — softenLight magnitude producers (the first shader diff; the classic_mode branching).
3. **HDR** — luminance→exposure→tonemap loop.
4. **sky/clouds/water** — dome + clouds + water passes.
5. **day-cycle** — per-frame atmospherics re-eval from the slerped lightnorm (the twilight fix, measured).
6. **terrain** — legacy splat + PBR terrain (already partly built in `fs_render`).
7. **meshes** — static + rigged skinning + alpha/shadows.
8. **UI** — the 2D composite + scissor.

## First concrete tasks (step 1 = the harness itself)
- `fs_render`: headless path — `fsr_init(null hwnd, w, h)` → device with NO surface, render to an offscreen
  target, present = readback to PNG (the capture path already reads back; generalize it). Keep the HWND
  path for the live viewer.
- `fs_ogl_ref`: new crate — headless wgpu init + the RT scaffolding + a trivial pass + readback.
- `fsdiff`: PNG A/B compare (per-pixel Δ, PSNR, max-Δ, a diff image).
- A tiny driver that feeds both engines the skeleton fixture and dumps both PNGs.
