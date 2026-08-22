# -*- cmake -*-

# Vulkan Storm deliberately does not select Kakadu, even when other proprietary
# packages are available. VulkanStormCodecPolicy.cmake rejects USE_KDU=ON.

set( ND_KDU_SUFFIX "" )
if( ADDRESS_SIZE EQUAL 64 )
  if( WINDOWS OR LINUX )
    set( ND_KDU_SUFFIX "_x64" )
  endif( WINDOWS OR LINUX )
endif( ADDRESS_SIZE EQUAL 64 )
    
include_guard()
add_library( ll::kdu INTERFACE IMPORTED )

if (USE_KDU)
  include(Prebuilt)
  use_prebuilt_binary(kdu)

  if (WINDOWS)
    find_library(KDU_LIBRARY
      NAMES
      kdu
      kdu${ND_KDU_SUFFIX} // <FS> FS-specific naming
      PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

  else (WINDOWS)
    find_library(KDU_LIBRARY
      NAMES
      libkdu.a
      libkdu${ND_KDU_SUFFIX}.a // <FS> FS-specific naming
      PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

  endif (WINDOWS)

  target_link_libraries(ll::kdu INTERFACE ${KDU_LIBRARY})

  target_include_directories( ll::kdu SYSTEM INTERFACE
          ${AUTOBUILD_INSTALL_DIR}/include/kdu
          ${LIBS_OPEN_DIR}/llkdu
          )
  target_compile_definitions(ll::kdu INTERFACE KDU_NO_THREADS=1)
endif (USE_KDU)
