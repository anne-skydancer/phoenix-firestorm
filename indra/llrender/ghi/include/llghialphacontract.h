/**
 * @file llghialphacontract.h
 * @brief Backend-neutral alpha routing and bounded OIT policy.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIALPHACONTRACT_H
#define LL_LLGHIALPHACONTRACT_H

#include "llghitypes.h"

#include <algorithm>
#include <cstdint>

namespace LL::GHI
{

enum class AlphaMethod : std::uint8_t
{
    LegacySorted,
    PPLL,
    DepthPeeling
};

enum class AlphaViewPhase : std::uint8_t
{
    MainPostWater,
    PreWater,
    HUD,
    Impostor,
    Reflection,
    CubeSnapshot,
    DynamicTexture,
    MediaSurface
};

enum class AlphaSubmissionClass : std::uint8_t
{
    Mask,
    StandardBlend,
    CustomBlend,
    Particle
};

enum class AlphaRoute : std::uint8_t
{
    Mask,
    LegacySorted,
    LegacyResidual,
    PPLLCapture,
    DepthPeelExact
};

struct AlphaSubmission
{
    AlphaViewPhase phase = AlphaViewPhase::MainPostWater;
    AlphaSubmissionClass classification = AlphaSubmissionClass::StandardBlend;
    bool rigged = false;
    bool fullbright = false;
    bool emissive = false;
};

struct AlphaRoutingState
{
    AlphaMethod requestedMethod = AlphaMethod::LegacySorted;
    bool ppllAvailable = false;
    bool depthPeelingAvailable = false;
    bool transientLoad = false;
};

struct AlphaRoutingDecision
{
    AlphaRoute route = AlphaRoute::LegacySorted;
    // Standard fullbright alpha follows the selected exact method. Its
    // independent glow contribution is replayed once after exact compositing.
    bool replayEmissiveContribution = false;

    friend bool operator==(const AlphaRoutingDecision&,
                           const AlphaRoutingDecision&) = default;
};

constexpr AlphaRoutingDecision routeAlphaSubmission(
    const AlphaSubmission& submission,
    const AlphaRoutingState& state)
{
    if (submission.classification == AlphaSubmissionClass::Mask)
        return {AlphaRoute::Mask, false};

    const bool residual =
        submission.classification == AlphaSubmissionClass::Particle ||
        submission.classification == AlphaSubmissionClass::CustomBlend;
    const bool exactView = submission.phase == AlphaViewPhase::MainPostWater;
    if (residual)
        return {AlphaRoute::LegacyResidual, submission.emissive};
    if (!exactView || state.transientLoad)
        return {AlphaRoute::LegacySorted, submission.emissive};

    if (state.requestedMethod == AlphaMethod::PPLL && state.ppllAvailable)
        return {AlphaRoute::PPLLCapture, submission.emissive};
    if (state.requestedMethod == AlphaMethod::DepthPeeling &&
        state.depthPeelingAvailable)
        return {AlphaRoute::DepthPeelExact, submission.emissive};
    return {AlphaRoute::LegacySorted, submission.emissive};
}

struct AlphaPPLLPolicy
{
    std::uint32_t nodesPerPixel = 8;
    std::uint32_t memoryMiB = 512;
    std::uint32_t exactLayersPerPixel = 24;
};

struct AlphaPPLLAllocation
{
    std::uint64_t nodeCapacity = 0;
    std::uint32_t exactLayersPerPixel = 0;
    bool usable = false;
};

// The accepted compact node is packed half-float RG/BA, depth, and next.
inline constexpr std::uint64_t ALPHA_PPLL_NODE_BYTES = 4u * sizeof(std::uint32_t);

constexpr AlphaPPLLAllocation planAlphaPPLLAllocation(
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t maximumStorageBufferBytes,
    AlphaPPLLPolicy policy)
{
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (!pixels || maximumStorageBufferBytes < ALPHA_PPLL_NODE_BYTES)
        return {};
    const std::uint64_t requested = pixels *
        std::clamp(policy.nodesPerPixel, std::uint32_t{1}, std::uint32_t{32});
    const std::uint64_t budget = static_cast<std::uint64_t>(
        std::clamp(policy.memoryMiB, std::uint32_t{32}, std::uint32_t{2048})) << 20;
    const std::uint64_t capacity = std::min(
        requested, std::min(budget, maximumStorageBufferBytes) /
                       ALPHA_PPLL_NODE_BYTES);
    return {capacity,
            std::clamp(policy.exactLayersPerPixel,
                       std::uint32_t{4}, std::uint32_t{32}),
            capacity >= pixels && capacity <= 0x7fffffffu};
}

struct AlphaDepthPeelPolicy
{
    std::uint32_t maximumLayers = 4;
    std::uint32_t submissionBudgetMilliseconds = 2;

    friend bool operator==(const AlphaDepthPeelPolicy&,
                           const AlphaDepthPeelPolicy&) = default;
};

constexpr AlphaDepthPeelPolicy clampAlphaDepthPeelPolicy(AlphaDepthPeelPolicy policy)
{
    return {std::clamp(policy.maximumLayers, std::uint32_t{1}, std::uint32_t{32}),
            std::clamp(policy.submissionBudgetMilliseconds,
                       std::uint32_t{1}, std::uint32_t{50})};
}

constexpr bool supportsPPLL(const RendererCapabilities& capabilities)
{
    // PPLL's semantic requirements are storage resources and image atomics,
    // which are core together in OpenGL 4.3. The production viewer's direct-GL
    // implementation deliberately has a stricter OpenGL 4.4 gate because it
    // uses glClearTexImage. Vulkan is evaluated solely by these capabilities.
    return capabilities.storageImageAtomics &&
           capabilities.maxStorageBuffersPerStage >= 2;
}

} // namespace LL::GHI

#endif // LL_LLGHIALPHACONTRACT_H
