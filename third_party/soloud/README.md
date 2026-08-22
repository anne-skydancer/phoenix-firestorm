# Vulkan Storm SoLoud Autobuild package

This directory is the reproducible source recipe for the SoLoud library used
by Vulkan Storm. It does not vendor an unexplained source snapshot.

- Upstream: `https://github.com/jarikomppa/soloud.git`
- Pinned commit: `e82fd32c1f62183922f08c14c814a02b58db1873`
  (`Flush to zero (FTZ) for ARM`, 2024-08-13)
- Package revision: `fsvs1`
- License: zlib/libpng (the upstream `LICENSE` is staged in every package)

`build-cmd.sh` checks out that exact commit, applies every patch in numeric
order, builds only the core, WAV, miniaudio, and nosound sources consumed by
the viewer, and stages an Autobuild package. Windows produces Release and
Debug static libraries; Linux produces a Release static library. Both stage
the public SoLoud headers and the pinned miniaudio header.

The patch provenance and validation history are documented in
`patches/PROVENANCE.md`. The first two patches are the original independently
reproducible fixes recovered from the July 2026 Vulkan Storm work. The last two
preserve the viewer integration and discrete-surround behavior that the
current `llaudioengine_soloud.cpp` consumes.

Local Windows package build:

```sh
autobuild -p windows64 -A 64 build
autobuild -p windows64 -A 64 package
```

The resulting archive is intentionally ignored by Git. Until a reviewed
package is published on the project package host, the viewer's local
`autobuild.xml` entry points at the archive in this directory. Publishing is a
separate release action and is not performed by this recipe.
