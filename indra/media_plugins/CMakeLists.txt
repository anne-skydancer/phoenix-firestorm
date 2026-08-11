# -*- cmake -*-
add_subdirectory(base)

if (LINUX)
    add_subdirectory(gstreamer10)
    add_subdirectory(cef)
    #add_subdirectory(example)
endif (LINUX)

if (WINDOWS)
    add_subdirectory(cef)
    add_subdirectory(libvlc)
    #add_subdirectory(example)
endif (WINDOWS)
