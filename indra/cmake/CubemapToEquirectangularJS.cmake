# -*- cmake -*-
if (NOT FREEBSD)  # JS asset; the copy step below is disabled, so it is unused
  use_prebuilt_binary(cubemaptoequirectangular)
endif ()

# Main JS file
#configure_file("${AUTOBUILD_INSTALL_DIR}/js/CubemapToEquirectangular.js" "${CMAKE_SOURCE_DIR}/newview/skins/default/html/common/equirectangular/js/CubemapToEquirectangular.js" COPYONLY)
