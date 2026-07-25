# -*- cmake -*-

project(media_plugin_webkit)

include(00-Common)
include(LLCommon)
include(LLImage)
include(LLWindow)
include(Linking)
include(PluginAPI)

include(WebKitGTKPlugin)

### media_plugin_webkit

set(media_plugin_webkit_SOURCE_FILES
    media_plugin_webkit.cpp
    )

add_library(media_plugin_webkit
    SHARED
    ${media_plugin_webkit_SOURCE_FILES}
    )

target_link_libraries(media_plugin_webkit
        media_plugin_base
        ll::webkitgtk
)

# WebKit2GTK plugin is FreeBSD-only (CEF/dullahan unavailable there); mirror the
# libvlc plugin's $ORIGIN rpath so it finds the staged libraries next to it.
if (LINUX OR FREEBSD)
  set(CMAKE_SHARED_LINKER_FLAGS  "-Wl,--build-id -Wl,-rpath,'$ORIGIN:$ORIGIN/../../lib'")
endif()
