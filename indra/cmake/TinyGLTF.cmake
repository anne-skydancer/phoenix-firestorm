# -*- cmake -*-
include(Prebuilt)

if (FREEBSD)
  # tinygltf ships as a platform-independent ("common") autobuild package of
  # headers; fetch and unpack it directly (no autobuild/prebuilts on FreeBSD).
  if (NOT EXISTS ${LIBS_PREBUILT_DIR}/include/tinygltf/tiny_gltf.h)
    set(_tinygltf_pkg tinygltf-2.9.3-r1-common-10341018043.tar.zst)
    if (NOT EXISTS ${CMAKE_BINARY_DIR}/${_tinygltf_pkg})
      file(DOWNLOAD
        https://github.com/secondlife/3p-tinygltf/releases/download/v2.9.3-r1/${_tinygltf_pkg}
        ${CMAKE_BINARY_DIR}/${_tinygltf_pkg})
    endif ()
    file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/${_tinygltf_pkg}
      DESTINATION ${LIBS_PREBUILT_DIR})
  endif ()
else ()
  use_prebuilt_binary(tinygltf)
endif ()

set(TINYGLTF_INCLUDE_DIR ${LIBS_PREBUILT_DIR}/include/tinygltf)

