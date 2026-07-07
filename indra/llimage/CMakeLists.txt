# -*- cmake -*-

project(llimage)

include(00-Common)
include(LLCommon)
include(LLImage)
include(JPEG)
include(LLKDU)
include(Grok)
include(ZLIBNG)
include(LLAddBuildTest)
include(bugsplat)
include(Tut)

set(llimage_SOURCE_FILES
    llimagebmp.cpp
    llimage.cpp
    llimagedimensionsinfo.cpp
    llimagedxt.cpp
    llimagefilter.cpp
    llimagej2c.cpp
    llimagejpeg.cpp
    llimagepng.cpp
    llimagetga.cpp
    llimageworker.cpp
    llpngwrapper.cpp
    )

set(llimage_HEADER_FILES
    CMakeLists.txt

    llimage.h
    llimagebmp.h
    llimagedimensionsinfo.h
    llimagedxt.h
    llimagefilter.h
    llimagej2c.h
    llimagejpeg.h
    llimagepng.h
    llimagetga.h
    llimageworker.h
    llmapimagetype.h
    llpngwrapper.h
    )

list(APPEND llimage_SOURCE_FILES ${llimage_HEADER_FILES})

add_library (llimage ${llimage_SOURCE_FILES})
target_include_directories( llimage  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})
# Libraries on which this library depends, needed for Linux builds
# Sort by high-level to low-level
if (USE_KDU)
    target_link_libraries(llimage llkdu)
elseif (USE_GROK)
    # Grok (GrokImageCompression/grok) is AGPLv3; only select this for a
    # build that does not also link Kakadu, FMOD, or Vivox in the same
    # binary. See indra/cmake/Grok.cmake.
    target_link_libraries(llimage llimagej2cgrok)
else ()
    target_link_libraries(llimage llimagej2coj)
endif ()

target_link_libraries(llimage
        llfilesystem
        llmath
        llcommon
        ll::libpng
        ll::libjpeg
    )

# Add tests
if (LL_TESTS)
  SET(llimage_TEST_SOURCE_FILES
    llimageworker.cpp
    )
  LL_ADD_PROJECT_UNIT_TESTS(llimage "${llimage_TEST_SOURCE_FILES}")
endif (LL_TESTS)



