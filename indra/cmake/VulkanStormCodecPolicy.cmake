# -*- cmake -*-

# Vulkan Storm distribution policy for JPEG 2000 codecs.
#
# PUBLIC is the safe default and is the only mode intended for distributable
# builds. Grok is deliberately gated behind the explicitly named PRIVATE mode.
set(VULKANSTORM_DISTRIBUTION "PUBLIC" CACHE STRING
    "Vulkan Storm distribution policy: PUBLIC or PRIVATE")
set_property(CACHE VULKANSTORM_DISTRIBUTION PROPERTY STRINGS PUBLIC PRIVATE)

option(USE_GROK "Use Grok as the J2C codec (PRIVATE builds only)" OFF)
option(USE_KDU "Use the Kakadu J2C codec (not permitted by Vulkan Storm policy)" OFF)

string(TOUPPER "${VULKANSTORM_DISTRIBUTION}" _vulkanstorm_distribution)
set(VULKANSTORM_DISTRIBUTION "${_vulkanstorm_distribution}" CACHE STRING
    "Vulkan Storm distribution policy: PUBLIC or PRIVATE" FORCE)

if (NOT VULKANSTORM_DISTRIBUTION MATCHES "^(PUBLIC|PRIVATE)$")
    message(FATAL_ERROR
        "VULKANSTORM_DISTRIBUTION must be PUBLIC or PRIVATE, not "
        "'${VULKANSTORM_DISTRIBUTION}'")
endif ()

if (USE_KDU)
    message(FATAL_ERROR
        "Kakadu is not an allowed Vulkan Storm JPEG 2000 backend. "
        "Use PUBLIC with OpenJPEG, or PRIVATE with -DUSE_GROK=ON.")
endif ()

if (USE_GROK AND NOT VULKANSTORM_DISTRIBUTION STREQUAL "PRIVATE")
    message(FATAL_ERROR
        "Grok is restricted to explicitly private, non-public builds. "
        "Use -DVULKANSTORM_DISTRIBUTION=PRIVATE -DUSE_GROK=ON, or disable "
        "Grok to make a PUBLIC OpenJPEG build.")
endif ()

if (USE_GROK)
    set(VULKANSTORM_J2C_BACKEND "Grok")
else ()
    set(VULKANSTORM_J2C_BACKEND "OpenJPEG")
endif ()

if (VULKANSTORM_DISTRIBUTION STREQUAL "PRIVATE")
    set(VULKANSTORM_PRIVATE_BUILD ON)
    message(WARNING
        "Vulkan Storm distribution: PRIVATE / NON-PUBLIC; "
        "JPEG 2000 backend: ${VULKANSTORM_J2C_BACKEND}")
else ()
    set(VULKANSTORM_PRIVATE_BUILD OFF)
    message(STATUS
        "Vulkan Storm distribution: PUBLIC; JPEG 2000 backend: OpenJPEG")
endif ()

unset(_vulkanstorm_distribution)
