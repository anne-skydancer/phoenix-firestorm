# I6 production terrain integration checkpoint

I6 adds a private, non-presenting native Vulkan execution path for real
production terrain observed from the main deferred world view. The selected
OpenGL provider continues to render the visible world, UI, and presentation.

## Terrain contract

The backend-neutral packet carries multiple regions in one frame so region
borders and teleport hand-off do not collapse distinct terrain state. Each
region records:

- legacy height/noise or PBR paint-map composition mode;
- planar or triplanar projection;
- four terrain layers with material factors, KHR texture transforms, and
  base, normal, metallic-roughness, and emissive texture references;
- the composition texture, region scale, detail scale, and exact resource
  identity.

Geometry is copied from the viewer's existing CPU-mapped post-cull terrain
buffers into a canonical 48-byte vertex. Only vertices referenced by the
draw's indices are retained. No OpenGL object name, Vulkan handle, capability
URL, credential, or GPU readback crosses the packet boundary.

Texture pixels are observed at the normal decoder/upload boundary and reduced
to at most 64 KiB per image for the developer probe. Observation begins when
the opt-in setting is loaded, before the native Vulkan device is created, so
preloaded resources such as the terrain alpha ramp remain available without
an OpenGL readback. A resource epoch changes when texture content,
comparability, region material state, projection, paint mode, or terrain
override changes; ordinary camera motion changes only the scene epoch.

## Native execution

`RenderVulkanTerrainOffscreenProbe` opts into I6 and requires
`RenderVulkanDeveloperProbe`. Each asynchronous sample is bounded to 16 draws,
131,072 vertices, 393,216 indices, 80 image bindings, 16 MiB total upload, and
4 MiB per image. It owns four 256x256 color attachments and a private reverse-Z
depth attachment. It owns no surface, swapchain, window, or presentation path.

The first shader slice executes the production four-layer base-color
composition for both height/noise and PBR paint-map weights. It implements
per-layer transforms and planar/triplanar sampling, then emits base color,
factor-derived ORM, geometry normal, and factor-derived emissive outputs.

Normal, metallic-roughness, occlusion, and emissive texture references are
already represented and captured, but sampling those maps is deliberately not
claimed by I6. Lighting, shadows, sky, water, alpha, HUDs, mirrors, hero probes,
cube snapshots, impostors, and presentation remain later integration slices.

## Deterministic gate

The 33-test GHI contract suite validates terrain packet round trips and
rejection limits, loads the packaged Vulkan shader, and executes a synthetic
PBR triplanar terrain draw through the real native Vulkan device. All 33 tests
pass. The deterministic I6 package hash is
`7471733c9e50f87d475ea18495dec1b9468bf7c5bb5a831a46e79a06c61e8a95`.

Source and staged settings XML parse successfully. The renderer API-boundary
ratchet passes with zero direct Vulkan calls or Vulkan types above the backend
boundary and three fewer direct OpenGL calls than the accepted baseline.

## Live exit gate

System OpenGL and Mesa + Zink must each produce initial and post-teleport I6
samples from visible production terrain. A pass requires real terrain draws,
nonzero output in all four attachments, clean Vulkan validation, no device
loss, correct resource-epoch turnover, normal logout, and clean private
resource retirement.

Mesa + Zink passed 21 consecutive initial samples with 16 PBR terrain draws
selected from 64 captured draws, 16,150 vertices, 75,780 indices, 11 texture
resources, and nonzero output in all four attachments. This scene exercised
planar PBR terrain. After teleport, three consecutive samples passed on legacy
terrain with 14,388 vertices, 67,278 indices, five texture resources, and new,
stable attachment hashes. The hand-off packet represented two regions. Normal
window close retired the private Vulkan resources and completed `Goodbye!`.

System OpenGL was positively identified as loaded from System32 and passed its
initial legacy-terrain samples with 15,058 vertices, 70,380 indices, five
texture resources, and stable nonzero attachment hashes. Scene epochs advanced
for each captured frame while the unchanged resource set correctly remained at
resource epoch 1. A second run passed 90 consecutive PBR-terrain samples with
16,150 vertices, 75,780 indices, 11 texture resources, and stable nonzero
attachment hashes before teleport. The hand-off to legacy terrain changed the
selected draw mode from 16 PBR draws to zero, advanced the resource epoch from
1 through the transient arrival states to a stable epoch 4, and settled at
14,388 vertices, 67,278 indices, five texture resources, and new stable hashes
in all four attachments. No Vulkan validation or device-loss error occurred.
Normal window close retired the private Vulkan resources and completed
`Goodbye!`.
