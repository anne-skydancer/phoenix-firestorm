# Vulkan GHI backend

R1 owns Vulkan instance, device, queue, Win32 surface, swapchain, clear/present,
resize/suspend, and shutdown here. Vulkan entry points and native types are
permitted in this directory only. Build it with `USE_VULKAN_GHI=ON`; it remains
developer-gated and does not yet receive viewer world-rendering commands.
