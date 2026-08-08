# -*- cmake -*-
# Grok (GrokImageCompression/grok) JPEG 2000 codec.
#
# On by default, like SoLoud. Grok is AGPLv3, so it is only safe in a build that
# does NOT also link Kakadu, FMOD, or Vivox in the same binary -- which holds
# for the default all-open viewer. The proprietary variants are opt-in and must
# leave USE_GROK off. When USE_GROK is off (or USE_KDU is on), the viewer falls
# back to the OpenJPEG J2C backend (see indra/llimage/CMakeLists.txt).
#
# Two ways to provide grok, chosen automatically:
#   * Vendored submodule at indra/grok (branch-tracks upstream master, no pin) ->
#     built in-tree as a static core-only library. This is the default.
#   * Otherwise a separately built grok checkout pointed to by GROK_ROOT (its
#     build/bin/ holding grokj2k). Used where the submodule is absent.

option(USE_GROK "Use Grok (GrokImageCompression/grok) as the Kakadu-free J2C decoder" ON)

include_guard(GLOBAL)
# GLOBAL so the imported target is visible from every scope that includes this
# file, even though the global include guard only lets the body run once.
add_library(ll::grok INTERFACE IMPORTED GLOBAL)

if (NOT USE_KDU AND USE_GROK)

  set(_grok_vendored "${CMAKE_SOURCE_DIR}/grok")

  if (EXISTS "${_grok_vendored}/src/lib/core/highway/CMakeLists.txt")

    # ---- Vendored submodule: build grok's core codec in-tree ----
    set(GROK_ROOT "${_grok_vendored}" CACHE PATH "Path to the vendored grok submodule")

    # Core codec only -- no CLI tools, bundled image-format libraries, language
    # bindings, docs, or tests.
    set(GRK_BUILD_CODEC              OFF CACHE BOOL "" FORCE)
    set(GRK_BUILD_LIBPNG            OFF CACHE BOOL "" FORCE)
    set(GRK_BUILD_LIBTIFF          OFF CACHE BOOL "" FORCE)
    set(GRK_BUILD_LCMS2            OFF CACHE BOOL "" FORCE)
    set(GRK_BUILD_JPEG            OFF CACHE BOOL "" FORCE)
    set(GRK_BUILD_CORE_EXAMPLES     OFF CACHE BOOL "" FORCE)
    set(GRK_BUILD_CODEC_EXAMPLES    OFF CACHE BOOL "" FORCE)
    set(GRK_BUILD_CORE_SWIG_BINDINGS OFF CACHE BOOL "" FORCE)
    set(GRK_BUILD_DOC               OFF CACHE BOOL "" FORCE)
    set(GRK_BUILD_PLUGIN_LOADER     OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING               OFF CACHE BOOL "" FORCE)

    if (NOT TARGET grokj2k)
      set(_grok_saved_shared "${BUILD_SHARED_LIBS}")
      set(BUILD_SHARED_LIBS OFF)
      add_subdirectory(${GROK_ROOT} ${CMAKE_BINARY_DIR}/grok-build EXCLUDE_FROM_ALL)
      set(BUILD_SHARED_LIBS "${_grok_saved_shared}")
    endif ()

    target_link_libraries(ll::grok INTERFACE grokj2k)
    target_include_directories(ll::grok SYSTEM INTERFACE
      "${GROK_ROOT}/src/lib/core"
      "${CMAKE_BINARY_DIR}/grok-build/src/lib/core")

  else ()

    # ---- Prebuilt fallback: a separately built grok checkout ----
    set(GROK_ROOT "C:/vulkanstorm/grok" CACHE PATH "Path to a built GrokImageCompression/grok checkout")

    find_library(GROK_LIBRARY
        NAMES grokj2k
        PATHS "${GROK_ROOT}/build/bin"
        REQUIRED
        NO_DEFAULT_PATH)

    find_file(GROK_RUNTIME_DLL
        NAMES grokj2k.dll
        PATHS "${GROK_ROOT}/build/bin"
        NO_DEFAULT_PATH)

    target_link_libraries(ll::grok INTERFACE ${GROK_LIBRARY})
    target_include_directories(ll::grok SYSTEM INTERFACE
        "${GROK_ROOT}/src/lib/core"
        "${GROK_ROOT}/build/src/lib/core")

    if (NOT GROK_RUNTIME_DLL)
        message(WARNING "Grok: could not find grokj2k.dll under ${GROK_ROOT}/build/bin; "
                        "the viewer will fail to start unless it is copied in manually.")
    endif ()

  endif ()

endif ()
