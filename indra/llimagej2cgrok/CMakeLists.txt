# -*- cmake -*-

project(llimagej2cgrok)

if (USE_GROK)
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

    add_library(llimagej2cgrok ${llimagej2cgrok_SOURCE_FILES})
    target_link_libraries(llimagej2cgrok
        llcommon
        llimage
        ll::grok
        )

    if (WINDOWS)
        add_custom_command(TARGET llimagej2cgrok POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHARED_LIB_STAGING_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${GROK_RUNTIME_DLL}" "${SHARED_LIB_STAGING_DIR}/grokj2k.dll"
            COMMENT "Staging grokj2k.dll"
            VERBATIM
            )
    endif ()
endif ()
