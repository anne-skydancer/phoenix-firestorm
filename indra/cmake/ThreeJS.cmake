# -*- cmake -*-
if (NOT FREEBSD)  # JS asset; the copy step below is disabled, so it is unused
  use_prebuilt_binary(threejs)
endif ()

# Main three.js file
#configure_file("${AUTOBUILD_INSTALL_DIR}/js/three.min.js" "${CMAKE_SOURCE_DIR}/newview/skins/default/html/common/equirectangular/js/three.min.js" COPYONLY)

# Controls to move around the scene using mouse or keyboard
#configure_file("${AUTOBUILD_INSTALL_DIR}/js/OrbitControls.js" "${CMAKE_SOURCE_DIR}/newview/skins/default/html/common/equirectangular/js/OrbitControls.js" COPYONLY)
