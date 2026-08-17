# OpenGL GHI backend

This directory owns OpenGL-native GHI execution and is the peer oracle for the
parallel Vulkan backend. Native entry points and types stay inside the backend.

R6 executes backend-neutral alpha fixtures for legacy blending and masking,
bounded PPLL, and bounded depth peeling. It verifies exact output hashes against
Vulkan while keeping production world and UI routing on the existing OpenGL
pipeline. Particles, custom blends, pre-water alpha, HUDs, impostors,
reflections, and cube snapshots remain on the legacy residual route.

Windows and Linux may expose the PPLL resource contract from OpenGL 4.3, but
the production direct-GL implementation is intentionally gated at OpenGL 4.4
because it uses `glClearTexImage`. macOS remains capped at OpenGL 4.1 and uses
depth peeling as its only non-legacy alpha-sort option.
