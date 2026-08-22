# SoLoud patch provenance

All patches apply in numeric order to official SoLoud commit
`e82fd32c1f62183922f08c14c814a02b58db1873` (2024-08-13).

1. `0001-miniaudio-bound-oversized-callbacks.patch` is the original
   hardware-independent, AddressSanitizer-confirmed fix recovered from
   `soloud-contrib/fix-miniaudio-multichannel-heap-overflow.patch` dated
   2026-07-07. WASAPI can supply a callback larger than SoLoud's configured
   scratch buffer, particularly on initial 5.1/7.1 fills. The patch divides
   it into bounded `mix()` calls. The pristine repro overflowed in
   `mixBus_internal`; the patched repro was clean.
2. `0002-3d-apply-distance-attenuation-once.patch` is the original numerical
   fix from `soloud-contrib/fix-3d-distance-attenuation-applied-twice.patch`,
   also dated 2026-07-07. The pristine engine produced attenuation exponent
   2.000; the patched engine produced 1.000.
3. `0003-firestorm-miniaudio-integration.patch` preserves the integration delta
   recovered from Vulkan Storm checkpoint `e26eeab374`: output-device
   selection, native device rate/channel layout, a 40 ms scheduling period,
   Windows MMCSS registration, callback-gap telemetry, and an optional final
   mix capture. The capture path is now the value of `FS_SOLOUD_CAPTURE`, not
   the old machine-specific `C:\\fs` path.
4. `0004-firestorm-discrete-surround-panning.patch` preserves the checkpoint's
   discrete 5.1/7.1 tuning: a restrained LFE feed and squared speaker focus.
   It applies after patch 0002 so distance gain remains applied exactly once.

The viewer-side start-paused voice creation, de-click fades, wind source,
device-reinitialization safety, and stream diagnostics live in the LGPL viewer
integration (`indra/llaudio/llaudioengine_soloud.*`) rather than in this
third-party package. This separation keeps the upstream patch delta small and
auditable.
