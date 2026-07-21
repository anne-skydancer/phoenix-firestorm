# -*- cmake -*-
include(Prebuilt)

if (NOT FREEBSD)
  # TUT is a header-only test framework; with LL_TESTS=OFF nothing includes it.
  use_prebuilt_binary(tut)
endif ()
