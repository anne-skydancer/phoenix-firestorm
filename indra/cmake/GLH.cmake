# -*- cmake -*-
include(Prebuilt)

add_library( ll::glh_linear INTERFACE IMPORTED )

if (NOT FREEBSD)
  # Headers-only autobuild package, unpackaged on FreeBSD. Nothing in the tree
  # currently includes glh_linear.h; leave the interface target empty here and
  # vendor the single header only if a build-time include turns out to need it.
  use_system_binary( glh_linear )
  use_prebuilt_binary(glh_linear)
endif ()
