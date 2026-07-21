# -*- cmake -*-
include(Prebuilt)

add_library(ll::vhacd INTERFACE IMPORTED)

if (FREEBSD)
  # V-HACD 4.x is a single self-contained header; there is no FreeBSD package,
  # so fetch it (as Megapahit does) into the prebuilt include dir used below.
  if (NOT EXISTS ${LIBS_PREBUILT_DIR}/include/vhacd/VHACD.h)
    if (NOT EXISTS ${CMAKE_BINARY_DIR}/v-hacd-4.1.0.tar.gz)
      file(DOWNLOAD
        https://github.com/kmammou/v-hacd/archive/refs/tags/v4.1.0.tar.gz
        ${CMAKE_BINARY_DIR}/v-hacd-4.1.0.tar.gz)
    endif ()
    file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/v-hacd-4.1.0.tar.gz
      DESTINATION ${CMAKE_BINARY_DIR})
    file(MAKE_DIRECTORY ${LIBS_PREBUILT_DIR}/include/vhacd)
    file(COPY ${CMAKE_BINARY_DIR}/v-hacd-4.1.0/include/VHACD.h
      DESTINATION ${LIBS_PREBUILT_DIR}/include/vhacd)
  endif ()
else ()
  use_system_binary(vhacd)
  use_prebuilt_binary(vhacd)
endif ()

target_include_directories(ll::vhacd SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include/vhacd/)
