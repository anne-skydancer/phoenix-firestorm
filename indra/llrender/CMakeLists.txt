# -*- cmake -*-

project(llrender)

include(00-Common)
include(OpenGL)
include(FreeType)
include(LLCommon)
include(LLImage)
include(LLWindow)
include(OpenSSL)

set(llrender_SOURCE_FILES
    llcubemap.cpp
    llcubemaparray.cpp
    llfontbitmapcache.cpp
    llfontfreetype.cpp
    llfontfreetypesvg.cpp
    llfontgl.cpp
    llfontvertexbuffer.cpp
    llfontregistry.cpp
    llgl.cpp
    llglslshader.cpp
    llgltexture.cpp
    llimagegl.cpp
    llpostprocess.cpp
    llrender.cpp
    llrender2dutils.cpp
    llrendernavprim.cpp
    llrendersphere.cpp
    llrendertarget.cpp
    llshadermgr.cpp
    lltexture.cpp
    lltexturemanagerbridge.cpp
    lluiimage.cpp
    llvertexbuffer.cpp
    llglcommonfunc.cpp
    ghi/backends/opengl/llghiopenglinfo.cpp
    ghi/core/llghipresentation.cpp
    ghi/core/llghirendererinfo.cpp
    ghi/core/llghitrace.cpp
    ghi/core/llghivalidation.cpp
    )
    
set(llrender_HEADER_FILES
    CMakeLists.txt

    llcubemap.h
    llcubemaparray.h
    llfontgl.h
    llfontvertexbuffer.h
    llfontfreetype.h
    llfontfreetypesvg.h
    llfontbitmapcache.h
    llfontregistry.h
    llgl.h
    llglheaders.h
    llglslshader.h
    llglstates.h
    llgltexture.h
    llgltypes.h
    llimagegl.h
    llpostprocess.h
    llrender.h
    llrender2dutils.h
    llrendernavprim.h
    llrendersphere.h
    llshadermgr.h
    lltexture.h
    lltexturemanagerbridge.h
    lluiimage.h
    lluiimage.inl
    llvertexbuffer.h
    llglcommonfunc.h
    ghi/backends/opengl/llghiopenglinfo.h
    ghi/include/llghi.h
    ghi/include/llghicommand.h
    ghi/include/llghidescriptors.h
    ghi/include/llghidevice.h
    ghi/include/llghipresentation.h
    ghi/include/llghirendererinfo.h
    ghi/include/llghitypes.h
    ghi/core/llghihandlepool.h
    ghi/core/llghitrace.h
    ghi/core/llghivalidation.h
    )

if (USE_VULKAN_GHI)
  if (NOT WINDOWS)
    message(FATAL_ERROR "The R1 Vulkan GHI presentation backend currently supports Windows only")
  endif ()
  find_package(Vulkan 1.3 REQUIRED)
  list(APPEND llrender_SOURCE_FILES
      ghi/backends/vulkan/llghivulkanpresentation.cpp
      ghi/backends/vulkan/llghivulkanpresentation.h
      )
endif ()

list(APPEND llrender_SOURCE_FILES ${llrender_HEADER_FILES})

if (BUILD_HEADLESS)
  add_library (llrenderheadless
    ${llrender_SOURCE_FILES}
    )

  set_property(TARGET llrenderheadless
    PROPERTY COMPILE_DEFINITIONS LL_MESA=1 LL_MESA_HEADLESS=1
      )

  target_link_libraries(llrenderheadless
          llcommon
          llimage
          llmath
          llrender
          llxml
          llfilesystem
          )
endif (BUILD_HEADLESS)

add_library (llrender ${llrender_SOURCE_FILES})
target_include_directories(llrender
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
        )
target_compile_definitions(llrender PRIVATE
        LL_GHI_VULKAN=$<BOOL:${USE_VULKAN_GHI}>
        )

set_property(TARGET llrender PROPERTY POSITION_INDEPENDENT_CODE ON)

# Libraries on which this library depends, needed for Linux builds
# Sort by high-level to low-level
target_link_libraries(llrender
        llcommon
        llimage
        llmath
        llfilesystem
        llxml
        llwindow
        ll::freetype
        ll::openssl
        OpenGL::GL
        OpenGL::GLU
        )

if (USE_VULKAN_GHI)
  target_link_libraries(llrender Vulkan::Vulkan)

  add_executable(llrender_vulkan_lifecycle_harness
      ghi/backends/vulkan/llghivulkanlifecycleharness.cpp
      )
  target_include_directories(llrender_vulkan_lifecycle_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_link_libraries(llrender_vulkan_lifecycle_harness
      llrender
      Vulkan::Vulkan
      )
endif ()

if (LL_TESTS)
  add_executable(llrender_ghi_contract_test
      ghi/core/llghitrace.cpp
      ghi/core/llghirendererinfo.cpp
      ghi/core/llghivalidation.cpp
      tests/ghi/core/llghivalidation_test.cpp
      ${CMAKE_SOURCE_DIR}/test/test.cpp
      ${CMAKE_SOURCE_DIR}/test/lltut.cpp
      )
  target_include_directories(llrender_ghi_contract_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      ${CMAKE_SOURCE_DIR}/test
      )
  target_compile_definitions(llrender_ghi_contract_test PRIVATE
      LL_TEST=llghivalidation
      LL_TEST_llghivalidation
      )
  target_link_libraries(llrender_ghi_contract_test
      llcommon
      llmath
      ll::openssl
      )
  add_test(NAME llrender_ghi_contract COMMAND llrender_ghi_contract_test)
endif (LL_TESTS)
