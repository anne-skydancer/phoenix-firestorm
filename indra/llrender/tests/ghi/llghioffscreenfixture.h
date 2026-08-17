/**
 * @file llghioffscreenfixture.h
 * @brief Backend-independent R7 probe and dynamic-image fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIOFFSCREENFIXTURE_H
#define LL_LLGHIOFFSCREENFIXTURE_H

#include "ghi/core/llghihash.h"
#include "ghi/include/llghi.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace LL::GHI::Test
{

struct OffscreenFixtureResult
{
    bool passed = false;
    std::string message;
    std::string colorSha256;
    std::uint32_t shadedPixelCount = 0;
};

inline OffscreenFixtureResult runOffscreenFixture(
    Device& device, const ShaderPackageDesc& shaderPackage)
{
    constexpr std::uint32_t probeSize = 16;
    constexpr std::uint32_t textureSize = 8;
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr std::uint16_t probeMips = 5;
    constexpr std::uint64_t textureBytes = textureSize * textureSize * 4u;
    constexpr std::uint64_t mediaPatchRowPixels = 6;
    constexpr std::uint64_t mediaPatchBytes = mediaPatchRowPixels * textureSize * 4u;
    constexpr std::uint64_t dynamicOffset = 0;
    constexpr std::uint64_t mediaBaseOffset = dynamicOffset + textureBytes;
    constexpr std::uint64_t mediaPatchOffset = mediaBaseOffset + textureBytes;
    constexpr std::uint64_t uploadBytes = mediaPatchOffset + mediaPatchBytes;
    constexpr std::uint64_t colorBytes = width * height * 4u;
    constexpr const char* referenceHash =
        "a7672b031451f5ccb87aaa7ad8f5d1cfee57fea0ff9f75302a60720551377bf5";
    constexpr std::uint32_t referenceCoverage = width * height;

    OffscreenFixtureResult result;
    auto fail = [&](const char* operation, const Status& status)
    {
        result.message = std::string(operation) + ": " + status.message();
        return result;
    };
    Status status = Status::success();

    std::vector<std::byte> uploadData(uploadBytes, std::byte{0});
    for (std::uint32_t y = 0; y < textureSize; ++y)
    {
        for (std::uint32_t x = 0; x < textureSize; ++x)
        {
            const std::size_t dynamic = dynamicOffset + (y * textureSize + x) * 4u;
            const bool bright = (x & 1u) != 0;
            uploadData[dynamic + 0] = bright ? std::byte{0xff} : std::byte{0x20};
            uploadData[dynamic + 1] = bright ? std::byte{0x40} : std::byte{0xff};
            uploadData[dynamic + 2] = std::byte{0x20};
            uploadData[dynamic + 3] = std::byte{0xff};

            const std::size_t media = mediaBaseOffset + (y * textureSize + x) * 4u;
            uploadData[media + 0] = std::byte{0x10};
            uploadData[media + 1] = std::byte{0x30};
            uploadData[media + 2] = std::byte{0xd0};
            uploadData[media + 3] = std::byte{0xff};
        }
        for (std::uint32_t x = 0; x < mediaPatchRowPixels; ++x)
        {
            const std::size_t patch = mediaPatchOffset +
                (y * mediaPatchRowPixels + x) * 4u;
            uploadData[patch + 0] = x < 4 ? std::byte{0xf0} : std::byte{0x5a};
            uploadData[patch + 1] = x < 4 ? std::byte{0x80} : std::byte{0xa5};
            uploadData[patch + 2] = x < 4 ? std::byte{0x10} : std::byte{0x3c};
            uploadData[patch + 3] = std::byte{0xff};
        }
    }

    BufferHandle upload = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status || !(status = device.writeBuffer(upload, 0, uploadData)))
        return fail("prepare R7 dynamic-image upload", status);

    ImageDesc probeDesc;
    probeDesc.extent = {probeSize, probeSize, 1};
    probeDesc.format = Format::RGBA8UNorm;
    probeDesc.usage = ResourceUsage::ColorAttachment | ResourceUsage::Sampled |
        ResourceUsage::TransferSource | ResourceUsage::TransferDestination;
    probeDesc.mipLevels = probeMips;
    probeDesc.arrayLayers = 6;
    probeDesc.cubeCompatible = true;
    ImageHandle probe = device.createImage(probeDesc, status);
    std::array<ImageViewHandle, 6> probeFaces{};
    for (std::uint16_t face = 0; face < probeFaces.size(); ++face)
        probeFaces[face] = device.createImageView(
            {probe, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, face, 1},
             ImageViewType::Texture2D}, status);
    ImageViewHandle probeView = device.createImageView(
        {probe, Format::RGBA8UNorm, {ImageAspect::Color, 0, probeMips, 0, 6},
         ImageViewType::TextureCubeArray}, status);

    auto createTexture = [&](ImageViewHandle& view)
    {
        ImageHandle image = device.createImage(
            {{textureSize, textureSize, 1}, Format::RGBA8UNorm,
             ResourceUsage::Sampled | ResourceUsage::TransferDestination, 1, 1, 1}, status);
        view = device.createImageView(
            {image, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1},
             ImageViewType::Texture2D}, status);
        return image;
    };
    ImageViewHandle dynamicView;
    ImageViewHandle mediaView;
    ImageHandle dynamicImage = createTexture(dynamicView);
    ImageHandle mediaImage = createTexture(mediaView);
    ImageHandle color = device.createImage(
        {{width, height, 1}, Format::RGBA8UNorm,
         ResourceUsage::ColorAttachment | ResourceUsage::TransferSource, 1, 1, 1}, status);
    ImageViewHandle colorView = device.createImageView(
        {color, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1},
         ImageViewType::Texture2D}, status);
    BufferHandle readback = device.createBuffer(
        {colorBytes, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = samplerDesc.magFilter = samplerDesc.mipFilter = Filter::Nearest;
    samplerDesc.addressU = samplerDesc.addressV = samplerDesc.addressW = AddressMode::ClampToEdge;
    SamplerHandle sampler = device.createSampler(samplerDesc, status);
    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    if (!status) return fail("create R7 offscreen resources", status);

    BindingSetDesc bindingDesc;
    bindingDesc.shader = shader;
    bindingDesc.group = 0;
    bindingDesc.resources = {
        {0, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
         {}, 0, 0, probeView, sampler},
        {1, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
         {}, 0, 0, dynamicView, sampler},
        {2, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
         {}, 0, 0, mediaView, sampler}};
    BindingSetHandle bindings = device.createBindingSet(bindingDesc, status);
    PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader;
    pipelineDesc.cullMode = CullMode::None;
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    pipelineDesc.colorFormats = {Format::RGBA8UNorm};
    pipelineDesc.blendStates = {BlendState{}};
    PipelineHandle pipeline = device.createPipeline(pipelineDesc, status);
    if (!status) return fail("create R7 offscreen resolve state", status);

    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame()))
        return fail("begin R7 offscreen frame", status);
    BufferImageCopyRegion dynamicCopy;
    dynamicCopy.bufferOffset = dynamicOffset;
    dynamicCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
    dynamicCopy.imageExtent = {textureSize, textureSize, 1};
    BufferImageCopyRegion mediaBaseCopy = dynamicCopy;
    mediaBaseCopy.bufferOffset = mediaBaseOffset;
    if (!(status = commands.copyBufferToImage(upload, dynamicImage,
            std::array<BufferImageCopyRegion, 1>{{dynamicCopy}})) ||
        !(status = commands.copyBufferToImage(upload, mediaImage,
            std::array<BufferImageCopyRegion, 1>{{mediaBaseCopy}})))
        return fail("upload R7 dynamic images", status);
    BufferImageCopyRegion mediaPatchCopy;
    mediaPatchCopy.bufferOffset = mediaPatchOffset;
    mediaPatchCopy.bufferRowLength = mediaPatchRowPixels;
    mediaPatchCopy.bufferImageHeight = textureSize;
    mediaPatchCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
    mediaPatchCopy.imageOffset = {2, 0, 0};
    mediaPatchCopy.imageExtent = {4, textureSize, 1};
    if (!(status = commands.copyBufferToImage(upload, mediaImage,
            std::array<BufferImageCopyRegion, 1>{{mediaPatchCopy}})))
        return fail("apply R7 row-pitched media update", status);

    constexpr std::array<std::array<float, 4>, 6> faceColors{{
        {{1.f, 0.f, 0.f, 1.f}}, {{0.f, 1.f, 0.f, 1.f}},
        {{0.f, 0.f, 1.f, 1.f}}, {{1.f, 1.f, 0.f, 1.f}},
        {{1.f, 0.f, 1.f, 1.f}}, {{0.f, 1.f, 1.f, 1.f}}}};
    for (std::uint16_t face = 0; face < probeFaces.size(); ++face)
    {
        RenderingInfo rendering;
        OffscreenPassDesc pass;
        pass.view = RenderViewClass::ReflectionProbe;
        pass.recursionDepth = 1;
        pass.face = static_cast<CubeFace>(face);
        pass.probePhase = ProbePhase::DirectLighting;
        pass.arrayLayer = face;
        pass.updateEpoch = 1;
        rendering.semanticId = offscreenSemanticId(pass);
        rendering.width = probeSize;
        rendering.height = probeSize;
        ClearValue clear;
        clear.color = faceColors[face];
        rendering.colors.push_back(
            {probeFaces[face], Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store, clear});
        if (!(status = commands.beginRendering(rendering)) ||
            !(status = commands.endRendering()))
            return fail("render R7 probe face", status);
    }
    if (!(status = commands.resourceBarrier(
            ResourceBarrier::ColorAttachmentWriteToSampledRead)) ||
        !(status = commands.generateMipmaps(
            probe, {ImageAspect::Color, 0, probeMips, 0, 6})))
        return fail("finish R7 probe capture", status);

    RenderingInfo resolve;
    resolve.semanticId = 0x52375f4f46465343ull;
    resolve.width = width;
    resolve.height = height;
    resolve.colors.push_back({colorView, Format::RGBA8UNorm, LoadOp::Clear,
        StoreOp::Store, {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
    if (!(status = commands.beginRendering(resolve)) ||
        !(status = commands.setViewport(
            {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f})) ||
        !(status = commands.setScissor({0, 0, width, height})) ||
        !(status = commands.bindPipeline(pipeline)) ||
        !(status = commands.bindBindingSet(0, bindings)) ||
        !(status = commands.draw({3, 1, 0, 0})) ||
        !(status = commands.endRendering()))
        return fail("resolve R7 offscreen sources", status);
    BufferImageCopyRegion colorCopy;
    colorCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
    colorCopy.imageExtent = {width, height, 1};
    if (!(status = commands.copyImageToBuffer(color, readback,
            std::array<BufferImageCopyRegion, 1>{{colorCopy}})) ||
        !(status = commands.endFrame()))
        return fail("finish R7 offscreen frame", status);

    std::vector<std::byte> pixels(colorBytes);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do
    {
        status = device.readBuffer(readback, 0, pixels);
        if (status || status.code() != StatusCode::NotReady) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!status) return fail("read R7 offscreen pixels", status);
    if (device.backend() == Backend::OpenGL)
    {
        constexpr std::size_t rowBytes = width * 4u;
        for (std::uint32_t y = 0; y < height / 2u; ++y)
            std::swap_ranges(pixels.begin() + y * rowBytes,
                pixels.begin() + (y + 1u) * rowBytes,
                pixels.begin() + (height - 1u - y) * rowBytes);
    }
    result.colorSha256 = sha256(pixels);
    for (std::size_t pixel = 0; pixel < width * height; ++pixel)
        if (pixels[pixel * 4u + 3u] != std::byte{0}) ++result.shadedPixelCount;
    if (result.shadedPixelCount != referenceCoverage)
        return fail("R7 offscreen coverage mismatch", Status::failure(
            StatusCode::BackendError, std::to_string(result.shadedPixelCount)));
    if (referenceHash[0] && result.colorSha256 != referenceHash)
        return fail("R7 offscreen hash mismatch", Status::failure(
            StatusCode::BackendError, result.colorSha256));

    device.destroy(bindings);
    device.destroy(pipeline);
    device.destroy(shader);
    for (ImageViewHandle face : probeFaces) device.destroy(face);
    for (Status destroyStatus : {
        device.destroy(probeView), device.destroy(dynamicView), device.destroy(mediaView),
        device.destroy(colorView), device.destroy(sampler), device.destroy(probe),
        device.destroy(dynamicImage), device.destroy(mediaImage), device.destroy(color),
        device.destroy(readback), device.destroy(upload)})
        if (!destroyStatus) return fail("destroy R7 offscreen resource", destroyStatus);
    if (!(status = device.waitIdle())) return fail("wait for R7 offscreen retirement", status);
    result.passed = true;
    result.message = "R7 offscreen probe/dynamic-image fixture PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIOFFSCREENFIXTURE_H
