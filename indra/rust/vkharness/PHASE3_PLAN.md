# Phase 3 — the live bridge (fs_render cdylib), 2026-07-31

Phase 2 proved the draw stream crosses correctly (P2c gate 1: real sim frame rendered
from the P2b dump). Phase 3 makes it LIVE: same data path, no disk round-trip, engine
presents to the viewer's window. Evidence-driven simplifications from the real dump:

- **UI crosses the seam already** — "UI Shader" = 272 draws through the SAME
  LLVertexBuffer taps (dump census). The "UI shim" is largely the ortho pass at the
  end of the stream, not a new subsystem.
- **CPU shadow is universal** (0 of 14,863 draws skipped across both dumps) — live
  submission can reference the same mapped bytes, zero-copy at capture time.
- **Pass identity is recoverable** (program name + fbo + projection aspect), proven by
  the replay's main-camera selection.

## Mechanism: engine mode = null-GL + live taps (the Zink-toggle pattern, inverted)

The viewer already ships /DELAYLOAD:opengl32 + AddDllDirectory switching (the Zink
toggle). Engine mode points the delayload at a **null-GL stub** (nullgl\opengl32.dll:
every entry point no-op; ~30 getters return canonical caps — GL 4.6, extension string,
sane limits; queries report "visible"; fake handles count up). The viewer's entire
GL-shaped pipeline (cull → sort → LOD → batch → state calls → draw leaves) runs
UNMODIFIED and harmlessly; the three LLVertexBuffer taps submit each draw to
fs_render.dll instead of GL executing it. No RHI refactor, no GL context, one window
one API — exactly rhi/PLAN.md's bridge, live.

Known consequences to accept initially (documented, not blockers): GL-readback picking
returns misses; snapshot-to-disk is engine-side later; occlusion queries always pass
(engine does its own culling later).

## Stages

- **P3a — fs_render cdylib skeleton.** New crate `indra/rust/fs_render` (cdylib),
  C ABI: `fsr_init(hwnd, w, h)`, `fsr_begin_frame(view, proj)`, `fsr_submit(draw_desc,
  vtx_ptr, idx_ptr, tex_id, ...)`, `fsr_texture_upload(id, w, h, rgba)`,
  `fsr_end_frame()`, `fsr_shutdown()`. Internals = the replay renderer (unlit-textured,
  SoA offsets, strip expansion, pass selection by projection aspect) on a real HWND
  swapchain (H4Scene's surface path). Gate: a 50-line C test loads the DLL, feeds it a
  triangle + texture, sees it on screen.
- **P3b — viewer engine-mode toggle.** Env/setting `FS_RENDER_ENGINE=1`: switch DLL dir
  to nullgl, LoadLibrary fs_render, hand HWND after window creation, taps route to
  fsr_submit (the fsscenedump hooks generalize: dump | live | off). Texture uploads:
  hook LLImageGL's upload path to mirror RGBA to fsr_texture_upload (initial: on-first-
  bind readback impossible under nullgl — mirror at decode instead, llimagegl has the
  raw data pre-upload).
- **P3c — first light, live.** Gate: viewer boots in engine mode, world renders unlit
  + UI ortho pass on top, camera moves live, A/B restart toggle vs GL. (The Phase-3
  acceptance from rhi/PLAN.md.)
- **P3d — engine passes onto the live stream.** Wire the proven shadow cascades + sky
  + soften analog over the live draws (the H4 pipeline fed by real data). SSIM gate
  vs GL screenshots + the in-viewer HashFrame oracle. (Bleeds into Phase 4 parity.)

## Risks
- nullgl caps fidelity: viewer init reads GL strings/limits and branches — canonical
  caps table must satisfy it (iterate on real init failures; it's one C file).
- Texture mirror volume: decode-time mirroring doubles upload memory briefly; throttle
  like the TDR lesson (64/frame) if needed.
- Rigged mesh (skinning) is GPU-side in GL mode — initial live render shows rigged
  attachments at bind pose (acceptable for P3c; skinning matrices cross in P3d via
  the same UBO path the shaders already declare).
- cdylib + staticlib coexistence: fs_render is standalone (own wgpu), llrust stays
  staticlib — no co-link (the constraint that forced cdylib in the first place).
