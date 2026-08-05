# UI Stratigraphy — stock LLRender/LLUI ↔ fs_render native-VK UI ↔ gaps

> Method (canonical OGL→VLK): for every visible difference, (a) *what does stock GL/LLUI do for X?*
> (b) *how does native-VK reproduce it accurately?* Stock behavior is the invariant spec, not a design
> choice. Verify login-free against a UI oracle that replays the SAME tapped draw stream under the stock
> LLRender state contract, exactly as `fs_ogl_ref` does for the 3D strata. No cherry-picking: the color
> stratum touches the whole present column, not just the UI pass.

All `file:line` citations are under `c:/fs/firestorm-upstream/indra/` (stock) or
`c:/fs/fs-vulkan-engine/indra/rust/fs_render/` (engine) unless noted.

---

## 0. The honest feed (what the engine already receives)

The native-VK UI is **not** re-implemented LLUI — it is a faithful renderer of the immediate-mode draw
stream that stock LLUI already produces. The skin is consumed **stock-side** and baked into the feed:

- `LLRender::flush()` (`llrender/llrender.cpp:1829-1850`), gated by `FSSceneDump::uiActive()`, re-packs the
  still-valid immediate striders into a 24-byte interleaved vertex `{pos vec3 (12B) | uv vec2 (8B) |
  color u8x4 (4B)}` and hands over `mvp = projection*modelview` (column-major), the bound tex id, `mMode`,
  and count → `fsr_ui_submit` (`fs_render/src/lib.rs:775`).
- The cached-font retained path (`LLFontVertexBuffer::renderBuffers`) bypasses `flush()` and is captured
  separately by `uiSubmitBufferData` (`llrender/fsscenedump.cpp:287-312`), which reads the SoA buffer
  (pos vec4a/16, tc vec2/8, color rgba/4), repacks to the SAME 24-byte form + current gGL mvp, and forwards
  through the SAME `sFsrUiSubmit`. → **cached button labels / list / editor text are covered.**
- Resolved skin colors arrive **in the vertex color stream**; skin images/glyph atlases arrive via
  `fsr_texture_upload` + `tex_id` (`fsscenedump.cpp:314`); `UIScaleFactor` is baked into positions
  (`llrender.cpp:1992-2002`, `mUIOffset/mUIScale` applied at `vertex3f`) and the mvp.

**Consequence for "settings consumed / skin":** the engine does **not** re-parse `colors.xml` (456 colors,
`LLUIColorTable`), `textures.xml` (961 images, `LLUIImageList`, 9-slice already resolved into positions/uv),
or `fonts.xml` (`LLFontGL`, glyphs already rasterized into the tapped atlas). Visual equivalence "including
skin" = **rendering the tapped feed faithfully.** The only settings that reach the *engine* render path are
**gamma/color-space** (Stratum A) and **UIScaleFactor** (baked into mvp; also the scissor scale — Stratum B).

`UiDraw` today (`src/live.rs:309-315`): `{ mvp[16], tex_id, first_vertex, vertex_count, line }` — **no clip,
no blend.** That is the shape the strata below must widen.

---

## Stratum A — Color space (STRUCTURAL, pervasive) — the headline

**Stock spec.** UI is drawn in a 2D ortho pass **after** `renderFinalize()` tonemaps the 3D scene to the
sRGB-encoded default framebuffer (`llviewerdisplay.cpp:1880`), then `render_ui_2d()` with `gUIProgram`.
There is **no sRGB/gamma conversion anywhere in the UI/font path** (grep-confirmed absent in
`llfontgl.cpp`, `llfontfreetype.cpp`, `llrender2dutils.cpp`): `GL_FRAMEBUFFER_SRGB` is **disabled** for UI,
so texel bytes are sampled **raw** (no decode), vertex color = **raw sRGB bytes**, `uiF.glsl` =
`vertex_color * texel`, blended with `GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA` **in sRGB (non-linear) space**,
written raw. This matches the historical look; AA fringes and drop-shadow translucency depend on the blend
being in sRGB space.

**fs_render current.** Swapchain = first **sRGB** surface format (`Bgra8UnormSrgb`, `lib.rs:163-168`, chosen
so the 3D tonemap's *linear* output auto-encodes). Every uploaded UI texture (images **and** the font atlas)
is created `Rgba8UnormSrgb` (`live.rs:1211`; white `:704`) → **sRGB-decoded on sample**. Vertex color
`Unorm8x4` → normalized, treated as **linear**. Blend on an sRGB target happens in **linear** space, then
linear→sRGB **auto-encode** on store (`ensure_ui` blend `live.rs:3776-3778`; UI pass `:4283-4299`).

**Gap.** A full linear round-trip (decode texel · treat color as linear · blend linear · re-encode) where
stock does raw-sRGB passthrough + **sRGB-space** blend. Bites **every translucent UI pixel**: panel/hovertext
backgrounds, AA glyph edges, soft drop-shadows, tinted icons. A mid-gray vertex color `0x80` (→0.5, treated
linear, re-encoded) lands ~0.735 — visibly **lighter** than stock's raw 0.5.

**Measured gap (U0 oracle, `fs_ogl_ref --example ui_ab`).** Translucent mid-gray (byte 128, α 128) over
gray bg (byte 64): stock = **96** (analytic, sRGB-space `128·0.502+64·0.498`); engine-current = **144**
(linear round-trip) → **Δ 48/255** on *every* such pixel. Translucent textured mid-gray image: Δ 6 (the
texture-decode sub-part; opaque untinted images round-trip cleanly — decode+encode cancel).

**Intervention (whole-column, structural).**
1. Swapchain → `Bgra8Unorm` (non-sRGB).
2. Move the linear→sRGB encode into the **tonemap/present shader** (the 3D scene must land byte-identical —
   it previously leaned on the swapchain to encode; now the shader encodes explicitly). Verify against the
   existing sky/ground offscreen present (headless present target is already sRGB for readback — decouple:
   headless keeps its sRGB readback target, live present goes UNORM + shader-encode, OR headless also encodes
   in-shader — pick one so oracle and live agree).
3. Uploaded textures get `view_formats: [Rgba8Unorm, Rgba8UnormSrgb]`; the **UI pass binds the raw
   `Rgba8Unorm` view** (no decode), the **3D pass keeps the `Rgba8UnormSrgb` view** (albedo still linearizes
   for lighting — that decode is *correct* for 3D and must be preserved).
4. `ui.frag` writes `v_color * texel` **raw** to the UNORM target → blend in sRGB space, matching stock.

This is the stratum that most needs the no-cherry-pick discipline: it cannot be fixed inside the UI pass
alone — the swapchain-format flip forces the 3D present path to own its sRGB encode.

---

## Stratum B — Clipping / scissor (STRUCTURAL, functional)

**Stock spec.** Nested clipping is a **GL scissor stack**, not part of the LLRender cached feed
(`lllocalcliprect.cpp`): `pushClipRect` **intersects** with the stack top (`:57-72`); `updateScissorRegion`
does `gGL.flush()` **first** (drains batched verts under the old scissor), then `glScissor` from the
stack-top rect **scaled by `LLUI::getScaleFactor()`**, `llfloor` origin + `llceil(...)+1` extent
(`:81-97`). `LLLocalClipRect` converts a local rect to screen space via `LLFontGL::sCurOrigin`.

**fs_render current.** **No scissor anywhere.** `UiDraw` has no clip field; the tap does not capture the
scissor (agent-confirmed: "scissor is NOT in the LLRender feed"); the UI pass sets none.

**Gap.** Minimap escapes its panel; scroll-list / text-editor / combobox-dropdown overflow is unclipped;
floater content spills its frame. **Functional breakage, not just cosmetic.**

**Measured gap (U0 oracle).** Full-screen white panel clipped to a center rect: stock outside = **64**
(clipped to bg), engine-current outside = **255** (unclipped) → **Δ 191/255** in the whole should-be-clipped
region (mean Δ 143 over the frame).

**Intervention.** (a) Widen the C++ tap: at flush time capture the current scissor rect + enable from the
llui clip stack (`LLScreenClipRect::sClipRectStack`) — the `flush()`-on-scissor-change ordering means the
clip is already correct **per flushed draw**, so one clip per `UiDraw` is exact. (b) `UiDraw` gains a clip
rect (+ enabled bool). (c) UI pass calls `up.set_scissor_rect(...)` per draw, applying the **same UI-scale +
llfloor/llceil+1** rule so pixel coverage matches stock.

---

## Stratum C — Blend modes (functional)

**Stock spec.** `setSceneBlendType` → 7 modes (`llrender.cpp:1428-1457`, factor table `:105-119`):
`BT_ALPHA` (default) `SRC_ALPHA/1-SRC_ALPHA`; `BT_ADD` `ONE/ONE`; `BT_ADD_WITH_ALPHA` `SRC_ALPHA/ONE`;
`BT_MULT` `DST_COLOR/ZERO`; `BT_MULT_ALPHA` `DST_ALPHA/ZERO`; `BT_MULT_X2` `DST_COLOR/SRC_COLOR`;
`BT_REPLACE` `ONE/ZERO`. Readable cheaply via `getCurrBlendSFactor/DFactor` (`llrender.h:409-410`).

**fs_render current.** One hard-coded straight-alpha pipeline; the tap's `mode` param is **geom topology
only** (used solely to expand strips/fans/lines → lists, `ui_submit` `live.rs:3664-3674`); blend type is not
captured.

**Gap.** Additive UI (glow, some selection highlights, certain overlays) and multiply UI composite wrong.

**Measured gap (U0 oracle).** Additive glow (byte 128, α 128) over bg 64: stock ADD (`ONE/ONE`) = **192**;
engine-current (forced alpha) = **144** → **Δ 48/255**.

**Intervention.** (a) Tap captures `getCurrBlendSFactor/DFactor` at flush; (b) `fsr_ui_submit` carries the
src/dst factors; (c) `UiDraw` gains a blend key; (d) UI pass selects from a small pipeline cache keyed by
(src,dst) factor pair. Coordinate the ABI widening with Stratum B (one `fsr_ui_submit` signature bump for
clip+blend, not two).

---

## Stratum D — Per-surface sampler filter (fidelity)

**Stock spec.** Font glyph atlas = `TFO_POINT` → **NEAREST** min+mag, no mips (`llfontbitmapcache.cpp:129`);
glyph screen origins are `ll_round`'d to whole pixels (`llfontgl.cpp:363-367`), so nearest is a 1:1 texel
map — that is *why* UI text is crisp. UI images loaded via `LLUIImageList` set **`TAM_CLAMP`** + compression
off (`llviewertexturelist.cpp:1844-1847`); general images default linear+mips+aniso (`llimagegl.cpp:568-569`).

**fs_render current.** Single sampler, **LINEAR** min+mag, `ClampToEdge`, `mipmap Nearest`
(`ensure_ui` `live.rs:3751-3760`).

**Gap.** Fonts are LINEAR-sampled → soft/blurry text vs stock's crisp NEAREST glyphs. (Clamp address mode is
already right for UI.)

**Measured gap (U0 oracle).** 2-texel atlas (transparent | opaque white) sampled across: endpoints agree
(64 / 255 under clamp), but the glyph edge diverges **Δ 125/255** — stock's hard NEAREST step vs the engine's
LINEAR ramp.

**Intervention.** Two samplers (NEAREST for font-atlas draws, LINEAR for image draws). The engine must know
which a draw is: carry a per-texture filter option at upload (stock knows each surface's `mFilterOption`),
or a per-draw font/image hint from the tap (the `uiSubmitBufferData`/`LLFontGL::render` paths are already the
font paths). UI pass picks the sampler per draw's texture.

---

## Already-satisfied invariants — DO NOT touch (would regress)

- **Pixel snapping (E):** stock `ll_round`s 9-slice edges and glyph origins to whole device pixels
  **in software before emit** (`vertexBatchPreTransformed`), so the tapped positions arrive **already
  snapped**. The engine must **not** re-snap; the UI ortho + full-swapchain viewport map them 1:1.
- **Depth/state (F):** UI is depth-test off, cull off, fill (`LLGLSUIDefault`, `llglstates.h:83-95`).
  Engine UI pass: `depth_stencil: None`, `cull_mode: None`. Matches.
- **Painter's order (G):** stock reverse child iteration → back-to-front (`llview.cpp:1300-1302`); the tap
  captures draws in that order; the engine iterates `ui_draws` in submission order. Matches.
- **Cached-font path (H):** `uiSubmitBufferData` forwards to `fsr_ui_submit`. Covered.
- **Vertex format (I):** 24-byte interleaved pos/uv/color u8x4 both sides. Matches
  (`live.rs:3771-3774` ↔ `fsscenedump.cpp:301-309`).

**Minor / deferred:** line width > 1px (`setLineWidth`, scaled by UI scale) — most UI lines are 1px; wgpu
caps line width at 1 on most backends. Defer to the in-world residual pass.

---

## Pinned roots (holistic root causes, chosen from the map)

1. **UI washed-out / translucency wrong** = Stratum A: sRGB swapchain + sRGB-decoded textures + linear-treated
   vertex color → full linear round-trip vs stock's raw-sRGB + sRGB-space blend. Root, not tuning.
2. **Minimap escapes window / list overflow** = Stratum B: no scissor plumbed from the llui clip stack.
3. **Additive/mult UI wrong** = Stratum C: blend type never captured (tap forwards geom mode only).
4. **Blurry text** = Stratum D: fonts LINEAR-sampled; stock is NEAREST 1:1 on `ll_round`'d origins.

---

## Phased plan (login-free oracle-first, structural-first)

- **U0 — UI oracle harness** ✅ DONE (`fs_ogl_ref/src/ui_pass.rs` + `examples/ui_ab.rs`, headless, no SL
  login). Renders the fixtures (translucent panel, additive glow, 2-texel font atlas, center-clipped panel,
  translucent textured image) under the **stock contract** and the **engine-current** state, using the REAL
  `ui.vert/ui.frag` (loaded by path — zero shader drift). Asserts the stock side == analytic sRGB-space blend
  math (the reference is pinned), and **measures** each stratum's gap: A Δ48, B Δ191, C Δ48, D Δ125, tex Δ6.
  `UiState` parameterizes all four axes, so U1-U4 each already have their fixture. The **engine-side**
  convergence pins (driving the real `LiveRenderer`) land in `fs_render/tests/headless.rs` per phase.
- **U1 — Color-space stratum (A):** swapchain → `Bgra8Unorm`; sRGB-encode moved into the tonemap/present
  shader; uploaded textures dual-view (UI binds raw `Rgba8Unorm`, 3D keeps sRGB); `ui.frag` writes raw.
  Assert UI-oracle color-exact for panel + text + image; assert 3D scene byte-unchanged (sky/ground oracles
  + offscreen present).
- **U2 — Clipping/scissor (B):** tap captures the clip rect at flush; ABI widen; `UiDraw` clip; UI pass
  `set_scissor_rect` with UI-scale + llfloor/llceil+1. Assert nested-clip fixture.
- **U3 — Blend modes (C):** tap captures src/dst factors; same ABI bump as U2; pipeline cache by blend key.
  Assert additive + multiply fixtures.
- **U4 — Per-surface sampler filter (D):** per-texture filter carried at upload/draw; nearest for fonts,
  linear for images. Assert text-crispness (nearest 1:1) + image fixtures.
- **U5 — In-world parity pass** (needs a user SL launch): real login screen + populated UI; A/B screenshot
  vs stock; chase residuals (line width, cursor, chiclets/toasts, minimap now clipped, login tonality) —
  the Cat B backlog.

**ABI note.** U2 + U3 widen the `fsr_ui_submit` signature (clip rect + blend factors). Do it **once** as a
single coordinated tap + header (`fsscenedump.{h,cpp}`) + `lib.rs` FFI + `UiDraw` change, not two bumps.

**Scope open for sign-off:** whether U5 is in this pass or deferred; whether U0's reference is a new
`fs_ogl_ref` example or a dedicated `fs_ui_ref` crate; whether the ABI widening is acceptable now (it touches
the C++ tap the viewer builds against).
