# -*- cmake -*-

include(Prebuilt)

include_guard()
add_library( ll::openxr INTERFACE IMPORTED )

if(USE_CONAN )
  target_link_libraries( ll::openxr INTERFACE CONAN_PKG::openxr )
  return()
endif()

if (FREEBSD)
  include(FindPkgConfig)
  pkg_check_modules(OPENXR REQUIRED IMPORTED_TARGET openxr)
  target_link_libraries(ll::openxr INTERFACE PkgConfig::OPENXR)
  return()
endif ()

use_prebuilt_binary(openxr)
if (WINDOWS)
  target_link_libraries( ll::openxr INTERFACE ${ARCH_PREBUILT_DIRS_RELEASE}/openxr_loader.lib )
else()
  target_link_libraries( ll::openxr INTERFACE ${ARCH_PREBUILT_DIRS_RELEASE}/libopenxr_loader.a )
endif (WINDOWS)

if( NOT LINUX )
  target_include_directories( ll::openxr SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include)
endif()
