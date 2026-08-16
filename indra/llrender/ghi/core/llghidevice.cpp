/**
 * @file llghidevice.cpp
 * @brief Backend-neutral GHI device factory.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "llghidevicebackend.h"

#ifndef LL_GHI_OPENGL_DEVICE
#define LL_GHI_OPENGL_DEVICE 0
#endif
#ifndef LL_GHI_VULKAN_DEVICE
#define LL_GHI_VULKAN_DEVICE 0
#endif

namespace LL::GHI
{

DeviceCreationResult createDevice(const DeviceCreateInfo& info)
{
    if (info.framesInFlight == 0 || info.framesInFlight > 16)
    {
        return {
            nullptr,
            Status::failure(
                StatusCode::InvalidArgument,
                "framesInFlight must be between one and sixteen")};
    }

    switch (info.backend)
    {
    case Backend::Validation:
        return createValidationDevice(info);
    case Backend::OpenGL:
#if LL_GHI_OPENGL_DEVICE
        return createOpenGLDevice(info);
#else
        return {nullptr, Status::failure(StatusCode::Unsupported,
            "the OpenGL GHI device is not compiled for this platform")};
#endif
    case Backend::Vulkan:
#if LL_GHI_VULKAN_DEVICE
        return createVulkanDevice(info);
#else
        return {nullptr, Status::failure(StatusCode::Unsupported,
            "the Vulkan GHI resource device is not compiled")};
#endif
    }

    return {nullptr, Status::failure(StatusCode::InvalidArgument,
        "unknown GHI backend")};
}

} // namespace LL::GHI
