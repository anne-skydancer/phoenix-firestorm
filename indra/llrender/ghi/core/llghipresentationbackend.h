/**
 * @file llghipresentationbackend.h
 * @brief Native-free private declarations for presentation backend factories.
 */

#ifndef LL_LLGHIPRESENTATIONBACKEND_H
#define LL_LLGHIPRESENTATIONBACKEND_H

#include "ghi/include/llghipresentation.h"

namespace LL::GHI
{

PresentationCreationResult createOpenGLPresentationSurface(
    const PresentationCreateInfo& info);
PresentationCreationResult createVulkanPresentationSurface(
    const PresentationCreateInfo& info);

} // namespace LL::GHI

#endif // LL_LLGHIPRESENTATIONBACKEND_H
