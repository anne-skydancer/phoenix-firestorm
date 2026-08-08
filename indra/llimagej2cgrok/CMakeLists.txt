# -*- cmake -*-

project(llimagej2cgrok)

# Mutually exclusive with both KDU and OpenJPEG: see indra/llimage/CMakeLists.txt
# for the USE_KDU / USE_GROK selection that picks exactly one J2C backend.
if (NOT USE_KDU AND USE_GROK)

include(00-Common)
include(LLCommon)
include(LLImage)
include(Grok)

set(llimagej2cgrok_SOURCE_FILES
    llimagej2cgrok.cpp
    )

set(llimagej2cgrok_HEADER_FILES
    CMakeLists.txt

    llimagej2cgrok.h
    )

list(APPEND llimagej2cgrok_SOURCE_FILES ${llimagej2cgrok_HEADER_FILES})

add_library (llimagej2cgrok ${llimagej2cgrok_SOURCE_FILES})
target_link_libraries( llimagej2cgrok
        llcommon
        llimage
        ll::grok
    )

# Stage the Grok runtime DLL into the shared-lib staging directory, the
# same location viewer_manifest.py already reads libgallium_wgl.dll and
# openjp2.dll from, since Grok is a locally built tree rather than an
# autobuild "3p" package and nothing else copies it there.
if (GROK_RUNTIME_DLL)
    add_custom_command(TARGET llimagej2cgrok POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHARED_LIB_STAGING_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GROK_RUNTIME_DLL}" "${SHARED_LIB_STAGING_DIR}/grokj2k.dll"
        COMMENT "Staging grokj2k.dll into ${SHARED_LIB_STAGING_DIR}"
        VERBATIM
        )
endif ()

endif()
