# -*- cmake -*-
include(Prebuilt)

if (FREEBSD)
  use_prebuilt_common(vulkan_gltf)
else ()
  use_prebuilt_binary(vulkan_gltf)
endif ()

