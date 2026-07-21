# -*- cmake -*-
include_guard()

include(Prebuilt)
if (NOT FREEBSD)
  # On FreeBSD xxhash.h comes from the system include path (devel/xxhash).
  use_prebuilt_binary(xxhash)
endif ()
