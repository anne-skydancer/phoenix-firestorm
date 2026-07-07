# -*- cmake -*-
include_guard()

# SoLoud is vendored in indra/soloud (zlib licensed); the static library
# target is added from indra/CMakeLists.txt when USE_SOLOUD is set.

set(USE_SOLOUD ON CACHE BOOL "Enable SoLoud audio engine")

if (USE_SOLOUD)
  add_library( ll::soloud INTERFACE IMPORTED )
  target_compile_definitions( ll::soloud INTERFACE LL_SOLOUD=1)
  target_link_libraries( ll::soloud INTERFACE soloud )
endif ()
