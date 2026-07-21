# -*- cmake -*-
include(Prebuilt)

if (FREEBSD)
  # fontconfig from the system (as LINUX does); hunspell is a system lib
  # (Hunspell.cmake); no FreeBSD SLVoice exists; fonts/emoji are "common" data
  # packages. nanosvg is vendored in-tree, so it needs nothing here.
  add_library( ll::fontconfig INTERFACE IMPORTED )
  find_package(Fontconfig REQUIRED)
  target_link_libraries( ll::fontconfig INTERFACE Fontconfig::Fontconfig )
  use_prebuilt_common(viewer-fonts)
  use_prebuilt_common(google-fonts)
  use_prebuilt_common(emoji_shortcodes)
  return()
endif ()

if (LINUX)
  use_prebuilt_binary(libuuid)
  add_library( ll::fontconfig INTERFACE IMPORTED )

  if( NOT USE_CONAN )
    find_package(Fontconfig REQUIRED) # <FS:PC> Use system wide Fontconfig
    target_link_libraries( ll::fontconfig INTERFACE Fontconfig::Fontconfig )
  else()
    target_link_libraries( ll::fontconfig INTERFACE CONAN_PKG::fontconfig )
  endif()
endif (LINUX)

if( NOT USE_CONAN )
  use_prebuilt_binary(libhunspell)
endif()

use_prebuilt_binary(slvoice)
use_prebuilt_binary(nanosvg)
use_prebuilt_binary(viewer-fonts)
use_prebuilt_binary(google-fonts)
use_prebuilt_binary(emoji_shortcodes)
