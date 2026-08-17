/**
 * @file llghiinteractionfixture.h
 * @brief Backend-independent R8 UI, HUD, picking, snapshot, and timing fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIINTERACTIONFIXTURE_H
#define LL_LLGHIINTERACTIONFIXTURE_H

#include "ghi/core/llghihash.h"
#include "ghi/include/llghi.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace LL::GHI::Test
{

struct InteractionFixtureResult
{
    bool passed = false;
    std::string message;
    std::string snapshotSha256;
    std::string selectionSha256;
    std::string pickDepthSha256;
    std::uint64_t gpuTicks = 0;
    double gpuNanoseconds = 0.0;
    std::uint32_t selectedPixelCount = 0;
};

inline InteractionFixtureResult runInteractionFixture(
    Device& device, const ShaderPackageDesc& shaderPackage)
{
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr std::uint64_t pixelBytes = width * height * 4u;
    constexpr const char* snapshotReference =
        "fa447dfa6cc02e12a93098086b57ef4ba64024b62e7aaafcb447f69f5cddf0cd";
    constexpr const char* selectionReference =
        "fd3304ed7606bde9eaa118143f4333f9907ad721ca5ed73cfb2606351421c9ea";
    constexpr const char* pickDepthReference =
        "89be3f6f79d4bb0ef7325b995f5fbf9ec10c9d73499e7addaef2256c3efa0be2";
    constexpr std::uint32_t selectedPixelReference = 1920;

    struct alignas(16) InteractionUniform
    {
        std::uint32_t config[4];
        float tint[4];
    };
    static_assert(sizeof(InteractionUniform) == 32);
    constexpr std::array<InteractionUniform, 3> uniforms{{
        InteractionUniform{{0u, 0u, 0u, 0u}, {0.f, 0.f, 0.f, 1.f}},
        InteractionUniform{{1u, 100u, 0u, 0u}, {0.95f, 0.25f, 0.10f, 0.55f}},
        InteractionUniform{{2u, 900u, 0u, 0u}, {0.20f, 0.85f, 1.00f, 0.70f}},
    }};
    constexpr std::uint64_t uniformBytes = sizeof(uniforms);

    InteractionFixtureResult result;
    auto fail = [&](const char* operation, const Status& status)
    {
        result.message = std::string(operation) + ": " + status.message();
        return result;
    };
    Status status = Status::success();
    if (!device.capabilities().timestampQueries)
        return fail("R8 timestamp capability missing", Status::failure(
            StatusCode::Unsupported, "timestamp queries are required"));
    if (!device.capabilities().independentBlend)
        return fail("R8 independent blend capability missing", Status::failure(
            StatusCode::Unsupported, "UI color and selection targets require independent blend"));

    std::vector<std::byte> uploadData(uniformBytes);
    std::memcpy(uploadData.data(), uniforms.data(), uniformBytes);
    BufferHandle upload = device.createBuffer(
        {uniformBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status || !(status = device.writeBuffer(upload, 0, uploadData)))
        return fail("prepare R8 interaction upload", status);
    std::array<BufferHandle, 3> uniformBuffers{};
    for (BufferHandle& uniform : uniformBuffers)
        uniform = device.createBuffer(
            {sizeof(InteractionUniform),
             ResourceUsage::Uniform | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);

    auto createTarget = [&](Format format, ImageViewHandle& view)
    {
        ImageHandle image = device.createImage(
            {{width, height, 1}, format,
             ResourceUsage::ColorAttachment | ResourceUsage::TransferSource,
             1, 1, 1}, status);
        view = device.createImageView(
            {image, format, {ImageAspect::Color, 0, 1, 0, 1},
             ImageViewType::Texture2D}, status);
        return image;
    };
    ImageViewHandle colorView;
    ImageViewHandle selectionView;
    ImageViewHandle pickDepthView;
    ImageHandle color = createTarget(Format::RGBA8UNorm, colorView);
    ImageHandle selection = createTarget(Format::R32UInt, selectionView);
    ImageHandle pickDepth = createTarget(Format::R32Float, pickDepthView);
    BufferHandle colorReadback = device.createBuffer(
        {pixelBytes, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
    BufferHandle selectionReadback = device.createBuffer(
        {pixelBytes, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
    BufferHandle pickDepthReadback = device.createBuffer(
        {pixelBytes, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
    QueryPoolHandle timestamps = device.createQueryPool({QueryType::Timestamp, 2}, status);
    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    if (!status) return fail("create R8 interaction resources", status);

    std::array<BindingSetHandle, 3> bindingSets{};
    for (std::size_t phase = 0; phase < bindingSets.size(); ++phase)
    {
        BindingSetDesc bindings;
        bindings.shader = shader;
        bindings.group = 0;
        bindings.resources = {{0, 0,
            ShaderPackageDesc::BindingType::UniformBuffer,
            uniformBuffers[phase], 0, sizeof(InteractionUniform), {}, {}}};
        bindingSets[phase] = device.createBindingSet(bindings, status);
    }

    auto createPipeline = [&](bool blendColor)
    {
        PipelineDesc desc;
        desc.shader = shader;
        desc.cullMode = CullMode::None;
        desc.depthTest = false;
        desc.depthWrite = false;
        desc.colorFormats = {
            Format::RGBA8UNorm, Format::R32UInt, Format::R32Float};
        desc.blendStates.resize(3);
        if (blendColor)
        {
            BlendState& blend = desc.blendStates[0];
            blend.enabled = true;
            blend.sourceColor = BlendFactor::SourceAlpha;
            blend.destinationColor = BlendFactor::OneMinusSourceAlpha;
            blend.sourceAlpha = BlendFactor::One;
            blend.destinationAlpha = BlendFactor::OneMinusSourceAlpha;
        }
        return device.createPipeline(desc, status);
    };
    PipelineHandle opaquePipeline = createPipeline(false);
    PipelineHandle overlayPipeline = createPipeline(true);
    if (!status) return fail("create R8 interaction pipelines", status);

    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame()))
        return fail("begin R8 interaction frame", status);
    for (std::size_t phase = 0; phase < uniformBuffers.size(); ++phase)
    {
        const BufferCopyRegion region{
            phase * sizeof(InteractionUniform), 0, sizeof(InteractionUniform)};
        if (!(status = commands.copyBuffer(
                upload, uniformBuffers[phase],
                std::array<BufferCopyRegion, 1>{{region}})))
            return fail("upload R8 interaction uniform", status);
    }
    if (!(status = commands.resetQueryPool(timestamps, 0, 2)) ||
        !(status = commands.writeTimestamp(timestamps, 0)))
        return fail("begin R8 profiling interval", status);

    RenderingInfo rendering;
    rendering.semanticId = 0x52385f5549504943ull;
    rendering.width = width;
    rendering.height = height;
    rendering.colors = {
        {colorView, Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store,
         {{0.f, 0.f, 0.f, 1.f}, 0.f, 0}},
        {selectionView, Format::R32UInt, LoadOp::Discard, StoreOp::Store, {}},
        {pickDepthView, Format::R32Float, LoadOp::Discard, StoreOp::Store, {}}};
    if (!(status = commands.beginRendering(rendering)) ||
        !(status = commands.setViewport(
            {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f})) ||
        !(status = commands.setScissor({0, 0, width, height})) ||
        !(status = commands.bindPipeline(opaquePipeline)) ||
        !(status = commands.bindBindingSet(0, bindingSets[0])) ||
        !(status = commands.draw({3, 1, 0, 0})) ||
        !(status = commands.setScissor({8, 12, 48, 40})) ||
        !(status = commands.bindPipeline(overlayPipeline)) ||
        !(status = commands.bindBindingSet(0, bindingSets[1])) ||
        !(status = commands.draw({3, 1, 0, 0})) ||
        !(status = commands.setScissor({16, 28, 32, 8})) ||
        !(status = commands.bindBindingSet(0, bindingSets[2])) ||
        !(status = commands.draw({3, 1, 0, 0})) ||
        !(status = commands.endRendering()) ||
        !(status = commands.writeTimestamp(timestamps, 1)))
        return fail("execute R8 UI/HUD and selection passes", status);

    const auto copyTarget = [&](ImageHandle image, BufferHandle destination)
    {
        BufferImageCopyRegion region;
        region.imageSubresource = {ImageAspect::Color, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        return commands.copyImageToBuffer(
            image, destination, std::array<BufferImageCopyRegion, 1>{{region}});
    };
    if (!(status = copyTarget(color, colorReadback)) ||
        !(status = copyTarget(selection, selectionReadback)) ||
        !(status = copyTarget(pickDepth, pickDepthReadback)) ||
        !(status = commands.endFrame()))
        return fail("finish R8 interaction frame", status);

    const auto read = [&](BufferHandle buffer, std::vector<std::byte>& data)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        Status readStatus = Status::success();
        do
        {
            readStatus = device.readBuffer(buffer, 0, data);
            if (readStatus || readStatus.code() != StatusCode::NotReady) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return readStatus;
    };
    std::vector<std::byte> colorPixels(pixelBytes);
    std::vector<std::byte> selectionPixels(pixelBytes);
    std::vector<std::byte> pickDepthPixels(pixelBytes);
    if (!(status = read(colorReadback, colorPixels)) ||
        !(status = read(selectionReadback, selectionPixels)) ||
        !(status = read(pickDepthReadback, pickDepthPixels)))
        return fail("read R8 snapshot and picking targets", status);
    if (device.backend() == Backend::OpenGL)
    {
        constexpr std::size_t rowBytes = width * 4u;
        const auto flip = [&](std::vector<std::byte>& pixels)
        {
            for (std::uint32_t y = 0; y < height / 2u; ++y)
                std::swap_ranges(pixels.begin() + y * rowBytes,
                    pixels.begin() + (y + 1u) * rowBytes,
                    pixels.begin() + (height - 1u - y) * rowBytes);
        };
        flip(colorPixels);
        flip(selectionPixels);
        flip(pickDepthPixels);
    }

    std::array<std::uint64_t, 2> queryResults{};
    if (!(status = device.getQueryResults(
            timestamps, 0, queryResults, QueryReadMode::Wait)))
        return fail("read R8 profiling interval", status);
    if (queryResults[1] <= queryResults[0])
        return fail("validate R8 profiling interval", Status::failure(
            StatusCode::BackendError, "timestamp interval is not positive"));
    result.gpuTicks = queryResults[1] - queryResults[0];
    result.gpuNanoseconds = result.gpuTicks *
        device.capabilities().timestampPeriodNanoseconds;

    const auto uintPixel = [&](const std::vector<std::byte>& pixels,
                               std::uint32_t x, std::uint32_t y)
    {
        std::uint32_t value = 0;
        std::memcpy(&value, pixels.data() + (y * width + x) * 4u, sizeof(value));
        return value;
    };
    const auto floatPixel = [&](std::uint32_t x, std::uint32_t y)
    {
        float value = 0.f;
        std::memcpy(&value,
            pickDepthPixels.data() + (y * width + x) * 4u, sizeof(value));
        return value;
    };
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            if (uintPixel(selectionPixels, x, y) != 0u)
                ++result.selectedPixelCount;
    if (result.selectedPixelCount != selectedPixelReference)
        return fail("R8 selected-pixel coverage mismatch", Status::failure(
            StatusCode::BackendError, std::to_string(result.selectedPixelCount)));
    if (uintPixel(selectionPixels, 2, 2) != 0u ||
        std::abs(floatPixel(2, 2) - 1.f) > 0.0001f ||
        uintPixel(selectionPixels, 12, 20) != 101u ||
        std::abs(floatPixel(12, 20) - 0.43f) > 0.0001f ||
        uintPixel(selectionPixels, 32, 32) != 900u ||
        std::abs(floatPixel(32, 32) - 0.80f) > 0.0001f)
        return fail("validate R8 selection ID/depth samples", Status::failure(
            StatusCode::BackendError, "selection samples do not match UI/HUD ordering"));

    result.snapshotSha256 = sha256(colorPixels);
    result.selectionSha256 = sha256(selectionPixels);
    result.pickDepthSha256 = sha256(pickDepthPixels);
    if (snapshotReference[0] && result.snapshotSha256 != snapshotReference)
        return fail("R8 snapshot hash mismatch", Status::failure(
            StatusCode::BackendError, result.snapshotSha256));
    if (selectionReference[0] && result.selectionSha256 != selectionReference)
        return fail("R8 selection hash mismatch", Status::failure(
            StatusCode::BackendError, result.selectionSha256));
    if (pickDepthReference[0] && result.pickDepthSha256 != pickDepthReference)
        return fail("R8 pick-depth hash mismatch", Status::failure(
            StatusCode::BackendError, result.pickDepthSha256));

    for (BindingSetHandle bindings : bindingSets) device.destroy(bindings);
    for (BufferHandle uniform : uniformBuffers) device.destroy(uniform);
    for (Status destroyStatus : {
        device.destroy(opaquePipeline), device.destroy(overlayPipeline),
        device.destroy(shader), device.destroy(timestamps), device.destroy(colorView),
        device.destroy(selectionView), device.destroy(pickDepthView),
        device.destroy(color), device.destroy(selection), device.destroy(pickDepth),
        device.destroy(colorReadback), device.destroy(selectionReadback),
        device.destroy(pickDepthReadback), device.destroy(upload)})
        if (!destroyStatus)
            return fail("destroy R8 interaction resource", destroyStatus);
    if (!(status = device.waitIdle()))
        return fail("wait for R8 interaction retirement", status);
    result.passed = true;
    result.message = "R8 UI/HUD, picking, snapshot, and profiling fixture PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIINTERACTIONFIXTURE_H
