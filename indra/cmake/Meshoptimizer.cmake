# -*- cmake -*-

include(Linking)
include(Prebuilt)

include_guard()
add_library( ll::meshoptimizer INTERFACE IMPORTED )

if (FREEBSD)
  # The system meshoptimizer pkg ships a cmake-config (used for headers) but the
  # generic use_system_binary path resolves it to a target, not a *_LIBRARIES
  # var, so ll::meshoptimizer ends up without the .so. Link it explicitly.
  find_library(MESHOPTIMIZER_LIBRARY NAMES meshoptimizer PATHS /usr/local/lib REQUIRED)
  target_link_libraries( ll::meshoptimizer INTERFACE ${MESHOPTIMIZER_LIBRARY} )
  return()
endif ()

use_system_binary(meshoptimizer)
use_prebuilt_binary(meshoptimizer)

find_library(MESHOPTIMIZER_LIBRARY
    NAMES
    meshoptimizer.lib
    libmeshoptimizer.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

target_link_libraries(ll::meshoptimizer INTERFACE ${MESHOPTIMIZER_LIBRARY})

target_include_directories(ll::meshoptimizer SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include/meshoptimizer)
