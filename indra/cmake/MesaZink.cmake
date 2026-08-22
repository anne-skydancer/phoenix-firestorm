# -*- cmake -*-
if (WINDOWS)
    option(USE_MESAZINK "Bundle the Mesa Zink OpenGL-over-Vulkan runtime" OFF)
else ()
    set(USE_MESAZINK OFF CACHE BOOL "Bundle the Mesa Zink OpenGL-over-Vulkan runtime" FORCE)
endif ()

if (USE_MESAZINK)
    foreach(mesazink_file libgallium_wgl.dll opengl32.dll)
        if (NOT EXISTS "${AUTOBUILD_INSTALL_DIR}/bin/release/${mesazink_file}")
            message(FATAL_ERROR "Missing bootstrapped Mesa Zink runtime: ${mesazink_file}")
        endif ()
    endforeach()
endif ()
