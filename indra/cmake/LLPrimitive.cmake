# -*- cmake -*-

# these should be moved to their own cmake file
include(Prebuilt)
include(Linking)
include(Boost)

include_guard()

add_library( ll::minizip-ng INTERFACE IMPORTED )
add_library( ll::libxml INTERFACE IMPORTED )
add_library( ll::colladadom INTERFACE IMPORTED )

# ND, needs fixup in collada conan pkg
if( USE_CONAN )
  target_include_directories( ll::colladadom SYSTEM INTERFACE
    "${CONAN_INCLUDE_DIRS_COLLADADOM}/collada-dom/"
    "${CONAN_INCLUDE_DIRS_COLLADADOM}/collada-dom/1.4/" )
endif()

if (FREEBSD)
  # System COLLADA DOM (collada-dom port: the 1.4 module = collada14dom),
  # minizip and libxml2 -- all from pkg. No build-from-source needed.
  include(FindPkgConfig)
  pkg_check_modules(MINIZIP    REQUIRED IMPORTED_TARGET minizip)
  pkg_check_modules(LIBXML2    REQUIRED IMPORTED_TARGET libxml-2.0)
  pkg_check_modules(COLLADADOM REQUIRED IMPORTED_TARGET collada-dom-141)
  target_link_libraries(ll::minizip-ng INTERFACE PkgConfig::MINIZIP)
  target_link_libraries(ll::libxml     INTERFACE PkgConfig::LIBXML2)
  target_link_libraries(ll::colladadom INTERFACE PkgConfig::COLLADADOM ll::boost ll::libxml ll::minizip-ng)
  return()
endif ()

use_system_binary( colladadom )

use_prebuilt_binary(colladadom)
use_prebuilt_binary(minizip-ng) # needed for colladadom
use_prebuilt_binary(libxml2)

find_library(MINIZIPNG_LIBRARY
    NAMES
    minizip.lib
    libminizip.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

target_link_libraries(ll::minizip-ng INTERFACE ${MINIZIPNG_LIBRARY})

find_library(LIBXML2_LIBRARY
    NAMES
    libxml2.lib
    libxml2.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

target_link_libraries(ll::libxml INTERFACE ${LIBXML2_LIBRARY})

if (WINDOWS)
    target_link_libraries( ll::libxml INTERFACE Bcrypt.lib)
endif()

target_include_directories( ll::colladadom SYSTEM INTERFACE
        ${LIBS_PREBUILT_DIR}/include/collada
        ${LIBS_PREBUILT_DIR}/include/collada/1.4
        )

find_library(COLLADADOM_LIBRARY
    NAMES
    libcollada14dom23-s.lib
    collada14dom
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

target_link_libraries(ll::colladadom INTERFACE ${COLLADADOM_LIBRARY} ll::boost ll::libxml ll::minizip-ng)

# <FS> GLIB uses pcre, so we need to keep it for Linux builds
if (LINUX)
   add_library( ll::pcre INTERFACE IMPORTED )
   use_prebuilt_binary(pcre)
   target_link_libraries( ll::pcre INTERFACE pcrecpp pcre )
endif ()
# </FS>