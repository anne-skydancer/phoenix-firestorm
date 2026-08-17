# Vulkanstorm channels and versioning

Vulkanstorm is published in two persistent variants:

- `Vulkanstorm-Release` is the production channel.
- `Vulkanstorm-Dev` is the development and integration channel.

The public version format is `version.subversion-year.month.build_incremental`.
Development and integration builds remain in the `0.x` series. The initial
family is `0.1-26.08`, producing complete versions such as
`0.1-26.08.1234`. Version `1.0` is reserved for the first production-ready
viewer.

The build component comes from the existing build-number precedence: the
explicit `revision` environment variable, the Autobuild build ID, or the Git
revision count for local builds.

Some platform and protocol interfaces require four numeric components. For
those interfaces, the year and month are combined into the numeric patch
component. Thus public version `0.1-26.08.1234` has compatibility version
`0.1.2608.1234`. The compatibility value is not the user-facing version.
