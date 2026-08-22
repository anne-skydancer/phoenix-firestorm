/**
 * @file llghinestedviewfixture.h
 * @brief Backend-independent bounded replay of a nested-view scene packet.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHINESTEDVIEWFIXTURE_H
#define LL_LLGHINESTEDVIEWFIXTURE_H

#include "ghi/core/llghihash.h"
#include "ghi/include/llghi.h"
#include "ghi/include/llghinestedviewscenepacket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace LL::GHI::Test
{

inline constexpr std::size_t NESTED_VIEW_REPLAY_MAX_PASSES = 64;

struct NestedViewFixtureResult
{
    bool passed = false;
    std::string message;
    std::string packetSha256;
    std::string imageSha256;
    std::size_t passCount = 0;
    std::uint32_t shadedPixelCount = 0;
    std::array<std::size_t, RENDER_VIEW_CLASS_COUNT> views{};
};

inline NestedViewFixtureResult runNestedViewFixture(
    Device& device, const NestedViewScenePacket& packet)
{
    constexpr std::uint32_t extent = 8;
    constexpr std::uint64_t passBytes = extent * extent * 4u;

    NestedViewFixtureResult result;
    auto fail = [&](const char* operation, const Status& status)
    {
        result.message = std::string(operation) + ": " + status.message();
        return result;
    };
    Status status = validateNestedViewScenePacket(packet);
    if (!status) return fail("validate P0e4 nested-view packet", status);
    if (packet.passes.size() > NESTED_VIEW_REPLAY_MAX_PASSES)
        return fail("bound P0e4 nested-view replay", Status::failure(
            StatusCode::InvalidArgument, "packet exceeds 64-pass replay bound"));

    result.packetSha256 = nestedViewScenePacketSha256(packet);
    result.passCount = packet.passes.size();
    for (const NestedViewPass& nested : packet.passes)
        ++result.views[static_cast<std::size_t>(nested.pass.view)];

    std::vector<ImageHandle> images;
    std::vector<ImageViewHandle> views;
    std::vector<ImageViewHandle> passViews(packet.passes.size());
    std::vector<std::size_t> imagePassOffsets;
    std::vector<std::uint16_t> imageLayers;
    for (std::size_t index = 0; index < packet.passes.size();)
    {
        const bool cube = isCubeView(packet.passes[index].pass.view);
        const std::uint16_t layers = cube ? 6 : 1;
        ImageDesc imageDesc;
        imageDesc.extent = {extent, extent, 1};
        imageDesc.format = Format::RGBA8UNorm;
        imageDesc.usage = ResourceUsage::ColorAttachment | ResourceUsage::TransferSource;
        imageDesc.arrayLayers = layers;
        imageDesc.cubeCompatible = cube;
        ImageHandle image = device.createImage(imageDesc, status);
        if (!status) return fail("create P0e4 nested-view target", status);
        images.push_back(image);
        imagePassOffsets.push_back(index);
        imageLayers.push_back(layers);
        for (std::uint16_t layer = 0; layer < layers; ++layer)
        {
            ImageViewHandle view = device.createImageView(
                {image, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, layer, 1},
                 ImageViewType::Texture2D}, status);
            if (!status) return fail("create P0e4 nested-view target layer", status);
            views.push_back(view);
            passViews[index + layer] = view;
        }
        index += layers;
    }

    const std::uint64_t readbackBytes = packet.passes.size() * passBytes;
    BufferHandle readback = device.createBuffer(
        {readbackBytes, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
    if (!status) return fail("create P0e4 nested-view readback", status);

    const auto channel = [](std::uint64_t value)
    {
        return static_cast<float>((value % 251u) + 1u) / 255.f;
    };
    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame()))
        return fail("begin P0e4 nested-view replay", status);
    for (std::size_t index = 0; index < packet.passes.size(); ++index)
    {
        const NestedViewPass& nested = packet.passes[index];
        RenderingInfo rendering;
        rendering.semanticId = nested.semanticId;
        rendering.width = extent;
        rendering.height = extent;
        ClearValue clear;
        clear.color = {
            channel(static_cast<std::uint8_t>(nested.pass.view) * 37u +
                    static_cast<std::uint8_t>(nested.pass.face)),
            channel(nested.resourceGeneration),
            channel(nested.semanticId >> 8u),
            1.f};
        rendering.colors.push_back(
            {passViews[index], Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store, clear});
        if (!(status = commands.beginRendering(rendering)) ||
            !(status = commands.endRendering()))
            return fail("render P0e4 nested-view pass", status);
    }
    for (std::size_t imageIndex = 0; imageIndex < images.size(); ++imageIndex)
    {
        BufferImageCopyRegion copy;
        copy.bufferOffset = imagePassOffsets[imageIndex] * passBytes;
        copy.imageSubresource = {ImageAspect::Color, 0, 0, imageLayers[imageIndex]};
        copy.imageExtent = {extent, extent, 1};
        if (!(status = commands.copyImageToBuffer(
                images[imageIndex], readback,
                std::array<BufferImageCopyRegion, 1>{{copy}})))
            return fail("copy P0e4 nested-view target", status);
    }
    if (!(status = commands.endFrame()))
        return fail("finish P0e4 nested-view replay", status);

    std::vector<std::byte> pixels(readbackBytes);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do
    {
        status = device.readBuffer(readback, 0, pixels);
        if (status || status.code() != StatusCode::NotReady) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!status) return fail("read P0e4 nested-view images", status);

    if (device.backend() == Backend::OpenGL)
    {
        constexpr std::size_t rowBytes = extent * 4u;
        for (std::size_t pass = 0; pass < packet.passes.size(); ++pass)
        {
            const std::size_t base = pass * passBytes;
            for (std::uint32_t y = 0; y < extent / 2u; ++y)
                std::swap_ranges(pixels.begin() + base + y * rowBytes,
                    pixels.begin() + base + (y + 1u) * rowBytes,
                    pixels.begin() + base + (extent - 1u - y) * rowBytes);
        }
    }
    result.imageSha256 = sha256(pixels);
    for (std::size_t pixel = 0; pixel < packet.passes.size() * extent * extent; ++pixel)
        if (pixels[pixel * 4u + 3u] != std::byte{0}) ++result.shadedPixelCount;
    const std::uint32_t expectedCoverage =
        static_cast<std::uint32_t>(packet.passes.size() * extent * extent);
    if (result.shadedPixelCount != expectedCoverage)
        return fail("verify P0e4 nested-view coverage", Status::failure(
            StatusCode::BackendError, std::to_string(result.shadedPixelCount)));

    for (ImageViewHandle view : views)
        if (!(status = device.destroy(view)))
            return fail("destroy P0e4 nested-view target layer", status);
    for (ImageHandle image : images)
        if (!(status = device.destroy(image)))
            return fail("destroy P0e4 nested-view target", status);
    if (!(status = device.destroy(readback)) || !(status = device.waitIdle()))
        return fail("retire P0e4 nested-view replay", status);

    result.passed = true;
    result.message = "P0e4 bounded nested-view replay PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHINESTEDVIEWFIXTURE_H