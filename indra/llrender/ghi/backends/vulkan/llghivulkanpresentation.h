/**
 * @file llghivulkanpresentation.h
 * @brief Private native-Vulkan presentation factory.
 */

#ifndef LL_LLGHIVULKANPRESENTATION_H
#define LL_LLGHIVULKANPRESENTATION_H

#include "ghi/include/llghipresentation.h"

namespace LL::GHI
{

PresentationCreationResult createVulkanPresentationSurface(
    const PresentationCreateInfo& info);

} // namespace LL::GHI

#endif // LL_LLGHIVULKANPRESENTATION_H
