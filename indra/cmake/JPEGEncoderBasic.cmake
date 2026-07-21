# -*- cmake -*-
if (NOT FREEBSD)  # JS asset; the copy step below is disabled, so it is unused
  use_prebuilt_binary(jpegencoderbasic)
endif ()

# Main JS file
#configure_file("${AUTOBUILD_INSTALL_DIR}/js/jpeg_encoder_basic.js" "${CMAKE_SOURCE_DIR}/newview/skins/default/html/common/equirectangular/js/jpeg_encoder_basic.js" COPYONLY)
