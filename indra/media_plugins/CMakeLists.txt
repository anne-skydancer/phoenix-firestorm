# -*- cmake -*-
add_subdirectory(base)

if (LINUX)
    add_subdirectory(gstreamer10)
    add_subdirectory(cef)
    #add_subdirectory(example)
endif (LINUX)

if (DARWIN)
    add_subdirectory(cef)
    add_subdirectory(libvlc)
    #add_subdirectory(example)
endif (DARWIN)

if (WINDOWS)
    add_subdirectory(cef)
    add_subdirectory(libvlc)
    #add_subdirectory(example)
endif (WINDOWS)

if (FREEBSD)
    # No CEF on FreeBSD (EXTERNAL_TOS); use the libvlc media plugin for
    # streaming audio/video (parcel music, internet radio, media-on-a-prim).
    add_subdirectory(libvlc)

    # WebKit2GTK (webkit2gtk-4.0 / GTK3) provides web + image MOAP rendering
    # (text/html, image/*) in place of CEF/dullahan, which is unavailable on
    # FreeBSD. Needed for the collar script dashboard.
    add_subdirectory(webkit)
endif (FREEBSD)
