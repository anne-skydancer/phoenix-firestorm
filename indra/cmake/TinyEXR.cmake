# -*- cmake -*-
include(Prebuilt)

if (FREEBSD)
  use_prebuilt_common(tinyexr)   # header-only, platform-independent
else ()
  use_prebuilt_binary(tinyexr)
endif ()

set(TINYEXR_INCLUDE_DIR ${LIBS_PREBUILT_DIR}/include/tinyexr)

