# -*- cmake -*-

project(llimagej2coj)

# <FS:ND> when using KDU, why compile the OpenJPEG library?
# Also skip this if USE_GROK has selected Grok as the Kakadu-free
# codec instead, so the fallbackCreateLLImageJ2CImpl() factory function
# is only ever defined by exactly one J2C implementation.
if (NOT USE_KDU AND NOT USE_GROK)

include(00-Common)
include(LLCommon)
include(LLImage)
include(OpenJPEG)

set(llimagej2coj_SOURCE_FILES
    llimagej2coj.cpp
    )

set(llimagej2coj_HEADER_FILES
    CMakeLists.txt

    llimagej2coj.h
    )

list(APPEND llimagej2coj_SOURCE_FILES ${llimagej2coj_HEADER_FILES})

add_library (llimagej2coj ${llimagej2coj_SOURCE_FILES})
target_link_libraries( llimagej2coj
        llcommon
        llimage
        ll::openjpeg
    )

endif()
