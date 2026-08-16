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
    ghi/core/llghidevice.cpp
    ghi/core/llghihash.cpp
    ghi/core/llghipresentation.cpp
    ghi/core/llghipipelinecache.cpp
    ghi/core/llghirendererinfo.cpp
    ghi/core/llghishaderpackage.cpp
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
    ghi/backends/opengl/llghiopenglpresentation.h
    ghi/include/llghi.h
    ghi/include/llghicommand.h
    ghi/include/llghidescriptors.h
    ghi/include/llghidevice.h
    ghi/include/llghipresentation.h
    ghi/include/llghirendererinfo.h
    ghi/include/llghitypes.h
    ghi/core/llghihandlepool.h
    ghi/core/llghihash.h
    ghi/core/llghipipelinecache.h
    ghi/core/llghidevicebackend.h
    ghi/core/llghipresentationbackend.h
    ghi/core/llghishaderpackage.h
    ghi/core/llghitrace.h
    ghi/core/llghivalidation.h
    )

if (WINDOWS)
  list(APPEND llrender_SOURCE_FILES
      ghi/backends/opengl/llghiopengldevice.cpp
      ghi/backends/opengl/llghiopenglpresentation.cpp
      )
endif ()

if (USE_VULKAN_GHI)
  if (NOT WINDOWS)
    message(FATAL_ERROR "The R1 Vulkan GHI presentation backend currently supports Windows only")
  endif ()
  include(Python)
  find_package(Vulkan 1.3 REQUIRED)

  set(GHI_SHADER_TOOL_HINT "$ENV{VULKAN_SDK}/Bin")
  find_program(GHI_GLSLANG_VALIDATOR NAMES glslangValidator HINTS "${GHI_SHADER_TOOL_HINT}" REQUIRED)
  find_program(GHI_SPIRV_VAL NAMES spirv-val HINTS "${GHI_SHADER_TOOL_HINT}" REQUIRED)
  find_program(GHI_SPIRV_OPT NAMES spirv-opt HINTS "${GHI_SHADER_TOOL_HINT}" REQUIRED)
  find_program(GHI_SPIRV_REFLECT NAMES spirv-reflect HINTS "${GHI_SHADER_TOOL_HINT}" REQUIRED)

  set(GHI_R3_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/r3_diagnostic")
  set(GHI_R3_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/r3_diagnostic.llghisp")
  set(GHI_R3_SHADER_SOURCES
      "${GHI_R3_SHADER_ROOT}/package.json"
      "${GHI_R3_SHADER_ROOT}/opengl41/diagnostic.vert.glsl"
      "${GHI_R3_SHADER_ROOT}/opengl41/diagnostic.frag.glsl"
      "${GHI_R3_SHADER_ROOT}/opengl46/diagnostic.vert.glsl"
      "${GHI_R3_SHADER_ROOT}/opengl46/diagnostic.frag.glsl"
      "${GHI_R3_SHADER_ROOT}/vulkan/diagnostic.vert.glsl"
      "${GHI_R3_SHADER_ROOT}/vulkan/diagnostic.frag.glsl"
      "${GHI_R3_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl"
      )
  add_custom_command(
      OUTPUT "${GHI_R3_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_R3_SHADER_ROOT}/package.json"
              --output "${GHI_R3_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_R3_SHADER_SOURCES}
      COMMENT "Building deterministic R3 GHI shader package"
      VERBATIM
      )
  add_custom_target(llrender_ghi_shaders DEPENDS "${GHI_R3_SHADER_PACKAGE}")
  if (LL_TESTS)
    add_custom_target(
        llrender_ghi_shader_packager_test
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_R3_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        DEPENDS
                llrender_ghi_shaders
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
        COMMENT "Testing deterministic GHI shader packaging and reflection rejection"
        VERBATIM
        )
  endif ()

  list(APPEND llrender_SOURCE_FILES
      ghi/backends/vulkan/llghivulkandevice.cpp
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
if (USE_VULKAN_GHI)
  add_dependencies(llrender llrender_ghi_shaders)
endif ()
target_include_directories(llrender
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
        )
target_compile_definitions(llrender PRIVATE
        LL_GHI_OPENGL_DEVICE=$<BOOL:${WINDOWS}>
        LL_GHI_VULKAN_DEVICE=$<BOOL:${USE_VULKAN_GHI}>
        LL_GHI_OPENGL_PRESENTATION=$<BOOL:${WINDOWS}>
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

  add_executable(llrender_vulkan_resource_harness
      ghi/backends/vulkan/llghivulkanresourceharness.cpp
      )
  target_include_directories(llrender_vulkan_resource_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_link_libraries(llrender_vulkan_resource_harness
      llrender
      Vulkan::Vulkan
      )

  add_executable(llrender_vulkan_draw_harness
      ghi/backends/vulkan/llghivulkandrawharness.cpp
      )
  target_include_directories(llrender_vulkan_draw_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_compile_definitions(llrender_vulkan_draw_harness PRIVATE
      LL_GHI_R3_SHADER_PACKAGE="${GHI_R3_SHADER_PACKAGE}"
      )
  target_link_libraries(llrender_vulkan_draw_harness
      llrender
      Vulkan::Vulkan
      )
  add_dependencies(llrender_vulkan_draw_harness llrender_ghi_shaders)
endif ()

if (WINDOWS)
  add_executable(llrender_opengl_lifecycle_harness
      ghi/backends/opengl/llghiopengllifecycleharness.cpp
      )
  target_include_directories(llrender_opengl_lifecycle_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_link_libraries(llrender_opengl_lifecycle_harness
      llrender
      OpenGL::GL
      )

  add_executable(llrender_opengl_resource_harness
      ghi/backends/opengl/llghiopenglresourceharness.cpp
      )
  target_include_directories(llrender_opengl_resource_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_link_libraries(llrender_opengl_resource_harness
      llrender
      OpenGL::GL
      )

  if (USE_VULKAN_GHI)
    add_executable(llrender_opengl_draw_harness
        ghi/backends/opengl/llghiopengldrawharness.cpp
        )
    target_include_directories(llrender_opengl_draw_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
        )
    target_compile_definitions(llrender_opengl_draw_harness PRIVATE
        LL_GHI_R3_SHADER_PACKAGE="${GHI_R3_SHADER_PACKAGE}"
        )
    target_link_libraries(llrender_opengl_draw_harness
        llrender
        OpenGL::GL
        )
    add_dependencies(llrender_opengl_draw_harness llrender_ghi_shaders)
  endif ()
endif ()

if (LL_TESTS)
  add_executable(llrender_ghi_contract_test
      ghi/core/llghidevice.cpp
      ghi/core/llghihash.cpp
      ghi/core/llghipipelinecache.cpp
      ghi/core/llghishaderpackage.cpp
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
      LL_GHI_OPENGL_DEVICE=0
      LL_GHI_VULKAN_DEVICE=0
      LL_TEST=llghivalidation
      LL_TEST_llghivalidation
      )
  if (USE_VULKAN_GHI)
    target_compile_definitions(llrender_ghi_contract_test PRIVATE
        LL_GHI_R3_SHADER_PACKAGE="${GHI_R3_SHADER_PACKAGE}"
        )
    add_dependencies(llrender_ghi_contract_test llrender_ghi_shader_packager_test)
  endif ()
  target_link_libraries(llrender_ghi_contract_test
      llcommon
      llmath
      ll::openssl
      )
  add_test(NAME llrender_ghi_contract COMMAND llrender_ghi_contract_test)
endif (LL_TESTS)
