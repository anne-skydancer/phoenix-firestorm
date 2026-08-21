# Rejected Mesa/Zink WGL synchronization experiments

## Status

Both experiments described here are rejected. They did not eliminate visible
one-frame flashing and must not be used for Vulkanstorm release packages.

The accepted transitional runtime remains the unmodified packaged baseline:

- Mesa/Zink version: `26.3.0-devel-git.5a5f893842`
- Mesa source base: `5a5f8938423`
- Autobuild archive: `mesazink-26.3.0-devel-git-5a5f893842-windows64.tar.bz2`
- Archive SHA-1: `9a98ddcff474616c28a75ce866bb671dd09265f1`

The source base includes the accepted shader-creation null checks. The WGL
synchronization changes below are not part of that archive.

## Test environment

- Date: 2026-08-15
- GPU: AMD Radeon RX 9070 XT
- Vulkan ICD: AMD vendor ICD from Adrenalin 26.7.1
- Frontend: Gallium WGL with Zink
- Test latency: `WGL_ZINK_FRAME_LATENCY=2`

## Experiment 1: bounded WGL synchronization

Branch: `experiment/zink-wgl-bounded-sync`

The generic WGL end-of-frame wait was replaced with a bounded fence queue.
The candidate allowed one prior frame to remain in flight and waited with the
live Gallium context (`st->pipe`).

Observed result:

- Flashing frequency varied and was sometimes reduced.
- Flashing returned after longer runtimes and region changes.
- Water and other environmental rendering remained affected.
- The defect was not eliminated, so the performance result was not acceptable.

The rejected source is preserved in the Mesa repository as:

```text
stash object b7d4ae0ad4caeb2020176422cf81e1dfce4c0523
rejected-zink-wgl-bounded-sync-flashing-20260815
```

## Experiment 2: DRI-style null-context fence wait

Branch: `experiment/zink-wgl-dri-null-fence`

This candidate retained the bounded fence queue but passed a null pipe context
to `fence_finish`, matching the DRI/Kopper throttle call pattern more closely.

Observed result:

- Visible one-frame flashing remained present.
- Changing the fence-wait context did not resolve the defect.
- The candidate was rejected and must not be promoted.

The rejected source is preserved in the Mesa repository as:

```text
stash object 870b9a3e18f448275f32d21d79358bf7419c1cb7
rejected-zink-wgl-dri-null-fence-flashing-20260815
```

## Decision

Vulkanstorm packages must continue using the accepted archive identified
above. These experiments are retained only to prevent the same hypotheses from
being repeated without new evidence. Any future synchronization work requires
a reproducible capture that identifies the faulty resource lifetime or
presentation dependency before another runtime candidate is built.
