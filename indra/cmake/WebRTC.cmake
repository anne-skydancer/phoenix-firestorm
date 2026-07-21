# -*- cmake -*-
include_guard()

include(Linking)
include(Prebuilt)

add_library( ll::webrtc INTERFACE IMPORTED )

if (FREEBSD)
  # PHASE 1: no native libwebrtc for FreeBSD yet, so llwebrtc is built from an
  # inert stub (see indra/llwebrtc/CMakeLists.txt) and ll::webrtc needs nothing.
  # PHASE 2 builds libwebrtc-m137 for FreeBSD (3p-webrtc-build freebsd64) and
  # wires the real library in here.
  return()
endif ()

target_include_directories( ll::webrtc SYSTEM INTERFACE "${LIBS_PREBUILT_DIR}/include/webrtc" "${LIBS_PREBUILT_DIR}/include/webrtc/third_party/abseil-cpp")
use_prebuilt_binary(webrtc)

find_library(WEBRTC_LIBRARY
    NAMES
    webrtc
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

target_link_libraries( ll::webrtc INTERFACE ${WEBRTC_LIBRARY} )

if (DARWIN)
    target_link_libraries( ll::webrtc INTERFACE ll::oslibraries )
elseif (LINUX)
    target_link_libraries( ll::webrtc INTERFACE X11 )
endif ()


