/**
 * @file llghipresentation.cpp
 * @brief Backend-neutral presentation factory.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/include/llghipresentation.h"

#if LL_GHI_VULKAN
#include "ghi/backends/vulkan/llghivulkanpresentation.h"
#endif

namespace LL::GHI
{

PresentationCreationResult createPresentationSurface(const PresentationCreateInfo& info)
{
    if (info.backend != Backend::Vulkan)
    {
        return { nullptr, Status::failure(
            StatusCode::Unsupported,
            "R1 presentation factory currently accepts only the developer-gated Vulkan backend") };
    }

#if LL_GHI_VULKAN
    return createVulkanPresentationSurface(info);
#else
    return { nullptr, Status::failure(
        StatusCode::Unsupported,
        "Native Vulkan GHI support was not enabled for this build") };
#endif
}

bool presentationBackendBuilt(Backend backend)
{
#if LL_GHI_VULKAN
    return backend == Backend::Vulkan;
#else
    (void)backend;
    return false;
#endif
}

} // namespace LL::GHI
