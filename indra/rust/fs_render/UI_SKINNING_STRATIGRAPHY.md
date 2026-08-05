# UI Skinning Stratigraphy — the stock UI shader/color/texture contract ↔ fs_render ↔ gap

> Follows the U0 method: excavate the WHOLE stock UI-rendering path (every shader program, every
> color/texture route, the texture-upload mechanism), map it, then transpose faithfully — not patch one
> symptom. Traced by three read-only passes over `c:/fs/firestorm-upstream` + direct reads. Citations are
> `file:line` under `indra/` unless noted.

Symptom that triggered this: a toast box that should be semi-transparent dark grey renders **solid white**,
and scroll bars skin wrong. The excavation shows the root cause is a whole missing shader PROGRAM, not a
one-off.

---

## 0. The two tap feeds (corrected model)

The engine receives the UI through **two independent channels**, not one:

1. **Draws** — `LLRender::flush()` hook (`llrender/llrender.cpp:1833-1850`) → `FSSceneDump::uiSubmit` →
   `fsr_ui_submit`. Captures per-vertex `mColorsp` + `getTexUnit(0)->mCurrTexture` + MVP + (U2/U3) clip +
   blend.
2. **Textures** — a **null-GL stub** `indra/newview/nullgl/opengl32.c` that REPLACES `opengl32.dll` in
   engine mode. It intercepts `glTexImage2D`/`glTexSubImage2D` and forwards pixels to
   `fsr_texture_upload`/`fsr_texture_subupload`, **keyed by the GL texture name** (`opengl32.c:546,572`).
   Source-agnostic: every real-pixel level-0 ≤4096² RGBA-convertible upload goes through it. **Local skin
   PNG/TGA DO reach the engine** (`LLViewerFetchedTexture → LLImageGL::createGLTexture → setManualImage →
   glTexImage2D (llimagegl.cpp:1508) → stub`). **Correction:** `FSSceneDump::textureUploaded` (the hook I
   had reasoned from) is **dead code — zero callers**; the live path is the stub.

So a UI draw's texture is present in the engine by GL name; a missing texture falls back to the engine's
1×1 white. Edge cases the stub flags (future S2): async-decode-not-yet-complete; the **"PERMANENT WHITE"**
path when a GL name is deleted then re-minted so its dims are vacated (`opengl32.c:611`);
`glTexSubImage2D`-built textures whose `glTexImage2D` re-spec dims weren't registered; formats not handled
by `to_rgba8_tight`; `glCompressedTexImage2D` not exported (UI disables compression, so skin images avoid
this).

---

## 1. Exactly TWO UI shader programs (the whole 2D UI)

No `llui`/widget code binds its own shader; everything renders with whatever `LLGLSLShader` is bound. Only
two are ever bound in the 2D UI (agent-confirmed; all others in `interface/` are ruled out — bound only in
3D/tool/post paths). Both exist only in `class1` (no class2/3 override).

### gUIProgram — the master UI program ✅ (reproduced)
- Bound once around the whole widget tree (`newview/llviewerwindow.cpp:3016`, unbind `:3193`) + in
  `render_ui_2d` (`llviewerdisplay.cpp:2176`). Files `interface/uiV.glsl` + `uiF.glsl`.
- `uiV.glsl`: `in vec4 diffuse_color; ... vertex_color = diffuse_color;` — PER-VERTEX color (`MAP_COLOR`).
- `uiF.glsl`: **`frag_color = vertex_color * texture(diffuseMap, uv);`** — component-wise RGBA multiply.
- fs_render's `shaders/ui.frag` is exactly this. Untextured draws bind white → `frag = vertex_color`.

### gSolidColorProgram — the solid-color program ❌ (THE GAP)
- Bound transiently inside `gl_draw_scaled_image_with_border` when `solid_color==true`
  (`llrender/llrender2dutils.cpp:376`, restored to gUIProgram `:703`). Files `interface/solidcolorV.glsl`
  + `solidcolorF.glsl`.
- `solidcolorV.glsl`: `in vec3 position; in vec2 texcoord0;` — **NO `diffuse_color` attribute.**
- `solidcolorF.glsl`:
  ```glsl
  uniform sampler2D tex0;
  uniform vec4 color;
  void main() {
      float alpha = texture(tex0, vary_texcoord0.xy).a * color.a;
      frag_color = vec4(color.rgb, alpha);   // RGB from the UNIFORM; texture = ALPHA MASK ONLY
  }
  ```
- Color source = the **`DIFFUSE_COLOR` uniform** ("color"), NOT per-vertex.

---

## 2. The color router — why gSolidColorProgram taps white

`LLRender::color4ub` (`llrender/llrender.cpp:2115-2125`) routes the SAME `gGL.color4fv()` call by the bound
shader's attribute mask:
```cpp
if (!sCurBoundShaderPtr || sCurBoundShaderPtr->mAttributeMask & LLVertexBuffer::MAP_COLOR)
    mColorsp[mCount] = LLColor4U(r,g,b,a);   // gUIProgram -> per-vertex (captured by the tap)
else
    diffuseColor4ub(r,g,b,a);                // gSolidColorProgram -> DIFFUSE_COLOR uniform (DROPPED by the tap)
```
`diffuseColor4ub` → `shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, ...)` (`llrender.cpp:2209-2217`), which
caches the value in `LLGLSLShader::mValue` (`llglslshader.h:308`, written `llglslshader.cpp:1474`). So the
current uniform color is **readable at flush with NO glGet.**

**Result:** a `drawSolid` batch never writes `mColorsp`; its per-vertex color is stale/white; the tap ships
white; the engine's `gUIProgram`-only shader renders `white * texture`. Since the SolidColor texture is a
white-RGB alpha-mask shape, the box comes out **solid white** — exactly the symptom.

---

## 3. Where gSolidColorProgram is used (the blast radius)

`drawSolid`/`drawBorder` (both → `gl_draw_scaled_image_with_border(solid_color=true)` → gSolidColorProgram)
are pervasive across widgets — every one currently renders white/untinted:
- **LLScrollbar** track + thumb-hover glow (`llui/llscrollbar.cpp:536,551,558,572`) — the user's scroll-bar report.
- **LLButton** focus border + glow (`llbutton.cpp:921,949,960,1112-1122`).
- **LLBadge** icon + border (`llbadge.cpp:337,341`).
- **LLConsole** background (`llconsole.cpp:340,432`).
- **LLMultiSlider / LLSlider** thumbs + borders (`llmultislider.cpp:759…863`, `llslider.cpp:340`).
- Any `LLUIImage::drawSolid()`/`drawBorder()` caller.

**NOT this path (already correct, per-vertex gUIProgram):** `gl_rect_2d`, `gl_draw_scaled_image`,
`LLUIImage::draw()`, lines/circles/drop-shadow, **font text**, and **LLPanel/LLFloater backgrounds**
(`llpanel.cpp:216` `mBgOpaqueImage->draw(...)`, `llfloater.cpp:2199`). So the toast **panel background** is
per-vertex — if it's still white after S1, that's a texture-path case (§0 edge cases), tracked as S2.

---

## Pinned roots
1. **Solid-white skinned boxes + wrong scroll bars / button glow** = the `gSolidColorProgram` program is not
   reproduced: its color rides the `DIFFUSE_COLOR` uniform (tap-dropped) and its fragment is
   `vec4(color.rgb, texel.a·color.a)` (texture = alpha mask), not `v_color·texel`.
2. **Toast panel background (per-vertex path)** is a *separate* question — if S1 doesn't resolve it, it's a
   texture-upload edge case in the null-GL stub (S2), not the color path.

---

## Transposition plan

- **S1 — gSolidColorProgram (the definite, complete color-path fix).**
  - *Tap* (`fsscenedump` uiSubmit + uiSubmitBufferData): when the bound shader lacks `MAP_COLOR` (i.e.
    `sCurBoundShaderPtr == &gSolidColorProgram`), read the `DIFFUSE_COLOR` value from `mValue` and **write it
    into the tapped per-vertex color** (overwriting the stale white), and set a `solid_color` flag.
  - *ABI*: `fsr_ui_submit` gains one `flags: u32` param (bit0 = solid_color). One more small, coordinated
    widening (lockstep engine + viewer branches), same pattern as clip+blend.
  - *Engine*: add `shaders/ui_solid.frag` = `out = vec4(v_color.rgb, texel.a * v_color.a)`; `UiDraw` gains
    `solid: bool`; the pipeline cache keys on it (or a parallel pipeline); the UI pass selects it.
  - *Pin (headless)*: a solid_color draw with color (r,g,b,a) over a shape-alpha texture must yield
    `rgb = color.rgb`, `a = texel.a * color.a` — NOT `color * texel` (the gUIProgram result). Then in-world:
    scroll bars, button glow, badges, console skin correctly.
- **S2 — toast/texture-path integrity (conditional).** Only if the toast panel background is still white
  after S1: trace the null-GL stub (`opengl32.c`) for the async-decode / PERMANENT-WHITE / re-spec-dims
  cases against the specific skin texture. Do NOT pre-implement; verify in-world first.

**Verification stays login-free for S1** (headless pin reproduces the solidcolor fragment exactly), then
in-world for the blast-radius widgets. S2 is in-world-driven by construction.

---

## North-star: retire the nullGL bridge (standing directive)

The nullGL bridge is a crutch to be **retired fully**. Two interception layers: (1) the **nullGL stub**
(`newview/nullgl/opengl32.c`) faking GL + intercepting `glTexImage2D` → `fsr_texture_upload` (wrap-the-
calls); (2) the `fsscenedump` `LLRender::flush` UI tap + typed scene feeds (bridge-the-data). **Rule:** once
an element is FULLY VALIDATED in-world, migrate it OFF the tap to a **native typed** path, element by
element, until the stub can be deleted. The *dead* `FSSceneDump::textureUploaded` hook is the intended
native texture path to resurrect (wire it at `LLImageGL::setManualImage`, esp. BOOST_UI/FTT_LOCAL_FILE skin
textures; have the stub skip what's covered). Do not pile onto the stub; migrate outward. Post-S1: schedule
the confirmed UI elements (U1 color, U2 scissor, U3 blend, U4 filter, S1 solidcolor) for native migration.
S1 itself already uses the typed `fsscenedump` feed (not the stub), so it's on the right side of the line.
