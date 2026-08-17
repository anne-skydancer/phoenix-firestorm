# I4 material-transform integration checkpoint

I4 removes the temporary transform coupling used by I3 while keeping all
visible rendering on the selected OpenGL provider. Native Vulkan continues to
execute only private, non-presenting attachments.

## Transform contract

The R5 material shader package now has three independent transform domains:

- group 0 carries the camera view-projection matrix;
- group 1 binding 0 carries the object model matrix and its CPU-derived
  inverse-transpose normal matrix; and
- group 1 binding 1 retains the separate synthetic four-joint skin block until
  I5 introduces bounded production palettes.

Rigid draws therefore no longer replicate their model matrix into four skin
matrices. Singular object transforms are rejected before submission. Normals
use the inverse-transpose transform, while tangents use the linear model
transform and are re-orthogonalized against the transformed normal.

The material block carries independent offset, scale and precomputed
cosine/sine rotation values for base-color, normal, ORM and emissive maps. All
shader dialects apply the glTF transform order:

`offset + rotation(scale * uv)`

The live capture path accepts nonidentity transforms on UV0. Other texture
coordinate sets remain outside this checkpoint.

## Deterministic peer oracle

The shared material fixture combines nonuniform object scale, translation,
inverse-transpose normal handling, tangent re-orthogonalization, synthetic
four-joint skinning and four distinct texture transforms. Native Vulkan with
Khronos validation, OpenGL 4.4 and the OpenGL 4.1 fallback produce 1,728 shaded
pixels in every attachment and the same hashes:

1. base color: `6009f38eca5792f3affb6f08896ff588cd03ab2639dd39197efe5fccd49262ef`;
2. ORM: `d7c4959f70b881adb21212fb23f078d44d5404d741c101bdb8c7c85e62af63f1`;
3. normal: `51faf358a0eb7d9f37041f8926cded6a9fdfe26a7c6b21117c4a8b99aee3cf0b`;
4. emissive: `663b7e11df673d77c194801b17c8395b6bf09982f03eac1382926c9407457ab0`.

The deterministic shader package SHA-256 is
`107fcf8a2cfb596d2beebb5b0529de9ecfdbac0296afe316a147b6db4e4a9993`.
The GHI contract suite remains 32/32 and now verifies the separate object/skin
binding group, the expanded 176-byte material block, exact transformed packet
round-trip, executable nonidentity transforms and singular-transform
rejection.

## Live exit gate

System OpenGL and Mesa + Zink must each complete an initial and post-teleport
sample with:

- at least one `uv-transformed-draws` observation;
- nonzero coverage in all four attachments;
- no Vulkan validation, synchronization, lifetime or device-loss error;
- unchanged visible OpenGL provider identity; and
- clean logout and Vulkan resource retirement.

Zero-coverage completions are diagnostic retries and do not count as passing
samples. Runtime capture performs the same conservative clip-volume rejection
as the private consumer before a draw can consume the bounded geometry and
texture budget. A real packet inspection confirmed the column-major
`viewProjection * model * position` contract: 30 of 39 rigid opaque PBR draws
in the diagnostic scene were potentially visible. Moving this filter ahead of
the 32-draw capture limit prevented clipped, texture-complete draws from
starving later visible work.

The completed Windows live gate used Isle of Repose for transformed PBR
content and Birch Hill as the settle/return destination:

- System OpenGL initially passed with 30 captured visible draws, two executed
  draws, one UV-transformed draw and 197 non-clear pixels in every attachment.
  After the Birch Hill round trip, sample 31 passed again with the same
  transformed-draw and coverage counts.
- Mesa + Zink initially passed with 30 captured visible draws, two executed
  draws, one UV-transformed draw and 197 non-clear pixels in every attachment.
  After the same round trip, sample 13 passed with five executed draws, three
  UV-transformed draws and 39 non-clear pixels in every attachment.

Both cells kept visible rendering on their selected OpenGL provider, reported
no validation, synchronization, lifetime or device-loss error, and completed
normal viewer cleanup. The final contract suite passed 32/32; native Vulkan
with Khronos validation and both OpenGL shader profiles reproduced the peer
oracle hashes above. Source and staged R4/R5 shader packages were byte-exact,
the render-API boundary ratchet passed, and source/staged settings XML parsed
successfully.

Alpha, legacy materials, rigged production skinning, HUDs, mirrors, probes,
cube snapshots, presentation and all visible Vulkan rendering remain excluded.
I5 owns variable-size live skin palettes and rigged opaque PBR execution.
