# Vulkan GHI backend

This directory owns native Vulkan GHI execution. Vulkan entry points and types
stay inside the backend; build it with `USE_VULKAN_GHI=ON`.

Through R6, Vulkan independently executes deterministic world-material and
alpha fixtures using Vulkan GLSL packages. R6 covers legacy alpha/masks and
residual routes, bounded PPLL, and bounded depth peeling with validation layers
enabled. Output must match the OpenGL peer hashes exactly.

The backend remains developer-gated and additive. It does not yet receive the
production viewer world or UI command stream, so Mesa/Zink and native OpenGL
remain usable and unchanged while Vulkan parity is established.

R7 independently executes cube-array face rendering, probe mip generation,
and synchronized dynamic/media image updates. Cube-array support is queried as
a semantic capability and enabled explicitly. The R7 native fixture must pass
with Khronos synchronization validation and match both OpenGL peers bit for
bit; it does not reroute production mirrors, probes, snapshots, or impostors.
