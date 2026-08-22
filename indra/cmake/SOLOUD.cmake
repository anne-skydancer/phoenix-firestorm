# -*- cmake -*-
# SoLoud audio engine, supplied by the reproducible Autobuild package in
# third_party/soloud. The package is pinned and carries the viewer's safety,
# device-selection, diagnostics, and surround-mixing patch series.
include_guard()

set(USE_SOLOUD OFF CACHE BOOL "Build with the SoLoud audio engine backend")

if (USE_SOLOUD)
    include(Prebuilt)
    use_prebuilt_binary(soloud)

    add_library(ll::soloud STATIC IMPORTED GLOBAL)
    find_library(SOLOUD_LIBRARY_RELEASE
        NAMES soloud
        PATHS ${ARCH_PREBUILT_DIRS_RELEASE}
        REQUIRED NO_DEFAULT_PATH)
    set_target_properties(ll::soloud PROPERTIES
        IMPORTED_LOCATION "${SOLOUD_LIBRARY_RELEASE}")

    if (WINDOWS)
        find_library(SOLOUD_LIBRARY_DEBUG
            NAMES soloud
            PATHS ${ARCH_PREBUILT_DIRS_DEBUG}
            NO_DEFAULT_PATH)
        if (SOLOUD_LIBRARY_DEBUG)
            set_target_properties(ll::soloud PROPERTIES
                IMPORTED_LOCATION_DEBUG "${SOLOUD_LIBRARY_DEBUG}"
                IMPORTED_LOCATION_RELEASE "${SOLOUD_LIBRARY_RELEASE}")
        endif ()
        target_link_libraries(ll::soloud INTERFACE avrt.lib)
    endif ()

    target_include_directories(ll::soloud SYSTEM INTERFACE
        ${LIBS_PREBUILT_DIR}/include/soloud)
    target_compile_definitions(ll::soloud INTERFACE LL_SOLOUD=1)
endif ()
