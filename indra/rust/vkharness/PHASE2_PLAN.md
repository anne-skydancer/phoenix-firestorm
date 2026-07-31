# Phase 2 — bounded gaps + the data bridge (execution plan, 2026-07-31)

Follows rhi/PLAN.md (Phase 1 gate MET: 458/458, commit a0d3bb3cd3). Order chosen so each
step retires the currently-largest risk first. Invariant holds throughout: **no in-viewer
render surgery** — viewer changes are dev-only instrumentation (logging/dump), gated, on
their own branch, GL path untouched.

## A) Bounded gaps (close before the bridge)

### A1 — config matrix (kills single-point-config risk) — CHEAP, FIRST
Sweep the corpus under multiple canonical configs, not just one:
1. `canonical` (shadows2+SSAO+probes3, SSR off, AMD/switch) — the proven baseline
2. `ssr-on` (SSR=1, screenSpaceReflUtil at level 3)
3. `probes-off` (REFMAP_LEVEL/REF_SAMPLE_COUNT absent, reflectionProbeF level 2,
   hasReflectionProbes=false table-wide — mirrors `mShaderLevel>2`→false? NO: keep table
   features fixed, this config only changes GLOBALS; record divergence)
4. `emissive-on` (HAS_EMISSIVE=1)
5. `shadows-off` (no SUN_SHADOW/SPOT_SHADOW; HAS_SUN_SHADOW perms stay per-table)
6. `nvidia` (if-chain diffuseLookup, no IS_AMD_CARD)
Implement: `Config` struct {globals overrides, nvidia: bool, amd: bool} threaded through
assemble(); `cargo run -- sweep` runs all configs; scoreboard per config.
Gate: all configs 458/458 (or documented expected-fail classes).

### A2 — corpus-wide separate-sampler split (sizes the wgpu-consumption work)
Add split mode to the sweep (second compile per TU): combined decl → texture+sampler pair
(H3b3d transform) PLUS the new part: rewrite FUNCTION SIGNATURES taking sampler-typed
params (`float f(sampler2DShadow m, ...)` → `(texture2D m_tex, samplerShadow m_smp, ...)`)
and their call sites/uses (`texture(m, x)` → constructor at point of use inside the body via
the same #define trick per-parameter, or textual param-name rewrite). Measure first: count
TUs with sampler-typed params (expect: the shadowUtil/reflectionProbe family). Gate:
458/458 in split mode. This is the wgpu-ready corpus.

## B) Phase 2 proper — the data bridge

### P2a — viewer createShader dump + table diff (kills transcription risk) — FIRST viewer touch
Branch `fs/scene-dump` off master (viewer). Dev-only, setting-gated (`FSDumpShaderManifest`):
in `LLGLSLShader::createShader()` (llglslshader.cpp:407), on success, append JSON line
{mName, mShaderFiles[(path,stage)], mShaderLevel, mFeatures (all bools+channels), mDefines}
to `shader_manifest.jsonl`. Run viewer once (canonical settings), quit. vkharness
`table-diff <manifest.jsonl>`: compare vs sweep_table (name-matched: files+order, level,
features, defines). Gate: zero unexplained diffs; fix table where wrong; re-run sweep.

### P2b — scene dump (the bridge, offline half)
Same branch, setting-gated (`FSDumpSceneFrame`): one-frame capture at the tap sites:
- `LLVertexBuffer::drawRange/drawRangeFast/drawArrays` (llvertexbuffer.cpp:963/970/989):
  serialize {vertex bytes, typemask, index bytes, draw params}
- current program (mName), bound textures per channel (TexUnit), modelview/projection
  matrices (gGLModelView etc.), pass/pool tag (gPipeline current pass — nearest available
  label), render-target id, blend/depth state snapshot
- format: one .fsdump dir: header.json + blobs (dedup identical buffers by hash)
Trigger via debug setting; dumps exactly ONE frame then auto-clears.

### P2c — vkharness replay (`cargo run -- replay <dump>`)
Load the dump; map typemask→VertexBufferLayout (known mapping); render the draw stream
through a minimal replay renderer: start UNLIT-TEXTURED (albedo only, correct transforms)
→ then wire the proven engine passes (shadow/soften analog) incrementally. Output PNG.
Gate 1: recognizable sim frame (geometry+textures correct).
Gate 2: screenshot-diff vs the GL frame captured at the same instant (SSIM + masked
per-pass diff; normalize AA/dither off for capture). Zink same-box capture = optional
Vulkan-vs-Vulkan cross-oracle.

## Order of execution
A1 → P2a (needs a viewer build; can overlap A2) → A2 → P2b → P2c.
Commits: one per lettered step, on fs/shader-sweep (harness) / fs/scene-dump (viewer).

## Risks
- P2a/P2b need a viewer BUILD+RUN (user in the loop; viewer must be closed for link).
- Dump volume: one frame of a busy scene ≈ tens of MB — hash-dedup buffers, accept size.
- Pass labels: gPipeline has no clean "current pass" string everywhere — nearest label +
  render-target id is enough for replay bucketing; refine during P2c.
- Replay renderer is NOT the final engine integration — it's the parity oracle. Don't
  gold-plate it; it exists to prove the data crosses correctly.
