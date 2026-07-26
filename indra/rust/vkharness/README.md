# vkharness — standalone Vulkan (wgpu) rendering harness

The de-risking spike and shader testbed for the Firestorm-FSVulkan work, run
**outside** the viewer so the Vulkan bring-up carries zero viewer-integration
friction. See `indra/llrender/rhi/PLAN.md` for how this feeds the main effort.

Three jobs:

1. **De-risk the Vulkan bet** — first pixel → pipeline → mesh → textures, proven
   in isolation before any of it touches the viewer.
2. **SPIR-V / shader testbed** — ingest the viewer's real `.glsl` (from
   `indra/newview/app_settings/shaders/`) → `shaderc` (Vulkan target, *keep*
   `DescriptorSet`/`Binding`) → SPIR-V → wgpu. This is Phase 2's target-env, and
   the place we reproduce-or-kill the AMD-native GLSL "dapple" under Vulkan.
3. **Seed of `RenderBackendWGPU`** — wgpu chosen over raw `ash` so harness code
   ports directly into the viewer's `rhi_wgpu` crate. Nothing here is throwaway.

It is **not** part of the viewer's CMake build; it's its own Cargo project.

## Build & run

```
cd indra/rust/vkharness
cargo run --release
```

`F` toggles fullscreen-borderless, `Esc` quits. On start it logs the wgpu adapter
name + Vulkan driver — that line is the proof AMD's Vulkan path is live.

## Milestones

- **H0** *(here)* — window + Vulkan device + swapchain + clear-to-color (pulsing).
- **H1** — triangle via an inline pipeline (vertex buffer + shader module).
- **H2** — one real viewer `.glsl` through shaderc→SPIR-V→wgpu (fullscreen quad).
- **H3** — `softenLightF` / atmospherics with synthetic UBO inputs → the dapple,
  reproduced or refuted under Vulkan. Consumes the UBO tiering from Phase 1.
- **H4+** — mesh, textures, a mini deferred pass → real engine; code seeds `rhi_wgpu`.
