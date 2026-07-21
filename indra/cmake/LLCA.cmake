# -*- cmake -*-
include(Prebuilt)

if (FREEBSD)
  use_prebuilt_common(llca)   # CA cert bundle, platform-independent data
else ()
  use_prebuilt_binary(llca)
endif ()
