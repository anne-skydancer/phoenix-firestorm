# -*- cmake -*-
include(Prebuilt)
include(GLH)

add_library( ll::glext INTERFACE IMPORTED )
if (NOT FREEBSD)
  # glext.h is provided by Mesa (/usr/local/include/GL/glext.h) on FreeBSD, on
  # the global system include path -- no autobuild package needed.
  use_system_binary(glext)
  use_prebuilt_binary(glext)
endif ()


