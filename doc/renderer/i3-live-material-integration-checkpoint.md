# I3 live rigid-material integration checkpoint

I3 extends the isolated native Vulkan offscreen path from R4 legacy geometry
to real post-cull PBR material submissions. Production world, recursive,
alpha, HUD, impostor and UI rendering remains OpenGL.

## R5b2 geometry contract

R5b observed exact material, decoded-texture and skin resources but did not
carry the vertex/index ranges or transforms required to execute a production
draw. R5b2 closes that seam without correlating independently ordered packet
streams:

- material packet version 2 carries source extent, a canonical 76-byte vertex,
  32-bit indices, draw spans, view-projection transforms and separate model
  transforms;
- version 1 resource-observation captures remain decodable;
- semantic-only records remain valid with a zero index count; and
- packet validation rejects invalid material/skin references, vertex indices
  and draw spans before any GPU resource is created.

The automated R5b2 test round-trips geometry and both transforms exactly,
executes an indexed material draw on the validation backend, verifies retained
draw/vertex/index counts and resource retirement, and exercises both invalid
index and invalid-span rejection.

## Live scope and safety limits

`RenderVulkanMaterialOffscreenProbe` opts into I3 and requires
`RenderVulkanDeveloperProbe`. It consumes only the main-view deferred
`PASS_GLTF_PBR` class when all of the following hold:

- the draw is rigid and opaque;
- the vertex buffer supplies position, normal, tangent and UV0;
- every referenced texture has comparable decoded CPU data;
- all bindings use UV0 and identity texture transforms; and
- the packet remains within 32 draws, 65,536 vertices, 196,608 indices,
  64 sampled images, 4 MiB per decoded image and 16 MiB total upload data.

Absent material maps use semantic 1x1 defaults. One sample may be pending.
Submission does not wait; later OpenGL frames poll four private 256 by 256
G-buffer attachments. The normal texture decoder retains or refetches bounded
CPU observations without reading texture data back from OpenGL. Runtime
observations are downsampled to at most 1 MiB per image and held in a 64 MiB
LRU cache; the material packet contract continues to accept images up to its
4 MiB per-image ceiling. Capture prefilters nonidentity UV transforms and packs
the largest complete draw prefix that fits the 16 MiB upload budget, so a later
oversized draw cannot discard an otherwise executable sample. Failed or empty
samples are retried at the configured interval, and device loss disables I3
without disturbing the production renderer.

The following remain deliberately outside I3: alpha-mask and alpha-blend,
rigged skinning, nonidentity texture transforms, legacy materials, particles,
HUDs, impostors, mirrors, hero probes, cube snapshots, pre-water alpha, all
presentation, and all visible Vulkan rendering.

## Automated exit gate

1. The GHI contract suite passes **32/32**, including R5b2 geometry execution.
2. The Release viewer links as `vulkanstorm-bin.exe`.
3. Both R4 and R5 runtime shader packages are staged under
   `app_settings/ghi_shaders`.
4. The renderer API-boundary ratchet remains within its accepted baseline.
5. Source and staged settings XML parse successfully.

## Windows live exit gate

Run System OpenGL and Mesa + Zink production-provider cells with Khronos
validation enabled. Each cell must show at least one completed I3 sample after
settling and one after teleport, four attachment hashes with nonzero coverage,
no Vulkan validation/device/synchronization/lifetime error, unchanged visible
OpenGL provider identity, and clean logout/resource retirement.

The Windows live gate passed on 17 August 2026 on an AMD Radeon RX 9070 XT:

- System OpenGL completed stable initial samples and, after the
  Isle of Repose -> Birch Hill -> Isle of Repose teleport cycle, repeatedly
  rendered five draws, 252 vertices, 318 indices and 20 textures. All four
  attachments contained 163 non-clear pixels and reported no capture-budget
  limitation.
- Mesa + Zink completed stable initial samples with six draws, 252 vertices,
  318 indices and 24 textures. After the same teleport cycle it rendered five
  draws with all four attachments populated and no capture-budget limitation.
- Neither cell reported Vulkan validation, device-loss, synchronization or
  lifetime errors, and visible rendering remained on its selected OpenGL
  provider throughout.

Both provider cells therefore pass I3. This checkpoint does not make native
Vulkan selectable for production rendering.
