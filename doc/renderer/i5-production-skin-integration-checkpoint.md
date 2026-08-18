# I5 production skin integration checkpoint

I5 extends the private, non-presenting native Vulkan material probe from rigid
opaque PBR geometry to both rigid and rigged opaque PBR geometry. Production
world and UI rendering remain on the selected OpenGL provider.

## Skin contract

The render-agnostic packet keeps each live palette variable-size and encodes
the viewer's canonical row-major 3x4 affine matrices at 12 floats per joint.
The contract rejects empty referenced palettes and palettes above Firestorm's
110-joint production mesh ceiling.

At the backend boundary, all shader dialects use a portable std140 block with
110 `mat3x4` entries followed by a joint-count metadata vector. The consumer
pads unused entries with identity transforms and clamps indices to the live
joint count. This block is 5,296 bytes, below the 16 KiB minimum uniform-block
capacity required by OpenGL 4.1 and Vulkan.

Firestorm's legacy packed `joint + fractional weight` vertex attribute is
decoded above GHI into explicit unsigned 16-bit joint indices and four float
weights. Vertex buffers that already carry separate joint and weight
attributes use those directly. Both the capture-side budget filter and the
private consumer apply the live palette before conservative clip-volume
classification, so animated geometry is not rejected using its bind pose.

The capture compacts each draw to vertices actually referenced by its index
list. OpenGL's draw-range bounds may include unused padding or vertices owned
by another face; interpreting that entire bounding interval had incorrectly
treated an unreferenced zero-filled weight as part of a valid rigged draw.
Compaction preserves the rendered topology and lowers packet and upload cost.

Runtime verification observes normal decoder output before the selected
OpenGL provider releases its CPU copy. It neither forces a refetch nor reads
pixels back from OpenGL. Each observation is reduced to at most 64 KiB
(normally 128x128 RGBA), and the development-only 256 MiB retention window can
therefore retain roughly 4,096 RGBA textures through login and region churn.
All peer backends receive the same reduced pixels.

## Deterministic peer oracle

The material fixture addresses joint 109 explicitly while retaining the I4
nonuniform model transform, inverse-transpose normals, tangent correction and
per-map UV transformations. Native Vulkan with Khronos validation, OpenGL 4.4
and the OpenGL 4.1 fallback retain exact peer output:

1. base color: `6009f38eca5792f3affb6f08896ff588cd03ab2639dd39197efe5fccd49262ef`;
2. ORM: `d7c4959f70b881adb21212fb23f078d44d5404d741c101bdb8c7c85e62af63f1`;
3. normal: `51faf358a0eb7d9f37041f8926cded6a9fdfe26a7c6b21117c4a8b99aee3cf0b`;
4. emissive: `663b7e11df673d77c194801b17c8395b6bf09982f03eac1382926c9407457ab0`.

Every attachment covers 1,728 pixels. The deterministic shader-package hash
is `8dfdbe84c7bb7e22e5a9846e3693a2183fb1d0f6b5f1fbcace4c15dcedbc8195`.
The 32-test GHI contract suite also executes a rigid draw and a 110-joint
rigged draw in one asynchronous sample and verifies the reported rigged draw
count and maximum palette size.

## Live exit gate

System OpenGL and Mesa + Zink must each complete initial and post-teleport
samples with a rigged opaque PBR attachment visible. A passing rigged sample
reports `rigged-draws` greater than zero, a nonzero `max-joints`, nonzero
coverage in all four attachments, and no validation, synchronization,
lifetime or device-loss error. Each cell must preserve its selected visible
OpenGL provider and complete normal logout and private resource retirement.

Alpha, legacy materials, HUDs, mirrors, probes, cube snapshots, presentation
and all visible Vulkan rendering remain excluded.

## Completed Windows live gate

The production attachment exercised two rigged opaque PBR draws containing
7,566 unique referenced vertices, 41,139 indices, eight decoded texture
bindings and a 26-joint palette. Every accepted sample produced nonzero output
in all four attachments without exhausting the capture budget.

- System OpenGL passed its initial sample at frame 1,201. After a region
  teleport, samples 7 through 12 passed with the same draw, texture and joint
  counts. Post-teleport sample 7 selected 1,614 pixels in every attachment.
- Mesa + Zink was positively identified as loaded from the staged `mesa`
  directory. Its extended run passed before teleport and again after the
  destination settled. Post-teleport samples 10 and 11 retained all eight
  textures; sample 11 selected 1,685 pixels in every attachment.

Both runs kept visible world and UI rendering on their selected OpenGL
provider. The private Vulkan execution owned no surface, swapchain or
presentation path. Both viewers accepted an ordinary window-close signal,
completed normal cleanup, logged `Goodbye!`, and stopped without forced
termination.

Final verification passed the 32/32 GHI contract suite, native Vulkan with
Khronos validation, and both OpenGL shader profiles. The four deterministic
peer hashes above remained exact. The R5 material/skin package was byte
deterministic, rejected a deliberately malformed reflection contract, and was
byte-identical between the build and staged viewer. Source and staged settings
XML parsed successfully. The render-API boundary ratchet passed with no growth
in any category and three fewer direct OpenGL calls than its accepted baseline.
