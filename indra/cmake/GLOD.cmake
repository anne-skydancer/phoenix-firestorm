# -*- cmake -*-

include_guard()
add_library( fs::glod INTERFACE IMPORTED )

if (FREEBSD)
  # GLOD has no FreeBSD prebuilt/port. Empty target so configure completes;
  # resolved at build time (build libGLOD from source to keep the GLOD LOD
  # feature, or gate that path -- meshoptimizer is the alternative).
  return()
endif ()

include(Prebuilt)
use_prebuilt_binary(glod)

find_library(GLOD_LIBRARY
  NAMES
  GLOD.lib
  libGLOD.dylib
  libGLOD.a
  PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

if (WINDOWS OR DARWIN)
  target_link_libraries(fs::glod INTERFACE ${GLOD_LIBRARY})
elseif (LINUX)
  find_library(VDS_LIBRARY
    NAMES
    libvds.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

  target_link_libraries(fs::glod INTERFACE ${GLOD_LIBRARY} ${VDS_LIBRARY})
endif ()

target_include_directories( fs::glod SYSTEM INTERFACE
        ${AUTOBUILD_INSTALL_DIR}/include/glod
        )