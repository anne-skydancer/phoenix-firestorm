# -*- cmake -*-
include(Prebuilt)
include(Linking)

include_guard()
add_library( ll::libcurl INTERFACE IMPORTED )

if (FREEBSD)
  # Link system libcurl directly rather than via pkg-config: libcurl.pc lists
  # Requires.private (mit-krb5-gssapi, brotli, zstd, ...) that trip pkg-config's
  # full-graph validation, but libcurl.so is self-contained for shared linking.
  find_library(CURL_LIBRARY NAMES curl REQUIRED)
  find_path(CURL_INCLUDE_DIR NAMES curl/curl.h REQUIRED)
  target_include_directories(ll::libcurl SYSTEM INTERFACE ${CURL_INCLUDE_DIR})
  target_link_libraries(ll::libcurl INTERFACE ${CURL_LIBRARY})
  return()
endif ()

use_system_binary(libcurl)
use_prebuilt_binary(curl)

find_library(CURL_LIBRARY
    NAMES
    libcurl.lib
    libcurl.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

target_link_libraries(ll::libcurl INTERFACE ${CURL_LIBRARY} ll::openssl ll::nghttp2 ll::zlib-ng)

target_include_directories( ll::libcurl SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include)
