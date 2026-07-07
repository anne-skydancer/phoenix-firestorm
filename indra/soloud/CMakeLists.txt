# -*- cmake -*-
# Vendored SoLoud audio engine (https://github.com/jarikomppa/soloud)
# zlib/libpng license; see LICENSE. Backends: miniaudio (primary), nosound.

project(soloud)

set(soloud_SOURCE_FILES
    src/core/soloud.cpp
    src/core/soloud_audiosource.cpp
    src/core/soloud_bus.cpp
    src/core/soloud_core_3d.cpp
    src/core/soloud_core_basicops.cpp
    src/core/soloud_core_faderops.cpp
    src/core/soloud_core_filterops.cpp
    src/core/soloud_core_getters.cpp
    src/core/soloud_core_setters.cpp
    src/core/soloud_core_voicegroup.cpp
    src/core/soloud_core_voiceops.cpp
    src/core/soloud_fader.cpp
    src/core/soloud_fft.cpp
    src/core/soloud_fft_lut.cpp
    src/core/soloud_file.cpp
    src/core/soloud_filter.cpp
    src/core/soloud_misc.cpp
    src/core/soloud_queue.cpp
    src/core/soloud_thread.cpp
    src/audiosource/wav/dr_impl.cpp
    src/audiosource/wav/soloud_wav.cpp
    src/audiosource/wav/soloud_wavstream.cpp
    src/audiosource/wav/stb_vorbis.c
    src/backend/miniaudio/soloud_miniaudio.cpp
    src/backend/nosound/soloud_nosound.cpp
    )

add_library(soloud STATIC ${soloud_SOURCE_FILES})

target_include_directories(soloud PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# miniaudio.h exposed for device enumeration in the viewer audio engine
target_include_directories(soloud PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backend/miniaudio)
target_compile_definitions(soloud PRIVATE WITH_MINIAUDIO=1 WITH_NOSOUND=1)

# Third-party code: undo the global warnings-as-errors from 00-Common
if (MSVC)
    target_compile_options(soloud PRIVATE /WX- /W1)
else ()
    target_compile_options(soloud PRIVATE -w)
endif ()
