cmake_minimum_required(VERSION 3.16)
project(soloud-vulkanstorm LANGUAGES C CXX)

if (NOT DEFINED SOLOUD_SOURCE_DIR)
    message(FATAL_ERROR "SOLOUD_SOURCE_DIR must name the pinned, patched SoLoud checkout")
endif ()

set(SOLOUD_SOURCES
    ${SOLOUD_SOURCE_DIR}/src/core/soloud.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_audiosource.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_bus.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_core_3d.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_core_basicops.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_core_faderops.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_core_filterops.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_core_getters.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_core_setters.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_core_voicegroup.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_core_voiceops.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_fader.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_fft.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_fft_lut.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_file.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_filter.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_misc.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_queue.cpp
    ${SOLOUD_SOURCE_DIR}/src/core/soloud_thread.cpp
    ${SOLOUD_SOURCE_DIR}/src/audiosource/wav/dr_impl.cpp
    ${SOLOUD_SOURCE_DIR}/src/audiosource/wav/soloud_wav.cpp
    ${SOLOUD_SOURCE_DIR}/src/audiosource/wav/soloud_wavstream.cpp
    ${SOLOUD_SOURCE_DIR}/src/audiosource/wav/stb_vorbis.c
    ${SOLOUD_SOURCE_DIR}/src/backend/miniaudio/soloud_miniaudio.cpp
    ${SOLOUD_SOURCE_DIR}/src/backend/nosound/soloud_nosound.cpp)

add_library(soloud STATIC ${SOLOUD_SOURCES})
target_compile_features(soloud PUBLIC cxx_std_11)
target_compile_definitions(soloud PRIVATE WITH_MINIAUDIO=1 WITH_NOSOUND=1)
target_include_directories(soloud PUBLIC
    ${SOLOUD_SOURCE_DIR}/include
    ${SOLOUD_SOURCE_DIR}/src/backend/miniaudio)

if (MSVC)
    target_compile_options(soloud PRIVATE /W1 /WX-)
else ()
    target_compile_options(soloud PRIVATE -w)
endif ()

install(TARGETS soloud ARCHIVE DESTINATION lib)
install(DIRECTORY ${SOLOUD_SOURCE_DIR}/include/ DESTINATION include/soloud
        FILES_MATCHING PATTERN "*.h")
install(FILES ${SOLOUD_SOURCE_DIR}/src/backend/miniaudio/miniaudio.h
        DESTINATION include/soloud)
install(FILES ${SOLOUD_SOURCE_DIR}/LICENSE DESTINATION LICENSES
        RENAME soloud.txt)
