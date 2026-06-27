# -*- cmake -*-
include(Prebuilt)

if (WINDOWS)
	option(USE_MESAZINK "Bundle Mesa Zink DLL overrides for Windows builds" OFF)
else ()
	set(USE_MESAZINK OFF CACHE BOOL "Bundle Mesa Zink DLL overrides for Windows builds" FORCE)
endif ()

if (USE_MESAZINK)
	use_prebuilt_binary(mesazink)
endif ()
