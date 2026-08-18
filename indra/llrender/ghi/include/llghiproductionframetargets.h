/**
 * @file llghiproductionframetargets.h
 * @brief Shared private attachment ownership for an I8 production frame graph.
 */

#ifndef LL_LLGHIPRODUCTIONFRAMETARGETS_H
#define LL_LLGHIPRODUCTIONFRAMETARGETS_H

#include "llghiproductionframepacket.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace LL::GHI
{

class Device;

inline constexpr std::size_t PRODUCTION_GBUFFER_TARGETS = 4;
inline constexpr std::size_t PRODUCTION_SHADOW_TARGETS =
    LIGHTING_DIRECTIONAL_SHADOW_CASCADES + LIGHTING_PROJECTOR_SHADOWS;

struct ProductionFrameTargetLimits
{
    std::uint32_t maxWidth = 512;
    std::uint32_t maxHeight = 512;
    std::uint64_t maxPixels = 512ull * 512ull;
    std::uint64_t maxBytes = 64ull * 1024ull * 1024ull;
};

struct ProductionFrameTargetSet
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t generation = 0;
    ProductionFramePassMask passes = 0;
    std::array<ImageHandle, PRODUCTION_GBUFFER_TARGETS> gbufferImages{};
    std::array<ImageViewHandle, PRODUCTION_GBUFFER_TARGETS> gbufferViews{};
    ImageHandle depthImage;
    ImageViewHandle depthView;
    ImageHandle lightingImage;
    ImageViewHandle lightingView;
    std::array<ImageHandle, PRODUCTION_SHADOW_TARGETS> shadowImages{};
    std::array<ImageViewHandle, PRODUCTION_SHADOW_TARGETS> shadowViews{};
};

struct ProductionFrameTargetResult
{
    std::uint64_t frameId = 0;
    std::uint64_t assemblyEpoch = 0;
    std::uint64_t targetGeneration = 0;
    std::uint64_t allocatedBytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t imageCount = 0;
    std::uint32_t passCount = 0;
    bool reused = false;
};

class ProductionFrameTargets
{
public:
    explicit ProductionFrameTargets(Device& device);
    ~ProductionFrameTargets();

    ProductionFrameTargets(const ProductionFrameTargets&) = delete;
    ProductionFrameTargets& operator=(const ProductionFrameTargets&) = delete;

    Status ensure(const ProductionFramePacket& frame,
                  const ProductionFrameTargetLimits& limits,
                  ProductionFrameTargetResult& result);
    const ProductionFrameTargetSet& targets() const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONFRAMETARGETS_H
