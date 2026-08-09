# -*- cmake -*-
# SoLoud audio engine (https://github.com/jarikomppa/soloud), zlib/libpng.
#
# Grok/Mesa-style EXTERNAL dependency (NOT vendored): point SOLOUD_ROOT at a
# latest-stable upstream checkout (with our soloud-contrib fixes applied). We
# compile the needed sources into a static lib here rather than using SoLoud's
# upstream "genie" build. Backends: miniaudio (primary, cross-platform), nosound.
#
# Opt-in like USE_GROK: --soloud sets -DUSE_SOLOUD:BOOL=ON in configure_firestorm.sh.
include_guard()

set(USE_SOLOUD OFF CACHE BOOL "Build with the SoLoud audio engine backend")

if (USE_SOLOUD)
    if (NOT SOLOUD_ROOT)
        set(SOLOUD_ROOT "C:/vulkanstorm/soloud" CACHE PATH "SoLoud source checkout")
    endif ()

    set(soloud_SOURCE_FILES
        ${SOLOUD_ROOT}/src/core/soloud.cpp
        ${SOLOUD_ROOT}/src/core/soloud_audiosource.cpp
        ${SOLOUD_ROOT}/src/core/soloud_bus.cpp
        ${SOLOUD_ROOT}/src/core/soloud_core_3d.cpp
        ${SOLOUD_ROOT}/src/core/soloud_core_basicops.cpp
        ${SOLOUD_ROOT}/src/core/soloud_core_faderops.cpp
        ${SOLOUD_ROOT}/src/core/soloud_core_filterops.cpp
        ${SOLOUD_ROOT}/src/core/soloud_core_getters.cpp
        ${SOLOUD_ROOT}/src/core/soloud_core_setters.cpp
        ${SOLOUD_ROOT}/src/core/soloud_core_voicegroup.cpp
        ${SOLOUD_ROOT}/src/core/soloud_core_voiceops.cpp
        ${SOLOUD_ROOT}/src/core/soloud_fader.cpp
        ${SOLOUD_ROOT}/src/core/soloud_fft.cpp
        ${SOLOUD_ROOT}/src/core/soloud_fft_lut.cpp
        ${SOLOUD_ROOT}/src/core/soloud_file.cpp
        ${SOLOUD_ROOT}/src/core/soloud_filter.cpp
        ${SOLOUD_ROOT}/src/core/soloud_misc.cpp
        ${SOLOUD_ROOT}/src/core/soloud_queue.cpp
        ${SOLOUD_ROOT}/src/core/soloud_thread.cpp
        ${SOLOUD_ROOT}/src/audiosource/wav/dr_impl.cpp
        ${SOLOUD_ROOT}/src/audiosource/wav/soloud_wav.cpp
        ${SOLOUD_ROOT}/src/audiosource/wav/soloud_wavstream.cpp
        ${SOLOUD_ROOT}/src/audiosource/wav/stb_vorbis.c
        ${SOLOUD_ROOT}/src/backend/miniaudio/soloud_miniaudio.cpp
        ${SOLOUD_ROOT}/src/backend/nosound/soloud_nosound.cpp
        )

    add_library(soloud STATIC ${soloud_SOURCE_FILES})

    target_include_directories(soloud PUBLIC
        ${SOLOUD_ROOT}/include
        # miniaudio.h exposed for device enumeration in the viewer audio engine
        ${SOLOUD_ROOT}/src/backend/miniaudio)
    target_compile_definitions(soloud PRIVATE WITH_MINIAUDIO=1 WITH_NOSOUND=1)

    # Third-party code: undo the global warnings-as-errors from 00-Common
    if (MSVC)
        target_compile_options(soloud PRIVATE /WX- /W1)
    else ()
        target_compile_options(soloud PRIVATE -w)
    endif ()

    add_library( ll::soloud INTERFACE IMPORTED )
    target_compile_definitions( ll::soloud INTERFACE LL_SOLOUD=1 )
    target_link_libraries( ll::soloud INTERFACE soloud )
endif ()
