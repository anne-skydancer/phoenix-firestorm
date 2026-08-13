# -*- cmake -*-

# Preferred open-source JPEG 2000 backend. Grok is AGPLv3, so redistributable
# viewer builds still require the project to apply its chosen license policy.
option(USE_GROK "Use Grok as the J2C codec instead of OpenJPEG" ON)
set(GROK_ROOT "" CACHE PATH "Path to a built Grok checkout")

include_guard(GLOBAL)

if (USE_KDU AND USE_GROK)
    message(STATUS "J2C backend: Kakadu (disabling default Grok selection)")
    set(USE_GROK OFF CACHE BOOL "Use Grok as the J2C codec instead of OpenJPEG" FORCE)
endif ()

if (USE_GROK)

    if (NOT GROK_ROOT)
        get_filename_component(_grok_sibling "${CMAKE_SOURCE_DIR}/../../grok" ABSOLUTE)
        if (EXISTS "${_grok_sibling}/src/lib/core/grok.h")
            set(GROK_ROOT "${_grok_sibling}" CACHE PATH "Path to a built Grok checkout" FORCE)
        endif ()
    endif ()

    if (NOT GROK_ROOT)
        message(FATAL_ERROR "USE_GROK=ON requires GROK_ROOT to point to a built Grok checkout")
    endif ()

    find_path(GROK_INCLUDE_DIR
        NAMES grok.h
        PATHS "${GROK_ROOT}/src/lib/core"
        REQUIRED
        NO_DEFAULT_PATH)
    find_path(GROK_GENERATED_INCLUDE_DIR
        NAMES grk_config.h
        PATHS "${GROK_ROOT}/build/src/lib/core"
        REQUIRED
        NO_DEFAULT_PATH)
    find_library(GROK_LIBRARY
        NAMES grokj2k
        PATHS "${GROK_ROOT}/build/bin" "${GROK_ROOT}/build/lib"
        REQUIRED
        NO_DEFAULT_PATH)

    add_library(ll::grok INTERFACE IMPORTED GLOBAL)
    target_link_libraries(ll::grok INTERFACE "${GROK_LIBRARY}")
    target_include_directories(ll::grok SYSTEM INTERFACE
        "${GROK_INCLUDE_DIR}"
        "${GROK_GENERATED_INCLUDE_DIR}")

    if (WINDOWS)
        find_file(GROK_RUNTIME_DLL
            NAMES grokj2k.dll
            PATHS "${GROK_ROOT}/build/bin"
            REQUIRED
            NO_DEFAULT_PATH)
    endif ()

    message(STATUS "J2C backend: Grok (${GROK_LIBRARY})")
endif ()
