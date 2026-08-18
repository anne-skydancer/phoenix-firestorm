# Native Vulkan UI integration plan

This plan adds Firestorm's UI to the native Vulkan peer without rewriting the
LLUI widget system and without constructing a mixed OpenGL/Vulkan production
frame. Until the complete Vulkan frame is production-eligible, visible world,
HUD, UI, and presentation remain on the selected OpenGL provider.

## Requirements and constraints

The existing widget tree, XML layouts, skins, input dispatch, focus, tooltips,
accessibility behavior, and CPU-side hit testing remain frontend policy. The
renderer must preserve:

- `render_ui_3d()` ordering for HUD attachments, selections, name tags, and
  world-space UI;
- `render_ui_2d()` and `LLViewerWindow::draw()` ordering for the root view,
  floaters, menus, notifications, debug text, and cursor overlays;
- nested transforms, clip/scissor rectangles, source-over blending, texture
  addressing, line widths, and display scaling;
- FreeType glyph layout, fallback fonts, emoji, font atlas updates, shadows,
  bold/italic behavior, and resolution-generation invalidation;
- CPU UI hit testing before world picking, plus transparent/rigged/HUD world
  pick policy when the UI does not consume the event;
- snapshot modes that include or exclude HUD and UI independently; and
- all shipped Firestorm skins at supported DPI and UI scales.

The UI path must not expose Vulkan handles to LLUI or add Vulkan-shaped methods
to `LLRender`, `LLFontGL`, `LLImageGL`, or `LLVertexBuffer`. It must not import
an OpenGL texture, framebuffer, fence, or shadow map as a hidden shortcut.

## Component boundary and data flow

```text
LLUI tree / LLView / LLFontGL / HUD traversal
                    |
                    v
        backend-neutral UI encoder
     batches + scissor + transforms + resources
                    |
                    v
       GHI UI compositor and pick contracts
              /                 \
      OpenGL GHI peer       Vulkan GHI peer
              \                 /
        final world + HUD + UI target
                    |
          selected API presentation
```

LLUI continues to decide *what* is drawn. The UI encoder records only immutable
rendering semantics:

- canonical 2D vertices and indices;
- content-addressed image/font-atlas references;
- orthographic transform and logical-to-physical scale;
- integer scissor rectangle;
- source-over, additive, opaque, or mask blend intent;
- display-referred color-space intent;
- stable layer/order and semantic label; and
- optional selection ID and pick depth.

The encoder is frame-local and bounded. It owns no native resources and cannot
submit or present. Both GHI peers consume the same packet.

## Color and composition decision

The native world renderer produces scene-linear/HDR color. World-space HUD
geometry is rendered with its existing depth and legacy-alpha policy before
the final display transform. Ordinary 2D UI is then composited in the
display-referred target after world tone mapping, matching current visual
intent and preventing UI colors from being altered by exposure or atmospheric
processing.

UI texture formats declare sRGB or linear intent explicitly. Blending occurs
in linearized attachment space; the presentation target performs the final
sRGB encoding. A reference grid will cover translucent panels, text edges,
icons, color swatches, and fullbright HUD content to catch double-gamma and
premultiplied-alpha mistakes.

## Integration increments

### UI0 — observation and semantic trace

Observe the real `render_ui_3d()` and `render_ui_2d()` traversal without
changing output. Record canonical batch, transform, scissor, resource, and
ordering hashes. Establish bounds for batches, vertices, glyphs, atlas updates,
and scissor changes across the shipped skins.

Exit gate: the trace is stable when UI state is stable, changes for real UI
changes, contains no native handles, and does not affect OpenGL output.

### UI1 — 2D primitives and scissor stack

Route rectangles, lines, textured quads, nine-slice panels, gradients, and
nested clipping through a private offscreen GHI target. Preserve physical-pixel
rounding at noninteger UI scales.

Exit gate: OpenGL 4.1, OpenGL 4.4, and Vulkan produce matching reference
regions for nested floaters, menus, scroll panels, and tooltips.

### UI2 — images, fonts, and dynamic atlas updates

Give UI images and font atlases backend-neutral image handles. Keep glyph
rasterization and fallback selection on the CPU; upload dirty atlas rectangles
as asynchronous GHI transfers. Use generation-tagged handles so DPI/font/skin
changes cannot sample retired atlas storage.

Exit gate: multilingual text, emoji, shadows, style variants, caret/selection,
and live DPI/scale changes pass without stalls or missing glyphs.

### UI3 — 3D HUD and ordering

Treat HUD attachments as a named view class, not as ordinary 2D widgets. Reuse
the production geometry/material packets with the HUD camera and established
legacy-alpha routing. PPLL and depth peeling remain excluded for HUD content.
Selection outlines, name tags, beacons, and build overlays retain their current
relative order.

Exit gate: HUD alpha/fullbright behavior and world/HUD/UI ordering match the
OpenGL references, including attached animated HUDs.

### UI4 — picking, selection, and readback

Keep CPU widget hit testing first. For world/HUD picks, render explicit
`R32UInt` selection IDs and `R32Float` pick depth and service results through a
bounded asynchronous readback queue. A synchronous compatibility path is
allowed only for interactions that cannot tolerate a one-frame result; it must
wait for the relevant completion value rather than the whole device.

Exit gate: transparent, rigged, HUD, parcel, reflection-probe, and surface-info
picks match current behavior. Device loss completes or cancels every callback
exactly once.

### UI5 — snapshots and final compositor

Express world-only, world+HUD, and world+HUD+UI snapshots as explicit graph
outputs. The final compositor consumes only native-GHI images from the selected
backend. Vulkan presentation is still developer-gated at this step.

Exit gate: snapshot dimensions, alpha, color space, tiling/high-resolution
paths, and UI inclusion policy match OpenGL without a full-device idle.

### UI6 — production-frame and presentation gate

Connect the complete Vulkan world, recursive/offscreen, alpha, HUD, UI, and
compositor graph to the existing Vulkan surface/swapchain lifecycle. No pass in
the displayed frame may fall back to OpenGL. Minimize/restore, resize, display
scale, window mode, and clean shutdown must pass before production approval.

Exit gate: native Vulkan can own the complete viewer session, but the selector
remains developer-only until the separate hardware, performance, region-entry,
device-loss, and release-approval gates pass.

## Reliability and performance budgets

- Two bounded frames in flight; no steady-state `waitIdle`.
- Frame-local UI vertex/index arenas and descriptor reuse.
- Dirty-rectangle atlas uploads rather than full-atlas replacement.
- One ordered batch stream with conservative adjacent-batch merging; never
  reorder across scissor, blend, target, or semantic-layer boundaries.
- Asynchronous snapshot and pick readback with explicit queue limits.
- Per-frame telemetry for batch count, vertices, atlas bytes, pipeline changes,
  scissor changes, upload bytes, pick latency, and GPU duration.
- A UI-only GPU target of no more than 1 ms at 2560x1440 on the primary Windows
  qualification system, revisited after representative live measurements.

## Trade-offs

- Recording immutable UI batches costs CPU memory and one traversal boundary,
  but avoids contaminating LLUI with backend state and enables deterministic
  peer comparison.
- Keeping FreeType rasterization on the CPU preserves layout and platform
  behavior; GPU glyph generation would add substantial risk without helping
  the Vulkan seam.
- Display-referred UI composition best preserves current appearance, but
  requires an explicit world-tone-map boundary and careful sRGB attachment
  selection.
- Asynchronous picking avoids renderer-wide stalls, while a narrowly scoped
  completion wait may still be necessary for a few immediate interactions.

## Qualification matrix

The live gate covers System OpenGL, Mesa + Zink, and native Vulkan on Windows,
then native OpenGL and Vulkan on Linux. It includes every shipped skin, 96/120/
144/192 DPI, UI scales 0.75/1.0/1.25/1.5/2.0, window resize and minimize,
inventory and outfit panels, chat/emoji, menus/tooltips, build tools, media
controls, HUD attachments, snapshots, and picking. Intel and macOS Vulkan are
not production requirements; macOS retains its OpenGL 4.1 path.

## Revisit points

Revisit bindless/descriptor-indexed UI images only after live descriptor churn
is measured. Revisit glyph-atlas page size after multilingual and emoji traces.
Revisit batch sorting only if recorded CPU/GPU timings show a material benefit;
semantic ordering remains authoritative. Revisit the 1 ms budget using the
Windows and Linux hardware qualification results.
