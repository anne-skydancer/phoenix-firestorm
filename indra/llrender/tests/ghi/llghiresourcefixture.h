/**
 * @file llghiresourcefixture.h
 * @brief Backend-independent R2 resource acceptance workload.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIRESOURCEFIXTURE_H
#define LL_LLGHIRESOURCEFIXTURE_H

#include "ghi/include/llghi.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>

namespace LL::GHI::Test
{

struct ResourceFixtureResult
{
    bool passed = false;
    std::string message;
};

inline ResourceFixtureResult runResourceFixture(Device& device)
{
    auto fail = [](const char* operation, const Status& status)
    {
        return ResourceFixtureResult{false,
            std::string(operation) + ": " + status.message()};
    };
    Status status = Status::success();
    const ResourceUsage transferSource = ResourceUsage::TransferSource;
    const ResourceUsage transferDestination = ResourceUsage::TransferDestination;
    BufferHandle upload = device.createBuffer(
        {64, transferSource, MemoryClass::Upload}, status);
    if (!status) return fail("create upload buffer", status);
    BufferHandle local = device.createBuffer(
        {64, transferSource | transferDestination, MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create device-local buffer", status);
    BufferHandle readback = device.createBuffer(
        {68, transferDestination, MemoryClass::Readback}, status);
    if (!status) return fail("create readback buffer", status);

    std::array<std::byte, 64> pixels{};
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        pixels[i] = std::byte{0x24};
        pixels[i + 1] = std::byte{0x68};
        pixels[i + 2] = std::byte{0xac};
        pixels[i + 3] = std::byte{0xff};
    }
    status = device.writeBuffer(upload, 0, pixels);
    if (!status) return fail("write upload buffer", status);

    ImageDesc imageDesc;
    imageDesc.extent = {4, 4, 1};
    imageDesc.format = Format::RGBA8UNorm;
    imageDesc.usage = ResourceUsage::Sampled | transferSource | transferDestination;
    imageDesc.mipLevels = 3;
    ImageHandle image = device.createImage(imageDesc, status);
    if (!status) return fail("create image", status);

    ImageViewDesc viewDesc;
    viewDesc.image = image;
    viewDesc.format = imageDesc.format;
    viewDesc.subresources = {ImageAspect::Color, 0, 3, 0, 1};
    ImageViewHandle view = device.createImageView(viewDesc, status);
    if (!status) return fail("create image view", status);
    SamplerHandle sampler = device.createSampler({}, status);
    if (!status) return fail("create sampler", status);
    QueryPoolHandle queries = device.createQueryPool({QueryType::Timestamp, 2}, status);
    if (!status) return fail("create timestamp queries", status);

    const std::array<std::pair<Format, ImageAspect>, 12> representativeFormats{{
        {Format::R8UNorm, ImageAspect::Color},
        {Format::RGBA8UNorm, ImageAspect::Color},
        {Format::BGRA8SRGB, ImageAspect::Color},
        {Format::RGB10A2UNorm, ImageAspect::Color},
        {Format::RGBA16UNorm, ImageAspect::Color},
        {Format::RGB16Float, ImageAspect::Color},
        {Format::RGBA16Float, ImageAspect::Color},
        {Format::R32UInt, ImageAspect::Color},
        {Format::Depth16UNorm, ImageAspect::Depth},
        {Format::Depth24Stencil8, ImageAspect::DepthStencil},
        {Format::Depth32Float, ImageAspect::Depth},
        {Format::Depth32FloatStencil8, ImageAspect::DepthStencil},
    }};
    for (const auto& [format, aspect] : representativeFormats)
    {
        ImageDesc representativeDesc;
        representativeDesc.extent = {8, 8, 1};
        representativeDesc.format = format;
        representativeDesc.usage = aspect == ImageAspect::Color
            ? ResourceUsage::Sampled
            : ResourceUsage::DepthStencilAttachment;
        ImageHandle representative = device.createImage(representativeDesc, status);
        // D24S8 and three-component float attachments are not universally
        // available in Vulkan. A peer must reject them explicitly so policy
        // can choose D32FS8 or RGBA16F without hidden resource substitution.
        if (!status &&
            (format == Format::Depth24Stencil8 || format == Format::RGB16Float) &&
            status.code() == StatusCode::Unsupported)
        {
            continue;
        }
        if (!status) return fail("create representative image", status);
        ImageViewDesc representativeViewDesc;
        representativeViewDesc.image = representative;
        representativeViewDesc.format = format;
        representativeViewDesc.subresources = {aspect, 0, 1, 0, 1};
        ImageViewHandle representativeView =
            device.createImageView(representativeViewDesc, status);
        if (!status) return fail("create representative image view", status);
        status = device.destroy(representativeView);
        if (!status) return fail("destroy representative image view", status);
        status = device.destroy(representative);
        if (!status) return fail("destroy representative image", status);
    }

    CommandContext& commands = device.commandContext();
    status = commands.beginFrame();
    if (!status) return fail("begin frame", status);
    status = commands.resetQueryPool(queries, 0, 2);
    if (!status) return fail("reset queries", status);
    status = commands.writeTimestamp(queries, 0);
    if (!status) return fail("write first timestamp", status);

    const std::array<BufferCopyRegion, 1> uploadCopy{{{0, 0, 64}}};
    status = commands.copyBuffer(upload, local, uploadCopy);
    if (!status) return fail("upload buffer copy", status);
    status = commands.copyBuffer(local, readback, uploadCopy);
    if (!status) return fail("readback buffer copy", status);

    BufferImageCopyRegion baseUpload;
    baseUpload.imageSubresource = {ImageAspect::Color, 0, 0, 1};
    baseUpload.imageExtent = {4, 4, 1};
    const std::array<BufferImageCopyRegion, 1> baseUploadRegions{{baseUpload}};
    status = commands.copyBufferToImage(upload, image, baseUploadRegions);
    if (!status) return fail("image upload", status);
    status = commands.generateMipmaps(image, {ImageAspect::Color, 0, 3, 0, 1});
    if (!status) return fail("mipmap generation", status);

    BufferImageCopyRegion finalReadback;
    finalReadback.bufferOffset = 64;
    finalReadback.imageSubresource = {ImageAspect::Color, 2, 0, 1};
    finalReadback.imageExtent = {1, 1, 1};
    const std::array<BufferImageCopyRegion, 1> finalReadbackRegions{{finalReadback}};
    status = commands.copyImageToBuffer(image, readback, finalReadbackRegions);
    if (!status) return fail("final mip readback", status);
    status = commands.writeTimestamp(queries, 1);
    if (!status) return fail("write second timestamp", status);
    status = commands.endFrame();
    if (!status) return fail("end frame", status);

    std::array<std::byte, 68> result{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        status = device.readBuffer(readback, 0, result);
        if (status || status.code() != StatusCode::NotReady) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!status) return fail("read completed buffer", status);
    if (!std::equal(pixels.begin(), pixels.end(), result.begin()))
        return {false, "buffer roundtrip was not byte-exact"};
    if (!std::equal(pixels.begin(), pixels.begin() + 4, result.begin() + 64))
        return {false, "constant-color final mip was not byte-exact"};

    std::array<std::uint64_t, 2> timestampValues{};
    const auto queryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        status = device.getQueryResults(queries, 0, timestampValues,
                                        QueryReadMode::AvailableOnly);
        if (status || status.code() != StatusCode::NotReady) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < queryDeadline);
    if (!status) return fail("read timestamp queries", status);
    if (timestampValues[1] < timestampValues[0])
        return {false, "timestamp results are not monotonic"};

    BufferHandle transientUpload = device.createBuffer(
        {16, transferSource, MemoryClass::Upload}, status);
    if (!status) return fail("create transient upload buffer", status);
    BufferHandle transientLocal = device.createBuffer(
        {16, transferDestination, MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create transient local buffer", status);
    const std::array<std::byte, 16> transientData{
        std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
        std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef},
        std::byte{0x10}, std::byte{0x32}, std::byte{0x54}, std::byte{0x76},
        std::byte{0x98}, std::byte{0xba}, std::byte{0xdc}, std::byte{0xfe}};
    status = device.writeBuffer(transientUpload, 0, transientData);
    if (!status) return fail("write transient upload buffer", status);
    status = commands.beginFrame();
    if (!status) return fail("begin retirement frame", status);
    const std::array<BufferCopyRegion, 1> transientCopy{{{0, 0, 16}}};
    status = commands.copyBuffer(transientUpload, transientLocal, transientCopy);
    if (!status) return fail("record transient copy", status);
    status = commands.endFrame();
    if (!status) return fail("submit retirement frame", status);
    status = device.destroy(transientUpload);
    if (!status) return fail("defer transient upload destruction", status);
    status = device.destroy(transientLocal);
    if (!status) return fail("defer transient local destruction", status);

    for (Status destroyStatus : {
        device.destroy(view), device.destroy(sampler), device.destroy(queries),
        device.destroy(image), device.destroy(upload), device.destroy(local),
        device.destroy(readback)})
    {
        if (!destroyStatus) return fail("destroy fixture resource", destroyStatus);
    }
    status = device.waitIdle();
    if (!status) return fail("wait for resource retirement", status);
    return {true, "R2 resource fixture PASS"};
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIRESOURCEFIXTURE_H
