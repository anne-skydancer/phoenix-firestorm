# -*- cmake -*-

project(media_plugin_example)

include(00-Common)
include(LLCommon)
include(LLImage)
include(LLWindow)
include(Linking)
include(PluginAPI)

include(ExamplePlugin)

### media_plugin_example

set(media_plugin_example_SOURCE_FILES
    media_plugin_example.cpp
    )

add_library(media_plugin_example
    SHARED
    ${media_plugin_example_SOURCE_FILES}
    )

target_link_libraries(media_plugin_example media_plugin_base )

if (WINDOWS)
  set_target_properties(
    media_plugin_example
    PROPERTIES
    LINK_FLAGS "/MANIFEST:NO /NODEFAULTLIB:LIBCMT"
    )
endif (WINDOWS)
