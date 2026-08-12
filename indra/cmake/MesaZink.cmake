# -*- cmake -*-
include(Prebuilt)

if (WINDOWS)
    option(USE_MESAZINK "Bundle the Mesa Zink OpenGL-over-Vulkan runtime" OFF)
else ()
    set(USE_MESAZINK OFF CACHE BOOL "Bundle the Mesa Zink OpenGL-over-Vulkan runtime" FORCE)
endif ()

if (USE_MESAZINK)
    use_prebuilt_binary(mesazink)
endif ()
