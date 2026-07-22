# -*- cmake -*-
include(Linking)
include(Prebuilt)

include_guard()
add_library( ll::vorbis INTERFACE IMPORTED )

if (FREEBSD)
  # use_system_binary(vorbis) only resolves the 'vorbis' module; the viewer also
  # needs vorbisenc/vorbisfile (encoder/file) and ogg. Pull the full set + ogg
  # from pkg so ogg_*/vorbis_encode_* symbols resolve at link.
  include(FindPkgConfig)
  pkg_check_modules(Vorbis REQUIRED vorbis vorbisenc vorbisfile ogg)
  target_include_directories( ll::vorbis SYSTEM INTERFACE ${Vorbis_INCLUDE_DIRS} )
  target_link_libraries( ll::vorbis INTERFACE ${Vorbis_LIBRARIES} )
  return()
endif ()

use_system_binary(vorbis)
use_prebuilt_binary(ogg_vorbis)
target_include_directories( ll::vorbis SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include )

find_library(OGG_LIBRARY
    NAMES
    libogg.lib
    libogg.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

find_library(VORBIS_LIBRARY
    NAMES
    libvorbis.lib
    libvorbis.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

find_library(VORBISENC_LIBRARY
    NAMES
    libvorbisenc.lib
    libvorbisenc.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

find_library(VORBISFILE_LIBRARY
    NAMES
    libvorbisfile.lib
    libvorbisfile.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

target_link_libraries(ll::vorbis INTERFACE ${VORBISENC_LIBRARY} ${VORBISFILE_LIBRARY} ${VORBIS_LIBRARY} ${OGG_LIBRARY} )

