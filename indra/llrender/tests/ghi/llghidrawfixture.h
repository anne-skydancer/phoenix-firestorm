/**
 * @file llghidrawfixture.h
 * @brief Backend-independent R3 offscreen indexed draw workload.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIDRAWFIXTURE_H
#define LL_LLGHIDRAWFIXTURE_H

#include "ghi/include/llghi.h"
#include "ghi/core/llghihash.h"
#include "ghi/core/llghipipelinecache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace LL::GHI::Test
{

struct DrawFixtureOptions
{
    Format colorFormat = Format::RGBA8UNorm;
    bool captureColor = false;
    ShaderPackageDesc::TargetProfile targetProfile =
        ShaderPackageDesc::TargetProfile::OpenGL44;
};

struct DrawFixtureResult
{
    bool passed = false;
    std::string message;
    Format depthStencilFormat = Format::Undefined;
    Format colorFormat = Format::Undefined;
    std::vector<std::byte> colorPixels;
    std::string colorSha256;
    std::string pipelineCacheIdentity;
    std::uint32_t shadedPixelCount = 0;
    std::uint64_t shaderCreateMicroseconds = 0;
    std::uint64_t pipelineCreateMicroseconds = 0;
};

inline const char* drawFixtureReferenceHash(Format format)
{
    switch (format)
    {
    case Format::RGBA8UNorm:
        return "f126a216776f58450335bc39732b8b1df604329cee2adc11e0bd9c8ec686937f";
    case Format::RGBA8SRGB:
        return "15f2cdf81d3b51469bb4082df1453ba60fccd40f043c7ff1e11a7ac211a4c53a";
    default:
        return nullptr;
    }
}

inline DrawFixtureResult runDrawFixture(
    Device& device,
    const ShaderPackageDesc& shaderPackage,
    const DrawFixtureOptions& options = {})
{
    auto fail = [&](const char* operation, const Status& status)
    {
        return DrawFixtureResult{false,
            std::string(operation) + ": " + status.message(), Format::Undefined,
            options.colorFormat};
    };
    struct Vertex
    {
        float position[3];
        float texCoord[2];
    };
    constexpr std::array<Vertex, 4> vertices{{
        {{-0.75f, -0.75f, 0.75f}, {0.f, 0.f}},
        {{ 0.75f, -0.75f, 0.75f}, {1.f, 0.f}},
        {{ 0.75f,  0.75f, 0.75f}, {1.f, 1.f}},
        {{-0.75f,  0.75f, 0.75f}, {0.f, 1.f}},
    }};
    constexpr std::array<std::uint16_t, 6> indices{{0, 1, 2, 2, 3, 0}};
    constexpr std::array<float, 16> transform{{
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f,
    }};
    constexpr std::array<std::uint8_t, 16> texels{{
        0xff, 0x20, 0x20, 0xff, 0x20, 0xff, 0x20, 0xff,
        0x20, 0x20, 0xff, 0xff, 0xff, 0xff, 0x20, 0xff,
    }};
    constexpr std::uint64_t vertexOffset = 0;
    constexpr std::uint64_t indexOffset = sizeof(vertices);
    constexpr std::uint64_t uniformOffset = indexOffset + sizeof(indices);
    constexpr std::uint64_t textureOffset = uniformOffset + sizeof(transform);
    constexpr std::uint64_t uploadBytes = textureOffset + sizeof(texels);
    constexpr std::uint32_t targetWidth = 64;
    constexpr std::uint32_t targetHeight = 64;
    constexpr std::uint32_t colorBytesPerPixel = 4;
    constexpr std::uint64_t readbackBytes =
        targetWidth * targetHeight * colorBytesPerPixel;

    Status status = Status::success();
    BufferHandle upload = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status) return fail("create upload buffer", status);
    BufferHandle vertex = device.createBuffer(
        {sizeof(vertices), ResourceUsage::Vertex | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create vertex buffer", status);
    BufferHandle index = device.createBuffer(
        {sizeof(indices), ResourceUsage::Index | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create index buffer", status);
    BufferHandle uniform = device.createBuffer(
        {sizeof(transform), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create uniform buffer", status);
    BufferHandle readback = device.createBuffer(
        {readbackBytes, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
    if (!status) return fail("create color readback buffer", status);

    std::array<std::byte, uploadBytes> uploadData{};
    std::memcpy(uploadData.data() + vertexOffset, vertices.data(), sizeof(vertices));
    std::memcpy(uploadData.data() + indexOffset, indices.data(), sizeof(indices));
    std::memcpy(uploadData.data() + uniformOffset, transform.data(), sizeof(transform));
    std::memcpy(uploadData.data() + textureOffset, texels.data(), sizeof(texels));
    status = device.writeBuffer(upload, 0, uploadData);
    if (!status) return fail("write upload buffer", status);

    ImageHandle texture = device.createImage(
        {{2, 2, 1}, Format::RGBA8UNorm,
         ResourceUsage::Sampled | ResourceUsage::TransferDestination, 1, 1, 1}, status);
    if (!status) return fail("create sampled image", status);
    ImageViewHandle textureView = device.createImageView(
        {texture, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    if (!status) return fail("create sampled image view", status);
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = Filter::Nearest;
    samplerDesc.magFilter = Filter::Nearest;
    samplerDesc.mipFilter = Filter::Nearest;
    samplerDesc.addressU = AddressMode::ClampToEdge;
    samplerDesc.addressV = AddressMode::ClampToEdge;
    SamplerHandle sampler = device.createSampler(samplerDesc, status);
    if (!status) return fail("create sampler", status);

    ImageHandle color = device.createImage(
        {{targetWidth, targetHeight, 1}, options.colorFormat,
         ResourceUsage::ColorAttachment | ResourceUsage::TransferSource, 1, 1, 1}, status);
    if (!status) return fail("create color target", status);
    ImageViewHandle colorView = device.createImageView(
        {color, options.colorFormat, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    if (!status) return fail("create color target view", status);

    const Format depthFormat = device.capabilities().preferredDepthStencilFormat;
    if (depthFormat == Format::Undefined)
        return {false, "device did not select a depth/stencil format", depthFormat};
    ImageHandle depth = device.createImage(
        {{targetWidth, targetHeight, 1}, depthFormat,
         ResourceUsage::DepthStencilAttachment, 1, 1, 1}, status);
    if (!status) return fail("create depth/stencil target", status);
    ImageViewHandle depthView = device.createImageView(
        {depth, depthFormat, {ImageAspect::DepthStencil, 0, 1, 0, 1}}, status);
    if (!status) return fail("create depth/stencil target view", status);

    const auto shaderStart = std::chrono::steady_clock::now();
    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    const auto shaderEnd = std::chrono::steady_clock::now();
    if (!status) return fail("create shader package", status);
    BindingSetDesc frameBindings;
    frameBindings.shader = shader;
    frameBindings.group = 0;
    frameBindings.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         uniform, 0, sizeof(transform), {}, {}});
    BindingSetHandle frameSet = device.createBindingSet(frameBindings, status);
    if (!status) return fail("create frame binding set", status);
    BindingSetDesc materialBindings;
    materialBindings.shader = shader;
    materialBindings.group = 2;
    materialBindings.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
         {}, 0, 0, textureView, sampler});
    BindingSetHandle materialSet = device.createBindingSet(materialBindings, status);
    if (!status) return fail("create material binding set", status);

    PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader;
    pipelineDesc.cullMode = CullMode::Back;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompare = CompareOp::GreaterEqual;
    pipelineDesc.colorFormats = {options.colorFormat};
    pipelineDesc.depthStencilFormat = depthFormat;
    pipelineDesc.blendStates = {BlendState{}};
    pipelineDesc.vertexBuffers = {{0, sizeof(Vertex), VertexInputRate::PerVertex}};
    pipelineDesc.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0},
        {1, 0, VertexFormat::Float32x2, 12},
    };
    const std::string cacheIdentity = pipelineCacheIdentity(
        shaderPackage, pipelineDesc, options.targetProfile, device.backend(),
        device.pipelineCacheDomain());
    const auto pipelineStart = std::chrono::steady_clock::now();
    PipelineHandle pipeline = device.createPipeline(pipelineDesc, status);
    const auto pipelineEnd = std::chrono::steady_clock::now();
    if (!status) return fail("create graphics pipeline", status);

    CommandContext& commands = device.commandContext();
    status = commands.beginFrame();
    if (!status) return fail("begin draw frame", status);
    const std::array<BufferCopyRegion, 1> vertexCopy{{{vertexOffset, 0, sizeof(vertices)}}};
    const std::array<BufferCopyRegion, 1> indexCopy{{{indexOffset, 0, sizeof(indices)}}};
    const std::array<BufferCopyRegion, 1> uniformCopy{{{uniformOffset, 0, sizeof(transform)}}};
    if (!(status = commands.copyBuffer(upload, vertex, vertexCopy))) return fail("upload vertices", status);
    if (!(status = commands.copyBuffer(upload, index, indexCopy))) return fail("upload indices", status);
    if (!(status = commands.copyBuffer(upload, uniform, uniformCopy))) return fail("upload frame data", status);
    BufferImageCopyRegion textureCopy;
    textureCopy.bufferOffset = textureOffset;
    textureCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
    textureCopy.imageExtent = {2, 2, 1};
    const std::array<BufferImageCopyRegion, 1> textureCopies{{textureCopy}};
    if (!(status = commands.copyBufferToImage(upload, texture, textureCopies)))
        return fail("upload texture", status);

    RenderingInfo rendering;
    rendering.semanticId = 0x52335f44524157ull; // "R3_DRAW"
    rendering.width = targetWidth;
    rendering.height = targetHeight;
    rendering.colors.push_back(
        {colorView, options.colorFormat, LoadOp::Clear, StoreOp::Store,
         {{0.02f, 0.03f, 0.05f, 1.f}, 0.f, 0}});
    rendering.depthStencil = AttachmentDesc{
        depthView, depthFormat, LoadOp::Clear, StoreOp::Store,
        {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};
    if (!(status = commands.beginRendering(rendering))) return fail("begin rendering", status);
    if (!(status = commands.bindPipeline(pipeline))) return fail("bind pipeline", status);
    if (!(status = commands.bindBindingSet(0, frameSet))) return fail("bind frame resources", status);
    if (!(status = commands.bindBindingSet(2, materialSet))) return fail("bind material resources", status);
    if (!(status = commands.setViewport(
              {0.f, 0.f, static_cast<float>(targetWidth),
               static_cast<float>(targetHeight), 0.f, 1.f})))
        return fail("set viewport", status);
    if (!(status = commands.setScissor({0, 0, targetWidth, targetHeight})))
        return fail("set scissor", status);
    if (!(status = commands.bindVertexBuffer(0, vertex, 0))) return fail("bind vertex buffer", status);
    if (!(status = commands.bindIndexBuffer(index, 0, IndexType::UInt16)))
        return fail("bind index buffer", status);
    if (!(status = commands.drawIndexed({6, 1, 0, 0, 0}))) return fail("draw indexed", status);
    if (!(status = commands.endRendering())) return fail("end rendering", status);
    BufferImageCopyRegion colorCopy;
    colorCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
    colorCopy.imageExtent = {targetWidth, targetHeight, 1};
    const std::array<BufferImageCopyRegion, 1> colorCopies{{colorCopy}};
    if (!(status = commands.copyImageToBuffer(color, readback, colorCopies)))
        return fail("copy color target to readback", status);
    if (!(status = commands.endFrame())) return fail("end draw frame", status);

    std::vector<std::byte> pixels;
    std::string pixelHash;
    std::uint32_t shadedPixelCount = 0;
    if (options.captureColor)
    {
        pixels.resize(readbackBytes);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do
        {
            status = device.readBuffer(readback, 0, pixels);
            if (status || status.code() != StatusCode::NotReady) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!status) return fail("read color target", status);

        // GHI diagnostic images use top-left row order. glGetTexImage returns
        // bottom-left row order, so normalize the OpenGL peer before hashing.
        if (device.backend() == Backend::OpenGL)
        {
            constexpr std::size_t rowBytes = targetWidth * colorBytesPerPixel;
            for (std::uint32_t y = 0; y < targetHeight / 2; ++y)
            {
                auto top = pixels.begin() + static_cast<std::ptrdiff_t>(y * rowBytes);
                auto bottom = pixels.begin() + static_cast<std::ptrdiff_t>(
                    (targetHeight - 1 - y) * rowBytes);
                std::swap_ranges(top, top + rowBytes, bottom);
            }
        }

        const std::array<std::byte, 4> clear{
            pixels[0], pixels[1], pixels[2], pixels[3]};
        for (std::size_t i = 0; i < pixels.size(); i += colorBytesPerPixel)
        {
            if (!std::equal(clear.begin(), clear.end(), pixels.begin() + i))
                ++shadedPixelCount;
        }
        if (shadedPixelCount != 48u * 48u)
        {
            return {false,
                "diagnostic coverage mismatch: expected 2304 shaded pixels, got " +
                    std::to_string(shadedPixelCount),
                depthFormat, options.colorFormat, std::move(pixels), {},
                cacheIdentity, shadedPixelCount};
        }
        pixelHash = sha256(pixels);
        const char* referenceHash = drawFixtureReferenceHash(options.colorFormat);
        if (!referenceHash || pixelHash != referenceHash)
        {
            return {false,
                "diagnostic image SHA-256 mismatch: " + pixelHash,
                depthFormat, options.colorFormat, std::move(pixels),
                std::move(pixelHash), cacheIdentity, shadedPixelCount};
        }
    }

    for (Status destroyStatus : {
        device.destroy(pipeline), device.destroy(frameSet), device.destroy(materialSet),
        device.destroy(shader), device.destroy(textureView), device.destroy(sampler),
        device.destroy(colorView), device.destroy(depthView), device.destroy(texture),
        device.destroy(color), device.destroy(depth), device.destroy(upload),
        device.destroy(readback),
        device.destroy(vertex), device.destroy(index), device.destroy(uniform)})
    {
        if (!destroyStatus) return fail("destroy draw fixture resource", destroyStatus);
    }
    if (!(status = device.waitIdle())) return fail("wait for draw fixture retirement", status);
    const auto microseconds = [](auto start, auto end)
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    };
    return {true, "R3 indexed draw fixture PASS", depthFormat,
            options.colorFormat, std::move(pixels), std::move(pixelHash),
            cacheIdentity, shadedPixelCount,
            microseconds(shaderStart, shaderEnd),
            microseconds(pipelineStart, pipelineEnd)};
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIDRAWFIXTURE_H
