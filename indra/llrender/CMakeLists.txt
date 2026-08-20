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
    ghi/core/llghialphascenepacket.cpp
    ghi/core/llghidevice.cpp
    ghi/core/llghienvironmentscenepacket.cpp
    ghi/core/llghihash.cpp
    ghi/core/llghilightingscenepacket.cpp
    ghi/core/llghilightingpacketconsumer.cpp
    ghi/core/llghimaterialscenepacket.cpp
    ghi/core/llghiproductionalphaexecutor.cpp
    ghi/core/llghiproductionenvironmentexecutor.cpp
    ghi/core/llghiproductionwaterexecutor.cpp
    ghi/core/llghiproductionframeconsumer.cpp
    ghi/core/llghiproductiongbufferexecutor.cpp
    ghi/core/llghiproductionlightingexecutor.cpp
    ghi/core/llghiproductionframepacket.cpp
    ghi/core/llghiproductionframetargets.cpp
    ghi/core/llghiproductiontextureresidency.cpp
    ghi/core/llghiterrainscenepacket.cpp
    ghi/core/llghiterrainoffscreenprobe.cpp
    ghi/core/llghimaterialoffscreenprobe.cpp
    ghi/core/llghiopaqueoffscreenprobe.cpp
    ghi/core/llghiopaquepacketconsumer.cpp
    ghi/core/llghiopaquescenepacket.cpp
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
    ghi/include/llghialphacontract.h
    ghi/include/llghialphascenepacket.h
    ghi/include/llghilightingscenepacket.h
    ghi/include/llghilightingpacketconsumer.h
    ghi/include/llghioffscreencontract.h
    ghi/include/llghiopaqueoffscreenprobe.h
    ghi/include/llghimaterialoffscreenprobe.h
    ghi/include/llghiopaquepacketconsumer.h
    ghi/include/llghicommand.h
    ghi/include/llghidescriptors.h
    ghi/include/llghidevice.h
    ghi/include/llghienvironmentscenepacket.h
    ghi/include/llghipresentation.h
    ghi/include/llghiproductioncontract.h
    ghi/include/llghiproductionalphaexecutor.h
    ghi/include/llghiproductionenvironmentexecutor.h
    ghi/include/llghiproductionwaterexecutor.h
    ghi/include/llghiproductionframeconsumer.h
    ghi/include/llghiproductiongbufferexecutor.h
    ghi/include/llghiproductionlightingexecutor.h
    ghi/include/llghiproductionframepacket.h
    ghi/include/llghiproductionframetargets.h
    ghi/include/llghiproductiontextureresidency.h
    ghi/include/llghimaterialscenepacket.h
    ghi/include/llghiterrainscenepacket.h
    ghi/include/llghiterrainoffscreenprobe.h
    ghi/include/llghiopaquescenepacket.h
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
      "${GHI_R3_SHADER_ROOT}/opengl44/diagnostic.vert.glsl"
      "${GHI_R3_SHADER_ROOT}/opengl44/diagnostic.frag.glsl"
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
  set(GHI_R4_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/r4_opaque")
  set(GHI_R4_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/r4_opaque.llghisp")
  set(GHI_R4_SHADER_SOURCES
      "${GHI_R4_SHADER_ROOT}/package.json"
      "${GHI_R4_SHADER_ROOT}/opengl41/opaque.vert.glsl"
      "${GHI_R4_SHADER_ROOT}/opengl41/opaque.frag.glsl"
      "${GHI_R4_SHADER_ROOT}/opengl44/opaque.vert.glsl"
      "${GHI_R4_SHADER_ROOT}/opengl44/opaque.frag.glsl"
      "${GHI_R4_SHADER_ROOT}/vulkan/opaque.vert.glsl"
      "${GHI_R4_SHADER_ROOT}/vulkan/opaque.frag.glsl"
      "${GHI_R4_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl"
      )
  add_custom_command(
      OUTPUT "${GHI_R4_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_R4_SHADER_ROOT}/package.json"
              --output "${GHI_R4_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_R4_SHADER_SOURCES}
      COMMENT "Building deterministic R4 opaque GHI shader package"
      VERBATIM
      )
  set(GHI_R4C_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/r4_visibility")
  set(GHI_R4C_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/r4_visibility.llghisp")
  set(GHI_R4C_SHADER_SOURCES
      "${GHI_R4C_SHADER_ROOT}/package.json"
      "${GHI_R4C_SHADER_ROOT}/opengl41/visibility.vert.glsl"
      "${GHI_R4C_SHADER_ROOT}/opengl41/visibility.frag.glsl"
      "${GHI_R4C_SHADER_ROOT}/opengl44/visibility.vert.glsl"
      "${GHI_R4C_SHADER_ROOT}/opengl44/visibility.frag.glsl"
      "${GHI_R4C_SHADER_ROOT}/vulkan/visibility.vert.glsl"
      "${GHI_R4C_SHADER_ROOT}/vulkan/visibility.frag.glsl"
      "${GHI_R4C_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl"
      )
  add_custom_command(
      OUTPUT "${GHI_R4C_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_R4C_SHADER_ROOT}/package.json"
              --output "${GHI_R4C_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_R4C_SHADER_SOURCES}
      COMMENT "Building deterministic R4c visibility GHI shader package"
      VERBATIM
      )
  set(GHI_R5A_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/r5_material_skin")
  set(GHI_R5A_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/r5_material_skin.llghisp")
  set(GHI_R5A_SHADER_SOURCES
      "${GHI_R5A_SHADER_ROOT}/package.json"
      "${GHI_R5A_SHADER_ROOT}/opengl41/material_skin.vert.glsl"
      "${GHI_R5A_SHADER_ROOT}/opengl41/material_skin.frag.glsl"
      "${GHI_R5A_SHADER_ROOT}/opengl44/material_skin.vert.glsl"
      "${GHI_R5A_SHADER_ROOT}/opengl44/material_skin.frag.glsl"
      "${GHI_R5A_SHADER_ROOT}/vulkan/material_skin.vert.glsl"
      "${GHI_R5A_SHADER_ROOT}/vulkan/material_skin.frag.glsl"
      "${GHI_R5A_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl"
      )
  add_custom_command(
      OUTPUT "${GHI_R5A_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_R5A_SHADER_ROOT}/package.json"
              --output "${GHI_R5A_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_R5A_SHADER_SOURCES}
      COMMENT "Building deterministic R5a material/skin GHI shader package"
      VERBATIM
      )
  set(GHI_I6_TERRAIN_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/i6_terrain")
  set(GHI_I6_TERRAIN_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/i6_terrain.llghisp")
  set(GHI_I6_TERRAIN_SHADER_SOURCES
      "${GHI_I6_TERRAIN_SHADER_ROOT}/package.json"
      "${GHI_I6_TERRAIN_SHADER_ROOT}/opengl41/terrain.vert.glsl"
      "${GHI_I6_TERRAIN_SHADER_ROOT}/opengl41/terrain.frag.glsl"
      "${GHI_I6_TERRAIN_SHADER_ROOT}/opengl44/terrain.vert.glsl"
      "${GHI_I6_TERRAIN_SHADER_ROOT}/opengl44/terrain.frag.glsl"
      "${GHI_I6_TERRAIN_SHADER_ROOT}/vulkan/terrain.vert.glsl"
      "${GHI_I6_TERRAIN_SHADER_ROOT}/vulkan/terrain.frag.glsl"
      "${GHI_I6_TERRAIN_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl")
  add_custom_command(
      OUTPUT "${GHI_I6_TERRAIN_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_I6_TERRAIN_SHADER_ROOT}/package.json"
              --output "${GHI_I6_TERRAIN_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_I6_TERRAIN_SHADER_SOURCES}
      COMMENT "Building deterministic I6 terrain GHI shader package"
      VERBATIM)
  set(GHI_I7_LIGHTING_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/i7_deferred_lighting")
  set(GHI_I7_LIGHTING_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/i7_deferred_lighting.llghisp")
  set(GHI_I7_LIGHTING_SHADER_SOURCES
      "${GHI_I7_LIGHTING_SHADER_ROOT}/package.json"
      "${GHI_I7_LIGHTING_SHADER_ROOT}/opengl41/deferred_lighting.vert.glsl"
      "${GHI_I7_LIGHTING_SHADER_ROOT}/opengl41/deferred_lighting.frag.glsl"
      "${GHI_I7_LIGHTING_SHADER_ROOT}/opengl44/deferred_lighting.vert.glsl"
      "${GHI_I7_LIGHTING_SHADER_ROOT}/opengl44/deferred_lighting.frag.glsl"
      "${GHI_I7_LIGHTING_SHADER_ROOT}/vulkan/deferred_lighting.vert.glsl"
      "${GHI_I7_LIGHTING_SHADER_ROOT}/vulkan/deferred_lighting.frag.glsl")
  add_custom_command(
      OUTPUT "${GHI_I7_LIGHTING_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_I7_LIGHTING_SHADER_ROOT}/package.json"
              --output "${GHI_I7_LIGHTING_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_I7_LIGHTING_SHADER_SOURCES}
      COMMENT "Building deterministic I7 deferred-lighting GHI shader package"
      VERBATIM)
  set(GHI_I7_PROJECTOR_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/i7_projector_lighting")
  set(GHI_I7_PROJECTOR_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/i7_projector_lighting.llghisp")
  set(GHI_I7_PROJECTOR_SHADER_SOURCES
      "${GHI_I7_PROJECTOR_SHADER_ROOT}/package.json"
      "${GHI_I7_PROJECTOR_SHADER_ROOT}/opengl41/projector_lighting.vert.glsl"
      "${GHI_I7_PROJECTOR_SHADER_ROOT}/opengl41/projector_lighting.frag.glsl"
      "${GHI_I7_PROJECTOR_SHADER_ROOT}/opengl44/projector_lighting.vert.glsl"
      "${GHI_I7_PROJECTOR_SHADER_ROOT}/opengl44/projector_lighting.frag.glsl"
      "${GHI_I7_PROJECTOR_SHADER_ROOT}/vulkan/projector_lighting.vert.glsl"
      "${GHI_I7_PROJECTOR_SHADER_ROOT}/vulkan/projector_lighting.frag.glsl")
  add_custom_command(
      OUTPUT "${GHI_I7_PROJECTOR_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_I7_PROJECTOR_SHADER_ROOT}/package.json"
              --output "${GHI_I7_PROJECTOR_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_I7_PROJECTOR_SHADER_SOURCES}
      COMMENT "Building deterministic I7c projector-lighting GHI shader package"
      VERBATIM)
  set(GHI_I7_SHADOW_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/i7_shadow")
  set(GHI_I7_SHADOW_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/i7_shadow.llghisp")
  set(GHI_I7_SHADOW_SHADER_SOURCES
      "${GHI_I7_SHADOW_SHADER_ROOT}/package.json"
      "${GHI_I7_SHADOW_SHADER_ROOT}/opengl41/shadow.vert.glsl"
      "${GHI_I7_SHADOW_SHADER_ROOT}/opengl41/shadow.frag.glsl"
      "${GHI_I7_SHADOW_SHADER_ROOT}/opengl44/shadow.vert.glsl"
      "${GHI_I7_SHADOW_SHADER_ROOT}/opengl44/shadow.frag.glsl"
      "${GHI_I7_SHADOW_SHADER_ROOT}/vulkan/shadow.vert.glsl"
      "${GHI_I7_SHADOW_SHADER_ROOT}/vulkan/shadow.frag.glsl"
      "${GHI_I7_SHADOW_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl")
  add_custom_command(
      OUTPUT "${GHI_I7_SHADOW_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_I7_SHADOW_SHADER_ROOT}/package.json"
              --output "${GHI_I7_SHADOW_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_I7_SHADOW_SHADER_SOURCES}
      COMMENT "Building deterministic I7d shadow GHI shader package"
      VERBATIM)
  set(GHI_R5_WORLD_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/r5_world")
  set(GHI_R5_WORLD_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/r5_world.llghisp")
  set(GHI_R5_WORLD_SHADER_SOURCES
      "${GHI_R5_WORLD_SHADER_ROOT}/package.json"
      "${GHI_R5_WORLD_SHADER_ROOT}/opengl41/world.vert.glsl"
      "${GHI_R5_WORLD_SHADER_ROOT}/opengl41/world.frag.glsl"
      "${GHI_R5_WORLD_SHADER_ROOT}/opengl44/world.vert.glsl"
      "${GHI_R5_WORLD_SHADER_ROOT}/opengl44/world.frag.glsl"
      "${GHI_R5_WORLD_SHADER_ROOT}/vulkan/world.vert.glsl"
      "${GHI_R5_WORLD_SHADER_ROOT}/vulkan/world.frag.glsl"
      "${GHI_R5_WORLD_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl"
      )
  add_custom_command(
      OUTPUT "${GHI_R5_WORLD_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_R5_WORLD_SHADER_ROOT}/package.json"
              --output "${GHI_R5_WORLD_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_R5_WORLD_SHADER_SOURCES}
      COMMENT "Building deterministic R5c-e world GHI shader package"
      VERBATIM
      )
  set(GHI_P0_ENVIRONMENT_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/p0_environment")
  set(GHI_P0_ENVIRONMENT_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/p0_environment.llghisp")
  set(GHI_P0_ENVIRONMENT_SHADER_SOURCES
      "${GHI_P0_ENVIRONMENT_SHADER_ROOT}/package.json"
      "${GHI_P0_ENVIRONMENT_SHADER_ROOT}/opengl41/environment.vert.glsl"
      "${GHI_P0_ENVIRONMENT_SHADER_ROOT}/opengl41/environment.frag.glsl"
      "${GHI_P0_ENVIRONMENT_SHADER_ROOT}/vulkan/environment.vert.glsl"
      "${GHI_P0_ENVIRONMENT_SHADER_ROOT}/vulkan/environment.frag.glsl"
      "${GHI_P0_ENVIRONMENT_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl")
  add_custom_command(
      OUTPUT "${GHI_P0_ENVIRONMENT_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_P0_ENVIRONMENT_SHADER_ROOT}/package.json"
              --output "${GHI_P0_ENVIRONMENT_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_P0_ENVIRONMENT_SHADER_SOURCES}
      COMMENT "Building deterministic P0e2 environment GHI shader package"
      VERBATIM)
  set(GHI_P0_WATER_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/p0_water")
  set(GHI_P0_WATER_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/p0_water.llghisp")
  set(GHI_P0_WATER_SHADER_SOURCES
      "${GHI_P0_WATER_SHADER_ROOT}/package.json"
      "${GHI_P0_WATER_SHADER_ROOT}/opengl41/water.vert.glsl"
      "${GHI_P0_WATER_SHADER_ROOT}/opengl41/water.frag.glsl"
      "${GHI_P0_WATER_SHADER_ROOT}/vulkan/water.vert.glsl"
      "${GHI_P0_WATER_SHADER_ROOT}/vulkan/water.frag.glsl"
      "${GHI_P0_WATER_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl")
  add_custom_command(
      OUTPUT "${GHI_P0_WATER_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_P0_WATER_SHADER_ROOT}/package.json"
              --output "${GHI_P0_WATER_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_P0_WATER_SHADER_SOURCES}
      COMMENT "Building deterministic P0e2d water GHI shader package"
      VERBATIM)
  set(GHI_R6_PPLL_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/r6_ppll")
  set(GHI_R6_PPLL_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/r6_ppll.llghisp")
  set(GHI_R6_PPLL_SHADER_SOURCES
      "${GHI_R6_PPLL_SHADER_ROOT}/package.json"
      "${GHI_R6_PPLL_SHADER_ROOT}/opengl41/ppll.vert.glsl"
      "${GHI_R6_PPLL_SHADER_ROOT}/opengl41/ppll.frag.glsl"
      "${GHI_R6_PPLL_SHADER_ROOT}/opengl44/ppll.vert.glsl"
      "${GHI_R6_PPLL_SHADER_ROOT}/opengl44/ppll.frag.glsl"
      "${GHI_R6_PPLL_SHADER_ROOT}/vulkan/ppll.vert.glsl"
      "${GHI_R6_PPLL_SHADER_ROOT}/vulkan/ppll.frag.glsl"
      "${GHI_R6_PPLL_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl"
      )
  add_custom_command(
      OUTPUT "${GHI_R6_PPLL_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_R6_PPLL_SHADER_ROOT}/package.json"
              --output "${GHI_R6_PPLL_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_R6_PPLL_SHADER_SOURCES}
      COMMENT "Building deterministic R6 PPLL GHI shader package"
      VERBATIM
      )
  set(GHI_R6_PEEL_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/r6_depth_peel")
  set(GHI_R6_PEEL_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/r6_depth_peel.llghisp")
  set(GHI_R6_PEEL_SHADER_SOURCES
      "${GHI_R6_PEEL_SHADER_ROOT}/package.json"
      "${GHI_R6_PEEL_SHADER_ROOT}/opengl41/depth_peel.vert.glsl"
      "${GHI_R6_PEEL_SHADER_ROOT}/opengl41/depth_peel.frag.glsl"
      "${GHI_R6_PEEL_SHADER_ROOT}/opengl44/depth_peel.vert.glsl"
      "${GHI_R6_PEEL_SHADER_ROOT}/opengl44/depth_peel.frag.glsl"
      "${GHI_R6_PEEL_SHADER_ROOT}/vulkan/depth_peel.vert.glsl"
      "${GHI_R6_PEEL_SHADER_ROOT}/vulkan/depth_peel.frag.glsl"
      "${GHI_R6_PEEL_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl"
      )
  add_custom_command(
      OUTPUT "${GHI_R6_PEEL_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_R6_PEEL_SHADER_ROOT}/package.json"
              --output "${GHI_R6_PEEL_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_R6_PEEL_SHADER_SOURCES}
      COMMENT "Building deterministic R6 depth-peeling GHI shader package"
      VERBATIM
      )
  set(GHI_R6_LEGACY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/r6_legacy_alpha")
  set(GHI_R6_LEGACY_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/r6_legacy_alpha.llghisp")
  set(GHI_R6_LEGACY_SHADER_SOURCES
      "${GHI_R6_LEGACY_SHADER_ROOT}/package.json"
      "${GHI_R6_LEGACY_SHADER_ROOT}/opengl41/legacy_alpha.vert.glsl"
      "${GHI_R6_LEGACY_SHADER_ROOT}/opengl41/legacy_alpha.frag.glsl"
      "${GHI_R6_LEGACY_SHADER_ROOT}/opengl44/legacy_alpha.vert.glsl"
      "${GHI_R6_LEGACY_SHADER_ROOT}/opengl44/legacy_alpha.frag.glsl"
      "${GHI_R6_LEGACY_SHADER_ROOT}/vulkan/legacy_alpha.vert.glsl"
      "${GHI_R6_LEGACY_SHADER_ROOT}/vulkan/legacy_alpha.frag.glsl"
      "${GHI_R6_LEGACY_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl"
      )
  add_custom_command(
      OUTPUT "${GHI_R6_LEGACY_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_R6_LEGACY_SHADER_ROOT}/package.json"
              --output "${GHI_R6_LEGACY_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_R6_LEGACY_SHADER_SOURCES}
      COMMENT "Building deterministic R6 legacy-alpha GHI shader package"
      VERBATIM
      )
      set(GHI_P0_ALPHA_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/p0_alpha")
      set(GHI_P0_ALPHA_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/p0_alpha.llghisp")
      set(GHI_P0_ALPHA_SHADER_SOURCES
          "${GHI_P0_ALPHA_SHADER_ROOT}/package.json"
          "${GHI_P0_ALPHA_SHADER_ROOT}/opengl41/alpha.vert.glsl"
          "${GHI_P0_ALPHA_SHADER_ROOT}/opengl41/alpha.frag.glsl"
          "${GHI_P0_ALPHA_SHADER_ROOT}/opengl44/alpha.vert.glsl"
          "${GHI_P0_ALPHA_SHADER_ROOT}/opengl44/alpha.frag.glsl"
          "${GHI_P0_ALPHA_SHADER_ROOT}/vulkan/alpha.vert.glsl"
          "${GHI_P0_ALPHA_SHADER_ROOT}/vulkan/alpha.frag.glsl"
          "${GHI_P0_ALPHA_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl")
      add_custom_command(
          OUTPUT "${GHI_P0_ALPHA_SHADER_PACKAGE}"
          COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_P0_ALPHA_SHADER_ROOT}/package.json"
              --output "${GHI_P0_ALPHA_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
          DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_P0_ALPHA_SHADER_SOURCES}
          COMMENT "Building deterministic production alpha GHI shader package"
          VERBATIM)
  set(GHI_P0_ALPHA_LEGACY_SHADER_ROOT
      "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/p0_alpha_legacy")
  set(GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE
      "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/p0_alpha_legacy.llghisp")
  set(GHI_P0_ALPHA_LEGACY_SHADER_SOURCES
      "${GHI_P0_ALPHA_LEGACY_SHADER_ROOT}/package.json"
      "${GHI_P0_ALPHA_LEGACY_SHADER_ROOT}/opengl41/alpha.vert.glsl"
      "${GHI_P0_ALPHA_LEGACY_SHADER_ROOT}/opengl41/alpha.frag.glsl"
      "${GHI_P0_ALPHA_LEGACY_SHADER_ROOT}/opengl44/alpha.vert.glsl"
      "${GHI_P0_ALPHA_LEGACY_SHADER_ROOT}/opengl44/alpha.frag.glsl"
      "${GHI_P0_ALPHA_LEGACY_SHADER_ROOT}/vulkan/alpha.vert.glsl"
      "${GHI_P0_ALPHA_LEGACY_SHADER_ROOT}/vulkan/alpha.frag.glsl"
      "${GHI_P0_ALPHA_LEGACY_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl")
  add_custom_command(
      OUTPUT "${GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_P0_ALPHA_LEGACY_SHADER_ROOT}/package.json"
              --output "${GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_P0_ALPHA_LEGACY_SHADER_SOURCES}
      COMMENT "Building deterministic production legacy alpha GHI shader package"
      VERBATIM)
  set(GHI_R7_OFFSCREEN_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/r7_offscreen")
  set(GHI_R7_OFFSCREEN_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/r7_offscreen.llghisp")
  set(GHI_R7_OFFSCREEN_SHADER_SOURCES
      "${GHI_R7_OFFSCREEN_SHADER_ROOT}/package.json"
      "${GHI_R7_OFFSCREEN_SHADER_ROOT}/opengl41/offscreen.vert.glsl"
      "${GHI_R7_OFFSCREEN_SHADER_ROOT}/opengl41/offscreen.frag.glsl"
      "${GHI_R7_OFFSCREEN_SHADER_ROOT}/opengl44/offscreen.vert.glsl"
      "${GHI_R7_OFFSCREEN_SHADER_ROOT}/opengl44/offscreen.frag.glsl"
      "${GHI_R7_OFFSCREEN_SHADER_ROOT}/vulkan/offscreen.vert.glsl"
      "${GHI_R7_OFFSCREEN_SHADER_ROOT}/vulkan/offscreen.frag.glsl"
      "${GHI_R7_OFFSCREEN_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl"
      )
  add_custom_command(
      OUTPUT "${GHI_R7_OFFSCREEN_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_R7_OFFSCREEN_SHADER_ROOT}/package.json"
              --output "${GHI_R7_OFFSCREEN_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_R7_OFFSCREEN_SHADER_SOURCES}
      COMMENT "Building deterministic R7 offscreen GHI shader package"
      VERBATIM
      )
  set(GHI_R8_INTERACTION_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ghi/shaders/r8_interaction")
  set(GHI_R8_INTERACTION_SHADER_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/ghi_shaders/r8_interaction.llghisp")
  set(GHI_R8_INTERACTION_SHADER_SOURCES
      "${GHI_R8_INTERACTION_SHADER_ROOT}/package.json"
      "${GHI_R8_INTERACTION_SHADER_ROOT}/opengl41/interaction.vert.glsl"
      "${GHI_R8_INTERACTION_SHADER_ROOT}/opengl41/interaction.frag.glsl"
      "${GHI_R8_INTERACTION_SHADER_ROOT}/opengl44/interaction.vert.glsl"
      "${GHI_R8_INTERACTION_SHADER_ROOT}/opengl44/interaction.frag.glsl"
      "${GHI_R8_INTERACTION_SHADER_ROOT}/vulkan/interaction.vert.glsl"
      "${GHI_R8_INTERACTION_SHADER_ROOT}/vulkan/interaction.frag.glsl"
      "${GHI_R8_INTERACTION_SHADER_ROOT}/vulkan/ghi_vulkan_clip.glsl"
      )
  add_custom_command(
      OUTPUT "${GHI_R8_INTERACTION_SHADER_PACKAGE}"
      COMMAND "${PYTHON_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              --manifest "${GHI_R8_INTERACTION_SHADER_ROOT}/package.json"
              --output "${GHI_R8_INTERACTION_SHADER_PACKAGE}"
              --glslang "${GHI_GLSLANG_VALIDATOR}"
              --spirv-val "${GHI_SPIRV_VAL}"
              --spirv-opt "${GHI_SPIRV_OPT}"
              --spirv-reflect "${GHI_SPIRV_REFLECT}"
      DEPENDS "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
              ${GHI_R8_INTERACTION_SHADER_SOURCES}
      COMMENT "Building deterministic R8 interaction GHI shader package"
      VERBATIM
      )
  add_custom_target(llrender_ghi_shaders DEPENDS
      "${GHI_R3_SHADER_PACKAGE}"
      "${GHI_R4_SHADER_PACKAGE}"
      "${GHI_R4C_SHADER_PACKAGE}"
      "${GHI_R5A_SHADER_PACKAGE}"
      "${GHI_I6_TERRAIN_SHADER_PACKAGE}"
      "${GHI_I7_LIGHTING_SHADER_PACKAGE}"
      "${GHI_I7_PROJECTOR_SHADER_PACKAGE}"
      "${GHI_I7_SHADOW_SHADER_PACKAGE}"
      "${GHI_R5_WORLD_SHADER_PACKAGE}"
      "${GHI_P0_ENVIRONMENT_SHADER_PACKAGE}"
      "${GHI_P0_WATER_SHADER_PACKAGE}"
      "${GHI_R6_PPLL_SHADER_PACKAGE}"
      "${GHI_R6_PEEL_SHADER_PACKAGE}"
      "${GHI_R6_LEGACY_SHADER_PACKAGE}"
    "${GHI_P0_ALPHA_SHADER_PACKAGE}"
    "${GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE}"
      "${GHI_R7_OFFSCREEN_SHADER_PACKAGE}"
      "${GHI_R8_INTERACTION_SHADER_PACKAGE}"
      )
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
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_R5_WORLD_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_R4C_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_R4_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_R5A_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_R6_PPLL_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_R6_PEEL_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_R6_LEGACY_SHADER_ROOT}/package.json"
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
    # Keep the newest packages in separate commands. On Visual Studio the
    # aggregate custom-command line otherwise reaches the generator limit and
    # silently omits the tail commands.
    add_custom_target(
        llrender_ghi_p0_environment_shader_packager_test
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_P0_ENVIRONMENT_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        DEPENDS llrender_ghi_shaders
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
        COMMENT "Testing deterministic P0e2 environment GHI shader packaging"
        VERBATIM)
    add_custom_target(
        llrender_ghi_p0_water_shader_packager_test
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_P0_WATER_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        DEPENDS llrender_ghi_shaders
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
        COMMENT "Testing deterministic P0e2d water GHI shader packaging"
        VERBATIM)
        add_custom_target(
        llrender_ghi_p0_alpha_shader_packager_test
        COMMAND "${PYTHON_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
            --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
            --manifest "${GHI_P0_ALPHA_SHADER_ROOT}/package.json"
            --glslang "${GHI_GLSLANG_VALIDATOR}"
            --spirv-val "${GHI_SPIRV_VAL}"
            --spirv-opt "${GHI_SPIRV_OPT}"
            --spirv-reflect "${GHI_SPIRV_REFLECT}"
        DEPENDS llrender_ghi_shaders
            "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
        COMMENT "Testing deterministic P0e3 alpha GHI shader packaging"
        VERBATIM)
        add_custom_target(
        llrender_ghi_p0_alpha_legacy_shader_packager_test
        COMMAND "${PYTHON_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
            --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
            --manifest "${GHI_P0_ALPHA_LEGACY_SHADER_ROOT}/package.json"
            --glslang "${GHI_GLSLANG_VALIDATOR}"
            --spirv-val "${GHI_SPIRV_VAL}"
            --spirv-opt "${GHI_SPIRV_OPT}"
            --spirv-reflect "${GHI_SPIRV_REFLECT}"
        DEPENDS llrender_ghi_shaders
            "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
        COMMENT "Testing deterministic production legacy alpha shader packaging"
        VERBATIM)
    add_custom_target(
        llrender_ghi_r7_shader_packager_test
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_R7_OFFSCREEN_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        DEPENDS llrender_ghi_shaders
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
        COMMENT "Testing deterministic R7 GHI shader packaging"
        VERBATIM)
    add_custom_target(
        llrender_ghi_r8_shader_packager_test
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_R8_INTERACTION_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        DEPENDS llrender_ghi_shaders
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
        COMMENT "Testing deterministic R8 GHI shader packaging"
        VERBATIM)
    add_custom_target(
        llrender_ghi_i7_shader_packager_test
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_I7_LIGHTING_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        DEPENDS llrender_ghi_shaders
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
        COMMENT "Testing deterministic I7 deferred-lighting GHI shader packaging"
        VERBATIM)
    add_custom_target(
        llrender_ghi_i7_projector_shader_packager_test
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_I7_PROJECTOR_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        DEPENDS llrender_ghi_shaders
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
        COMMENT "Testing deterministic I7c projector-lighting GHI shader packaging"
        VERBATIM)
    add_custom_target(
        llrender_ghi_i7_shadow_shader_packager_test
        COMMAND "${PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
                --packer "${CMAKE_SOURCE_DIR}/../scripts/renderer/pack_ghi_shader.py"
                --manifest "${GHI_I7_SHADOW_SHADER_ROOT}/package.json"
                --glslang "${GHI_GLSLANG_VALIDATOR}"
                --spirv-val "${GHI_SPIRV_VAL}"
                --spirv-opt "${GHI_SPIRV_OPT}"
                --spirv-reflect "${GHI_SPIRV_REFLECT}"
        DEPENDS llrender_ghi_shaders
                "${CMAKE_SOURCE_DIR}/../scripts/renderer/test_ghi_shader_determinism.py"
        COMMENT "Testing deterministic I7d shadow GHI shader packaging"
        VERBATIM)
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

  add_executable(llrender_vulkan_opaque_harness
      ghi/backends/vulkan/llghivulkanopaqueharness.cpp
      )
  target_include_directories(llrender_vulkan_opaque_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_compile_definitions(llrender_vulkan_opaque_harness PRIVATE
      LL_GHI_R4_SHADER_PACKAGE="${GHI_R4_SHADER_PACKAGE}"
      )
  target_link_libraries(llrender_vulkan_opaque_harness
      llrender
      Vulkan::Vulkan
      )
  add_dependencies(llrender_vulkan_opaque_harness llrender_ghi_shaders)

  add_executable(llrender_vulkan_visibility_harness
      ghi/backends/vulkan/llghivulkanvisibilityharness.cpp
      )
  target_include_directories(llrender_vulkan_visibility_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_compile_definitions(llrender_vulkan_visibility_harness PRIVATE
      LL_GHI_R4C_SHADER_PACKAGE="${GHI_R4C_SHADER_PACKAGE}"
      )
  target_link_libraries(llrender_vulkan_visibility_harness
      llrender
      Vulkan::Vulkan
      )
  add_dependencies(llrender_vulkan_visibility_harness llrender_ghi_shaders)

  add_executable(llrender_vulkan_material_skin_harness
      ghi/backends/vulkan/llghivulkanmaterialskinharness.cpp
      )
  target_include_directories(llrender_vulkan_material_skin_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_compile_definitions(llrender_vulkan_material_skin_harness PRIVATE
      LL_GHI_R5A_SHADER_PACKAGE="${GHI_R5A_SHADER_PACKAGE}"
      )
  target_link_libraries(llrender_vulkan_material_skin_harness
      llrender
      Vulkan::Vulkan
      )
  add_dependencies(llrender_vulkan_material_skin_harness llrender_ghi_shaders)

  add_executable(llrender_vulkan_world_harness
      ghi/backends/vulkan/llghivulkanworldharness.cpp
      )
  target_include_directories(llrender_vulkan_world_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_compile_definitions(llrender_vulkan_world_harness PRIVATE
      LL_GHI_R5_WORLD_SHADER_PACKAGE="${GHI_R5_WORLD_SHADER_PACKAGE}"
      )
  target_link_libraries(llrender_vulkan_world_harness
      llrender
      Vulkan::Vulkan
      )
  add_dependencies(llrender_vulkan_world_harness llrender_ghi_shaders)

  add_executable(llrender_vulkan_ppll_harness
      ghi/backends/vulkan/llghivulkanppllharness.cpp
      )
  target_include_directories(llrender_vulkan_ppll_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_compile_definitions(llrender_vulkan_ppll_harness PRIVATE
      LL_GHI_R6_PPLL_SHADER_PACKAGE="${GHI_R6_PPLL_SHADER_PACKAGE}"
      )
  target_link_libraries(llrender_vulkan_ppll_harness
      llrender
      Vulkan::Vulkan
      )
  add_dependencies(llrender_vulkan_ppll_harness llrender_ghi_shaders)

  add_executable(llrender_vulkan_depth_peel_harness
      ghi/backends/vulkan/llghivulkandepthpeelharness.cpp
      )
  target_include_directories(llrender_vulkan_depth_peel_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
      )
  target_compile_definitions(llrender_vulkan_depth_peel_harness PRIVATE
      LL_GHI_R6_PEEL_SHADER_PACKAGE="${GHI_R6_PEEL_SHADER_PACKAGE}"
      )
  target_link_libraries(llrender_vulkan_depth_peel_harness llrender Vulkan::Vulkan)
  add_dependencies(llrender_vulkan_depth_peel_harness llrender_ghi_shaders)

  add_executable(llrender_vulkan_legacy_alpha_harness
      ghi/backends/vulkan/llghivulkanlegacyalphaharness.cpp)
  target_include_directories(llrender_vulkan_legacy_alpha_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
  target_compile_definitions(llrender_vulkan_legacy_alpha_harness PRIVATE
      LL_GHI_R6_LEGACY_SHADER_PACKAGE="${GHI_R6_LEGACY_SHADER_PACKAGE}")
  target_link_libraries(llrender_vulkan_legacy_alpha_harness llrender Vulkan::Vulkan)
  add_dependencies(llrender_vulkan_legacy_alpha_harness llrender_ghi_shaders)

  add_executable(llrender_vulkan_production_alpha_harness
      ghi/backends/vulkan/llghivulkanproductionalphaharness.cpp)
  target_include_directories(llrender_vulkan_production_alpha_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
  target_compile_definitions(llrender_vulkan_production_alpha_harness PRIVATE
      LL_GHI_P0_ALPHA_SHADER_PACKAGE="${GHI_P0_ALPHA_SHADER_PACKAGE}"
      LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE="${GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE}")
  target_link_libraries(llrender_vulkan_production_alpha_harness
      llrender Vulkan::Vulkan)
  add_dependencies(llrender_vulkan_production_alpha_harness llrender_ghi_shaders)

  add_executable(llrender_vulkan_offscreen_harness
      ghi/backends/vulkan/llghivulkanoffscreenharness.cpp)
  target_include_directories(llrender_vulkan_offscreen_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
  target_compile_definitions(llrender_vulkan_offscreen_harness PRIVATE
      LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE="${GHI_R7_OFFSCREEN_SHADER_PACKAGE}")
  target_link_libraries(llrender_vulkan_offscreen_harness llrender Vulkan::Vulkan)
  add_dependencies(llrender_vulkan_offscreen_harness llrender_ghi_shaders)

  add_executable(llrender_vulkan_interaction_harness
      ghi/backends/vulkan/llghivulkaninteractionharness.cpp)
  target_include_directories(llrender_vulkan_interaction_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
  target_compile_definitions(llrender_vulkan_interaction_harness PRIVATE
      LL_GHI_R8_INTERACTION_SHADER_PACKAGE="${GHI_R8_INTERACTION_SHADER_PACKAGE}")
  target_link_libraries(llrender_vulkan_interaction_harness llrender Vulkan::Vulkan)
  add_dependencies(llrender_vulkan_interaction_harness llrender_ghi_shaders)

  add_executable(llrender_vulkan_production_frame_harness
      tests/ghi/llghiproductionframeharness.cpp)
  target_include_directories(llrender_vulkan_production_frame_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
  target_compile_definitions(llrender_vulkan_production_frame_harness PRIVATE
      LL_GHI_PRODUCTION_VULKAN=1
      LL_GHI_PRODUCTION_OPAQUE_PACKAGE="${GHI_R4_SHADER_PACKAGE}"
      LL_GHI_PRODUCTION_MATERIAL_PACKAGE="${GHI_R5A_SHADER_PACKAGE}"
      LL_GHI_PRODUCTION_TERRAIN_PACKAGE="${GHI_I6_TERRAIN_SHADER_PACKAGE}"
      LL_GHI_PRODUCTION_LIGHTING_PACKAGE="${GHI_I7_LIGHTING_SHADER_PACKAGE}"
      LL_GHI_PRODUCTION_PROJECTOR_PACKAGE="${GHI_I7_PROJECTOR_SHADER_PACKAGE}"
      LL_GHI_PRODUCTION_SHADOW_PACKAGE="${GHI_I7_SHADOW_SHADER_PACKAGE}")
  target_link_libraries(llrender_vulkan_production_frame_harness
      llrender Vulkan::Vulkan)
  add_dependencies(llrender_vulkan_production_frame_harness llrender_ghi_shaders)
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

    add_executable(llrender_opengl_opaque_harness
        ghi/backends/opengl/llghiopenglopaqueharness.cpp
        )
    target_include_directories(llrender_opengl_opaque_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
        )
    target_compile_definitions(llrender_opengl_opaque_harness PRIVATE
        LL_GHI_R4_SHADER_PACKAGE="${GHI_R4_SHADER_PACKAGE}"
        )
    target_link_libraries(llrender_opengl_opaque_harness
        llrender
        OpenGL::GL
        )
    add_dependencies(llrender_opengl_opaque_harness llrender_ghi_shaders)

    add_executable(llrender_opengl_visibility_harness
        ghi/backends/opengl/llghiopenglvisibilityharness.cpp
        )
    target_include_directories(llrender_opengl_visibility_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
        )
    target_compile_definitions(llrender_opengl_visibility_harness PRIVATE
        LL_GHI_R4C_SHADER_PACKAGE="${GHI_R4C_SHADER_PACKAGE}"
        )
    target_link_libraries(llrender_opengl_visibility_harness
        llrender
        OpenGL::GL
        )
    add_dependencies(llrender_opengl_visibility_harness llrender_ghi_shaders)

    add_executable(llrender_opengl_material_skin_harness
        ghi/backends/opengl/llghiopenglmaterialskinharness.cpp
        )
    target_include_directories(llrender_opengl_material_skin_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
        )
    target_compile_definitions(llrender_opengl_material_skin_harness PRIVATE
        LL_GHI_R5A_SHADER_PACKAGE="${GHI_R5A_SHADER_PACKAGE}"
        )
    target_link_libraries(llrender_opengl_material_skin_harness
        llrender
        OpenGL::GL
        )
    add_dependencies(llrender_opengl_material_skin_harness llrender_ghi_shaders)

    add_executable(llrender_opengl_world_harness
        ghi/backends/opengl/llghiopenglworldharness.cpp
        )
    target_include_directories(llrender_opengl_world_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
        )
    target_compile_definitions(llrender_opengl_world_harness PRIVATE
        LL_GHI_R5_WORLD_SHADER_PACKAGE="${GHI_R5_WORLD_SHADER_PACKAGE}"
        )
    target_link_libraries(llrender_opengl_world_harness
        llrender
        OpenGL::GL
        )
    add_dependencies(llrender_opengl_world_harness llrender_ghi_shaders)

    add_executable(llrender_opengl_ppll_harness
        ghi/backends/opengl/llghiopenglppllharness.cpp
        )
    target_include_directories(llrender_opengl_ppll_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
        )
    target_compile_definitions(llrender_opengl_ppll_harness PRIVATE
        LL_GHI_R6_PPLL_SHADER_PACKAGE="${GHI_R6_PPLL_SHADER_PACKAGE}"
        )
    target_link_libraries(llrender_opengl_ppll_harness
        llrender
        OpenGL::GL
        )
    add_dependencies(llrender_opengl_ppll_harness llrender_ghi_shaders)

    add_executable(llrender_opengl_depth_peel_harness
        ghi/backends/opengl/llghiopengldepthpeelharness.cpp
        )
    target_include_directories(llrender_opengl_depth_peel_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include
        )
    target_compile_definitions(llrender_opengl_depth_peel_harness PRIVATE
        LL_GHI_R6_PEEL_SHADER_PACKAGE="${GHI_R6_PEEL_SHADER_PACKAGE}"
        )
    target_link_libraries(llrender_opengl_depth_peel_harness llrender OpenGL::GL)
    add_dependencies(llrender_opengl_depth_peel_harness llrender_ghi_shaders)

    add_executable(llrender_opengl_legacy_alpha_harness
        ghi/backends/opengl/llghiopengllegacyalphaharness.cpp)
    target_include_directories(llrender_opengl_legacy_alpha_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
    target_compile_definitions(llrender_opengl_legacy_alpha_harness PRIVATE
        LL_GHI_R6_LEGACY_SHADER_PACKAGE="${GHI_R6_LEGACY_SHADER_PACKAGE}")
    target_link_libraries(llrender_opengl_legacy_alpha_harness llrender OpenGL::GL)
    add_dependencies(llrender_opengl_legacy_alpha_harness llrender_ghi_shaders)

    add_executable(llrender_opengl_production_alpha_harness
        ghi/backends/opengl/llghiopenglproductionalphaharness.cpp)
    target_include_directories(llrender_opengl_production_alpha_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
    target_compile_definitions(llrender_opengl_production_alpha_harness PRIVATE
        LL_GHI_P0_ALPHA_SHADER_PACKAGE="${GHI_P0_ALPHA_SHADER_PACKAGE}"
        LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE="${GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE}")
    target_link_libraries(llrender_opengl_production_alpha_harness
        llrender OpenGL::GL)
    add_dependencies(llrender_opengl_production_alpha_harness llrender_ghi_shaders)

    add_executable(llrender_opengl_offscreen_harness
        ghi/backends/opengl/llghiopengloffscreenharness.cpp)
    target_include_directories(llrender_opengl_offscreen_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
    target_compile_definitions(llrender_opengl_offscreen_harness PRIVATE
        LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE="${GHI_R7_OFFSCREEN_SHADER_PACKAGE}")
    target_link_libraries(llrender_opengl_offscreen_harness llrender OpenGL::GL)
    add_dependencies(llrender_opengl_offscreen_harness llrender_ghi_shaders)

    add_executable(llrender_opengl_interaction_harness
        ghi/backends/opengl/llghiopenglinteractionharness.cpp)
    target_include_directories(llrender_opengl_interaction_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
    target_compile_definitions(llrender_opengl_interaction_harness PRIVATE
        LL_GHI_R8_INTERACTION_SHADER_PACKAGE="${GHI_R8_INTERACTION_SHADER_PACKAGE}")
    target_link_libraries(llrender_opengl_interaction_harness llrender OpenGL::GL)
    add_dependencies(llrender_opengl_interaction_harness llrender_ghi_shaders)

    add_executable(llrender_opengl_production_frame_harness
        tests/ghi/llghiproductionframeharness.cpp)
    target_include_directories(llrender_opengl_production_frame_harness PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
    target_compile_definitions(llrender_opengl_production_frame_harness PRIVATE
        LL_GHI_PRODUCTION_OPENGL=1
        LL_GHI_PRODUCTION_OPAQUE_PACKAGE="${GHI_R4_SHADER_PACKAGE}"
        LL_GHI_PRODUCTION_MATERIAL_PACKAGE="${GHI_R5A_SHADER_PACKAGE}"
        LL_GHI_PRODUCTION_TERRAIN_PACKAGE="${GHI_I6_TERRAIN_SHADER_PACKAGE}"
        LL_GHI_PRODUCTION_LIGHTING_PACKAGE="${GHI_I7_LIGHTING_SHADER_PACKAGE}"
        LL_GHI_PRODUCTION_PROJECTOR_PACKAGE="${GHI_I7_PROJECTOR_SHADER_PACKAGE}"
        LL_GHI_PRODUCTION_SHADOW_PACKAGE="${GHI_I7_SHADOW_SHADER_PACKAGE}")
    target_link_libraries(llrender_opengl_production_frame_harness
        llrender OpenGL::GL)
    add_dependencies(llrender_opengl_production_frame_harness llrender_ghi_shaders)

    add_executable(llrender_environment_packet_inspector
        tests/ghi/llghienvironmentpacketinspector.cpp)
    target_include_directories(llrender_environment_packet_inspector PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
    target_link_libraries(llrender_environment_packet_inspector llrender)

    add_executable(llrender_alpha_packet_inspector
        tests/ghi/llghialphapacketinspector.cpp)
    target_include_directories(llrender_alpha_packet_inspector PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/ghi/include)
    target_link_libraries(llrender_alpha_packet_inspector llrender)
  endif ()
endif ()

if (LL_TESTS)
  add_executable(llrender_ghi_contract_test
      ghi/core/llghidevice.cpp
      ghi/core/llghienvironmentscenepacket.cpp
      ghi/core/llghihash.cpp
      ghi/core/llghilightingscenepacket.cpp
      ghi/core/llghilightingpacketconsumer.cpp
      ghi/core/llghialphascenepacket.cpp
      ghi/core/llghimaterialscenepacket.cpp
    ghi/core/llghiproductionalphaexecutor.cpp
      ghi/core/llghiproductionenvironmentexecutor.cpp
      ghi/core/llghiproductionwaterexecutor.cpp
      ghi/core/llghiproductionframeconsumer.cpp
      ghi/core/llghiproductiongbufferexecutor.cpp
      ghi/core/llghiproductionlightingexecutor.cpp
      ghi/core/llghiproductionframepacket.cpp
      ghi/core/llghiproductionframetargets.cpp
      ghi/core/llghiproductiontextureresidency.cpp
      ghi/core/llghiterrainscenepacket.cpp
      ghi/core/llghiterrainoffscreenprobe.cpp
      ghi/core/llghimaterialoffscreenprobe.cpp
      ghi/core/llghiopaqueoffscreenprobe.cpp
      ghi/core/llghiopaquepacketconsumer.cpp
      ghi/core/llghiopaquescenepacket.cpp
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
        LL_GHI_R4_SHADER_PACKAGE="${GHI_R4_SHADER_PACKAGE}"
        LL_GHI_R5A_SHADER_PACKAGE="${GHI_R5A_SHADER_PACKAGE}"
        LL_GHI_R5_WORLD_SHADER_PACKAGE="${GHI_R5_WORLD_SHADER_PACKAGE}"
        LL_GHI_I6_TERRAIN_SHADER_PACKAGE="${GHI_I6_TERRAIN_SHADER_PACKAGE}"
        LL_GHI_I7_LIGHTING_SHADER_PACKAGE="${GHI_I7_LIGHTING_SHADER_PACKAGE}"
        LL_GHI_I7_PROJECTOR_SHADER_PACKAGE="${GHI_I7_PROJECTOR_SHADER_PACKAGE}"
        LL_GHI_I7_SHADOW_SHADER_PACKAGE="${GHI_I7_SHADOW_SHADER_PACKAGE}"
        LL_GHI_R6_PPLL_SHADER_PACKAGE="${GHI_R6_PPLL_SHADER_PACKAGE}"
        LL_GHI_R6_PEEL_SHADER_PACKAGE="${GHI_R6_PEEL_SHADER_PACKAGE}"
        LL_GHI_R6_LEGACY_SHADER_PACKAGE="${GHI_R6_LEGACY_SHADER_PACKAGE}"
        LL_GHI_P0_ALPHA_SHADER_PACKAGE="${GHI_P0_ALPHA_SHADER_PACKAGE}"
        LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE="${GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE}"
        LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE="${GHI_R7_OFFSCREEN_SHADER_PACKAGE}"
        LL_GHI_R8_INTERACTION_SHADER_PACKAGE="${GHI_R8_INTERACTION_SHADER_PACKAGE}"
        )
    add_dependencies(llrender_ghi_contract_test
        llrender_ghi_shader_packager_test
        llrender_ghi_p0_environment_shader_packager_test
        llrender_ghi_p0_water_shader_packager_test
        llrender_ghi_p0_alpha_shader_packager_test
        llrender_ghi_p0_alpha_legacy_shader_packager_test
        llrender_ghi_i7_shader_packager_test
        llrender_ghi_i7_projector_shader_packager_test
        llrender_ghi_i7_shadow_shader_packager_test
        llrender_ghi_r7_shader_packager_test
        llrender_ghi_r8_shader_packager_test)
  endif ()
  target_link_libraries(llrender_ghi_contract_test
      llcommon
      llmath
      ll::openssl
      )
  add_test(NAME llrender_ghi_contract COMMAND llrender_ghi_contract_test)
endif (LL_TESTS)
