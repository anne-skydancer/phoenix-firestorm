/**
 * @file llghiproductionframetargets.cpp
 * @brief I8c1 shared private production-frame attachment ownership.
 */

#include "linden_common.h"

#include "ghi/include/llghiproductionframetargets.h"

#include "ghi/include/llghidevice.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace LL::GHI
{
namespace
{
constexpr std::array<Format, PRODUCTION_GBUFFER_TARGETS> GBUFFER_FORMATS{{
    Format::RGBA8UNorm, Format::RGBA8UNorm,
    Format::RGBA16UNorm, Format::RGBA16Float}};
constexpr Format DEPTH_FORMAT = Format::Depth32Float;
constexpr Format LIGHTING_FORMAT = Format::RGBA16Float;
constexpr Format SHADOW_FORMAT = Format::Depth32Float;

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

Status unsupported(const char* message)
{
    return Status::failure(StatusCode::Unsupported, message);
}

std::pair<std::uint32_t, std::uint32_t> boundedExtent(
    std::uint32_t width, std::uint32_t height,
    const ProductionFrameTargetLimits& limits)
{
    const double widthScale = static_cast<double>(limits.maxWidth) / width;
    const double heightScale = static_cast<double>(limits.maxHeight) / height;
    const double pixelScale = std::sqrt(
        static_cast<double>(limits.maxPixels) /
        (static_cast<double>(width) * height));
    const double scale = std::min({1.0, widthScale, heightScale, pixelScale});
    return {
        std::max(1u, static_cast<std::uint32_t>(std::floor(width * scale))),
        std::max(1u, static_cast<std::uint32_t>(std::floor(height * scale)))};
}

std::uint32_t shadowCount(ProductionFramePassMask passes)
{
    std::uint32_t count = 0;
    if (productionFrameHasPass(passes,
                               ProductionFramePass::DirectionalShadow))
        count += LIGHTING_DIRECTIONAL_SHADOW_CASCADES;
    if (productionFrameHasPass(passes,
                               ProductionFramePass::ProjectorShadow))
        count += LIGHTING_PROJECTOR_SHADOWS;
    return count;
}

std::uint64_t targetBytes(std::uint32_t width, std::uint32_t height,
                          std::uint32_t shadows)
{
    constexpr std::uint64_t gbufferBytes = 4 + 4 + 8 + 8;
    constexpr std::uint64_t depthBytes = 4;
    constexpr std::uint64_t lightingBytes = 8;
    constexpr std::uint64_t shadowBytes = 4;
    return static_cast<std::uint64_t>(width) * height *
        (gbufferBytes + depthBytes + lightingBytes + shadows * shadowBytes);
}
} // namespace

class ProductionFrameTargets::Impl
{
public:
    explicit Impl(Device& device) : mDevice(device) {}

    Status ensure(const ProductionFramePacket& frame,
                  const ProductionFrameTargetLimits& limits,
                  ProductionFrameTargetResult& result)
    {
        result = {};
        result.frameId = frame.frameId;
        result.assemblyEpoch = frame.assemblyEpoch;
        if (!limits.maxWidth || !limits.maxHeight || !limits.maxPixels ||
            !limits.maxBytes)
            return invalid("production frame target limits must be nonzero");
        Status status = validateProductionFramePacket(frame);
        if (!status) return status;
        const auto [width, height] = boundedExtent(
            frame.sourceWidth, frame.sourceHeight, limits);
        const std::uint32_t shadows = shadowCount(frame.passes);
        const std::uint64_t bytes = targetBytes(width, height, shadows);
        if (bytes > limits.maxBytes)
            return unsupported("production frame targets exceed byte limit");
        const ProductionFramePassMask targetPasses = frame.passes &
            (productionFramePassBit(ProductionFramePass::DirectionalShadow) |
             productionFramePassBit(ProductionFramePass::ProjectorShadow) |
             productionFramePassBit(ProductionFramePass::DeferredLighting));
        if (mTargets.width == width && mTargets.height == height &&
            mTargets.passes == targetPasses)
        {
            fillResult(frame, bytes, shadows, true, result);
            return Status::success();
        }

        ProductionFrameTargetSet replacement;
        replacement.width = width;
        replacement.height = height;
        replacement.generation = mTargets.generation + 1;
        if (!replacement.generation) replacement.generation = 1;
        replacement.passes = targetPasses;
        for (std::size_t target = 0;
             status && target < PRODUCTION_GBUFFER_TARGETS; ++target)
        {
            replacement.gbufferImages[target] = mDevice.createImage(
                {{width, height, 1}, GBUFFER_FORMATS[target],
                 ResourceUsage::ColorAttachment | ResourceUsage::Sampled |
                     ResourceUsage::TransferSource,
                 1, 1, 1}, status);
            if (status) replacement.gbufferViews[target] =
                mDevice.createImageView(
                    {replacement.gbufferImages[target],
                     GBUFFER_FORMATS[target],
                     {ImageAspect::Color, 0, 1, 0, 1}}, status);
        }
        if (status) replacement.depthImage = mDevice.createImage(
            {{width, height, 1}, DEPTH_FORMAT,
             ResourceUsage::DepthStencilAttachment | ResourceUsage::Sampled |
                 ResourceUsage::TransferSource,
             1, 1, 1}, status);
        if (status) replacement.depthView = mDevice.createImageView(
            {replacement.depthImage, DEPTH_FORMAT,
             {ImageAspect::Depth, 0, 1, 0, 1}}, status);
        if (status) replacement.lightingImage = mDevice.createImage(
            {{width, height, 1}, LIGHTING_FORMAT,
             ResourceUsage::ColorAttachment | ResourceUsage::Sampled |
                 ResourceUsage::TransferSource,
             1, 1, 1}, status);
        if (status) replacement.lightingView = mDevice.createImageView(
            {replacement.lightingImage, LIGHTING_FORMAT,
             {ImageAspect::Color, 0, 1, 0, 1}}, status);
        auto createShadow = [&](std::size_t shadow)
        {
            replacement.shadowImages[shadow] = mDevice.createImage(
                {{width, height, 1}, SHADOW_FORMAT,
                 ResourceUsage::DepthStencilAttachment |
                     ResourceUsage::Sampled | ResourceUsage::TransferSource,
                 1, 1, 1}, status);
            if (status) replacement.shadowViews[shadow] =
                mDevice.createImageView(
                    {replacement.shadowImages[shadow], SHADOW_FORMAT,
                     {ImageAspect::Depth, 0, 1, 0, 1}}, status);
        };
        if (productionFrameHasPass(frame.passes,
                                   ProductionFramePass::DirectionalShadow))
            for (std::size_t shadow = 0;
                 status && shadow < LIGHTING_DIRECTIONAL_SHADOW_CASCADES;
                 ++shadow)
                createShadow(shadow);
        if (productionFrameHasPass(frame.passes,
                                   ProductionFramePass::ProjectorShadow))
            for (std::size_t slot = 0;
                 status && slot < LIGHTING_PROJECTOR_SHADOWS; ++slot)
                createShadow(LIGHTING_DIRECTIONAL_SHADOW_CASCADES + slot);
        if (!status)
        {
            destroy(replacement);
            return status;
        }

        ProductionFrameTargetSet previous = mTargets;
        mTargets = replacement;
        status = destroy(previous);
        if (!status) return status;
        fillResult(frame, bytes, shadows, false, result);
        return Status::success();
    }

    Status shutdown()
    {
        ProductionFrameTargetSet previous = mTargets;
        mTargets = {};
        return destroy(previous);
    }

    const ProductionFrameTargetSet& targets() const { return mTargets; }

private:
    void fillResult(const ProductionFramePacket& frame, std::uint64_t bytes,
                    std::uint32_t shadows, bool reused,
                    ProductionFrameTargetResult& result) const
    {
        result.frameId = frame.frameId;
        result.assemblyEpoch = frame.assemblyEpoch;
        result.targetGeneration = mTargets.generation;
        result.allocatedBytes = bytes;
        result.width = mTargets.width;
        result.height = mTargets.height;
        result.imageCount = static_cast<std::uint32_t>(
            PRODUCTION_GBUFFER_TARGETS + 2 + shadows);
        result.passCount = std::popcount(frame.passes);
        result.reused = reused;
    }

    Status destroy(ProductionFrameTargetSet& targets)
    {
        Status first = Status::success();
        auto destroyView = [&](ImageViewHandle& handle)
        {
            if (!handle) return;
            const Status status = mDevice.destroy(handle);
            if (first && !status) first = status;
            handle = {};
        };
        auto destroyImage = [&](ImageHandle& handle)
        {
            if (!handle) return;
            const Status status = mDevice.destroy(handle);
            if (first && !status) first = status;
            handle = {};
        };
        for (auto& view : targets.gbufferViews) destroyView(view);
        destroyView(targets.depthView);
        destroyView(targets.lightingView);
        for (auto& view : targets.shadowViews) destroyView(view);
        for (auto& image : targets.gbufferImages) destroyImage(image);
        destroyImage(targets.depthImage);
        destroyImage(targets.lightingImage);
        for (auto& image : targets.shadowImages) destroyImage(image);
        return first;
    }

    Device& mDevice;
    ProductionFrameTargetSet mTargets;
};

ProductionFrameTargets::ProductionFrameTargets(Device& device) :
    mImpl(std::make_unique<Impl>(device))
{
}

ProductionFrameTargets::~ProductionFrameTargets()
{
    if (mImpl) mImpl->shutdown();
}

Status ProductionFrameTargets::ensure(
    const ProductionFramePacket& frame,
    const ProductionFrameTargetLimits& limits,
    ProductionFrameTargetResult& result)
{
    return mImpl->ensure(frame, limits, result);
}

const ProductionFrameTargetSet& ProductionFrameTargets::targets() const
{
    return mImpl->targets();
}

Status ProductionFrameTargets::shutdown()
{
    return mImpl->shutdown();
}

} // namespace LL::GHI
