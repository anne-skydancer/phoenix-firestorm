# Vulkan Storm JPEG 2000 distribution policy

Vulkan Storm has two deliberately separate build modes. `PUBLIC` is the safe
default. It uses OpenJPEG and is the only mode intended to produce publicly
available artifacts. `PRIVATE` is an explicit developer mode; it may use Grok,
and every packaged artifact made in this mode receives a
`VULKAN_STORM_PRIVATE_BUILD.txt` marker.

This is a project release policy and a technical safety boundary, not a legal
opinion. A public release still requires the normal license, notice, source,
security, and reproducibility review described below.

## Supported selections

| Distribution | JPEG 2000 backend | Configuration | Packaging result |
|---|---|---|---|
| Public | OpenJPEG 2.5.4 | `--public` (default) | Allowed by the build policy |
| Private/non-public | OpenJPEG 2.5.4 | `-DVULKANSTORM_DISTRIBUTION=PRIVATE -DUSE_GROK=OFF` | Marked private/non-public |
| Private/non-public | Grok from a local `GROK_ROOT` | `--private-grok` or `-DVULKANSTORM_DISTRIBUTION=PRIVATE -DUSE_GROK=ON` | Marked private/non-public |
| Any | Kakadu | Not supported | Configuration is rejected |

`--grok` is intentionally rejected by `configure_firestorm.sh`; the explicit
`--private-grok` spelling prevents an old or copied command from silently
producing a Grok-enabled artifact without acknowledging its status.

The CMake boundary rejects `USE_GROK=ON` unless
`VULKANSTORM_DISTRIBUTION=PRIVATE`. The viewer manifest independently repeats
the check before copying or packaging runtime files, so bypassing one layer
does not create a public Grok package.

## Public build

The normal public configuration is:

```sh
bash ../scripts/configure_firestorm.sh --config --public [other options]
```

The `ReleaseFS_open` Autobuild configuration passes `--public` explicitly.
OpenJPEG is pinned in `autobuild.xml` to the official Second Life
`3p-openjpeg` `v2.5.4-r1` packages (`2.5.4.18754730947`) for Windows, Linux,
and macOS. This supersedes the 2.5.3 package affected by CVE-2025-54874.

## Private Grok development build

Grok is not downloaded or published by the viewer Autobuild manifest. A
developer must provide a separately built local checkout and opt in:

```sh
export GROK_ROOT=/absolute/path/to/grok
bash ../scripts/configure_firestorm.sh --config --private-grok [other options]
```

The equivalent direct CMake settings are:

```text
-DVULKANSTORM_DISTRIBUTION=PRIVATE
-DUSE_GROK=ON
-DGROK_ROOT=/absolute/path/to/grok
```

Grok is licensed AGPL-3.0-only upstream. Vulkan Storm source is distributed
under its repository license (LGPL-2.1), while OpenJPEG is BSD-2-Clause. The
project does not approve Grok-enabled artifacts for public distribution; the
private setting and marker do not themselves establish license compliance.

## Public-release checklist

Before publishing a public Vulkan Storm binary:

1. Configure from a clean checkout with `VULKANSTORM_DISTRIBUTION=PUBLIC`,
   `USE_GROK=OFF`, and `USE_KDU=OFF`; retain the complete configure log and
   `CMakeCache.txt`.
2. Confirm `packages-info.txt` names OpenJPEG 2.5.4 and that the assembled tree
   contains the platform OpenJPEG runtime but no Grok or Kakadu runtime.
3. Retain `autobuild.xml`, the exact source revision, dependency archives or
   their immutable URLs and hashes, toolchain versions, build commands, and all
   applied patches needed to reproduce the binary.
4. Include the viewer source/license material and all third-party notices,
   including the OpenJPEG BSD notice, required for the exact conveyed binary.
5. Run the JPEG 2000 texture corpus tests and platform smoke tests, then review
   current OpenJPEG and transitive-dependency security advisories at release
   time.
6. Perform a final license and Third Party Viewer Policy review for the exact
   artifact. Passing the automated boundary is necessary but is not proof of
   public-release compliance.
