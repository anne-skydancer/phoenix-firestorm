/**
 * @file llghiworldfixture.h
 * @brief R5c-e terrain, lighting/shadow, and environment peer fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIWORLDFIXTURE_H
#define LL_LLGHIWORLDFIXTURE_H

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

struct WorldFixtureResult
{
    bool passed = false;
    std::string message;
    Format depthStencilFormat = Format::Undefined;
    std::array<std::string, 3> colorSha256;
    std::array<std::uint32_t, 3> shadedPixelCount{};
};

inline WorldFixtureResult runWorldFixture(
    Device& device, const ShaderPackageDesc& shaderPackage)
{
    auto fail = [](const char* operation, const Status& status)
    {
        return WorldFixtureResult{
            false, std::string(operation) + ": " + status.message()};
    };

    struct Vertex
    {
        float position[3];
        float normal[3];
        float texCoord[2];
    };
    static_assert(sizeof(Vertex) == 32);
    struct WorldData
    {
        float viewProjection[16];
        float terrainTransform[16];
        float vectors[56];
    };
    static_assert(sizeof(WorldData) == 352);

    constexpr std::array<Vertex, 4> vertices{{
        {{-.75f, -.75f, .75f}, {0.f, 0.f, 1.f}, {-.25f, -.25f}},
        {{ .75f, -.75f, .75f}, {0.f, 0.f, 1.f}, {1.25f, -.25f}},
        {{ .75f,  .75f, .75f}, {0.f, 0.f, 1.f}, {1.25f, 1.25f}},
        {{-.75f,  .75f, .75f}, {0.f, 0.f, 1.f}, {-.25f, 1.25f}},
    }};
    constexpr std::array<std::uint16_t, 6> indices{{0, 1, 2, 2, 3, 0}};
    constexpr WorldData world{{
        1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}, {
        1.f, 1.f, 0.f, 0.f, 2.f, 2.f, .125f, .25f,
        1.f, 2.f, .25f, .125f, 2.f, 1.f, -.125f, .25f}, {
        // terrain: detail scale, height blend, PBR mix, normal scale
        2.f, .75f, .625f, .5f,
        // sun direction/intensity and color
        0.f, -.6f, -.8f, .75f, .9f, .8f, .625f, 1.f,
        // moon direction/intensity and color
        0.f, .8f, -.6f, .25f, .25f, .375f, .625f, 1.f,
        // local position/radius and color/falloff mode
        .25f, .25f, 1.f, 2.f, .5f, .25f, .125f, .75f,
        // projector direction/cutoff and color/shadow bias
        0.f, 0.f, -1.f, .25f, .25f, .5f, .75f, .5f,
        // haze, water line, Fresnel base, underwater blend
        .25f, .5f, .125f, 0.f,
        // sky zenith and horizon
        .125f, .25f, .625f, 1.f, .625f, .5f, .375f, 1.f,
        // water color/time and two wave directions
        .0625f, .25f, .375f, .25f, .25f, .125f, -.125f, .25f,
    }};
    constexpr std::array<std::array<std::uint8_t, 16>, 6> texels{{
        {{192, 32, 16, 15, 32, 192, 16, 15,
          32, 16, 192, 15, 64, 64, 64, 63}},
        {{64, 96, 32, 32, 96, 128, 48, 96,
          48, 80, 24, 160, 128, 160, 64, 224}},
        {{160, 96, 48, 224, 128, 64, 32, 160,
          192, 128, 64, 96, 96, 48, 24, 32}},
        {{48, 96, 160, 96, 64, 128, 192, 32,
          32, 64, 128, 224, 96, 160, 224, 160}},
        {{128, 128, 255, 160, 160, 112, 248, 224,
          112, 160, 248, 32, 128, 128, 224, 96}},
        {{32, 32, 32, 255, 224, 224, 224, 255,
          160, 160, 160, 255, 96, 96, 96, 255}},
    }};
    constexpr std::array<Format, 6> textureFormats{{
        Format::RGBA8UNorm, Format::RGBA8SRGB, Format::RGBA8SRGB,
        Format::RGBA8SRGB, Format::RGBA8UNorm, Format::RGBA8UNorm}};
    constexpr std::array<const char*, 3> referenceHashes{{
        "a5cf6a0b67b9227adaa765a0560512cfe3c559a7bcedd8461915c873c04b2cd2",
        "8b6f8bb2356985056adf8d073d48b1cc5ab4d5550518ac9175ba17c1a304f751",
        "a7a89831e9fa70c1cdb73cf90e28ee896c956ebca8fb27d65d2697f4da1afa9b"}};
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr std::uint64_t vertexOffset = 0;
    constexpr std::uint64_t indexOffset = vertexOffset + sizeof(vertices);
    constexpr std::uint64_t worldOffset = indexOffset + sizeof(indices);
    constexpr std::uint64_t textureOffset = worldOffset + sizeof(world);
    constexpr std::uint64_t uploadBytes = textureOffset + sizeof(texels);

    Status status = Status::success();
    BufferHandle upload = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status) return fail("create R5 world upload buffer", status);
    BufferHandle vertex = device.createBuffer(
        {sizeof(vertices), ResourceUsage::Vertex | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R5 world vertex buffer", status);
    BufferHandle index = device.createBuffer(
        {sizeof(indices), ResourceUsage::Index | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R5 world index buffer", status);
    BufferHandle uniform = device.createBuffer(
        {sizeof(world), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R5 world uniform buffer", status);

    std::vector<std::byte> uploadData(uploadBytes);
    std::memcpy(uploadData.data() + vertexOffset, vertices.data(), sizeof(vertices));
    std::memcpy(uploadData.data() + indexOffset, indices.data(), sizeof(indices));
    std::memcpy(uploadData.data() + worldOffset, &world, sizeof(world));
    std::memcpy(uploadData.data() + textureOffset, texels.data(), sizeof(texels));
    if (!(status = device.writeBuffer(upload, 0, uploadData)))
        return fail("write R5 world upload buffer", status);

    std::array<ImageHandle, 6> textures;
    std::array<ImageViewHandle, 6> textureViews;
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        textures[i] = device.createImage(
            {{2, 2, 1}, textureFormats[i],
             ResourceUsage::Sampled | ResourceUsage::TransferDestination, 1, 1, 1}, status);
        if (!status) return fail("create R5 world sampled image", status);
        textureViews[i] = device.createImageView(
            {textures[i], textureFormats[i], {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (!status) return fail("create R5 world sampled view", status);
    }
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = samplerDesc.magFilter = samplerDesc.mipFilter = Filter::Nearest;
    samplerDesc.addressU = samplerDesc.addressV = AddressMode::Repeat;
    SamplerHandle sampler = device.createSampler(samplerDesc, status);
    if (!status) return fail("create R5 world sampler", status);

    constexpr std::array<Format, 3> colorFormats{{
        Format::RGBA8UNorm, Format::RGBA8UNorm, Format::RGBA8UNorm}};
    std::array<ImageHandle, 3> colors;
    std::array<ImageViewHandle, 3> colorViews;
    std::array<BufferHandle, 3> readbacks;
    for (std::size_t i = 0; i < colors.size(); ++i)
    {
        colors[i] = device.createImage(
            {{width, height, 1}, colorFormats[i],
             ResourceUsage::ColorAttachment | ResourceUsage::TransferSource, 1, 1, 1}, status);
        if (!status) return fail("create R5 world color target", status);
        colorViews[i] = device.createImageView(
            {colors[i], colorFormats[i], {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (!status) return fail("create R5 world color view", status);
        readbacks[i] = device.createBuffer(
            {static_cast<std::uint64_t>(width) * height * 4,
             ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        if (!status) return fail("create R5 world readback", status);
    }
    const Format depthFormat = device.capabilities().preferredDepthStencilFormat;
    ImageHandle depth = device.createImage(
        {{width, height, 1}, depthFormat, ResourceUsage::DepthStencilAttachment,
         1, 1, 1}, status);
    if (!status) return fail("create R5 world depth target", status);
    ImageViewHandle depthView = device.createImageView(
        {depth, depthFormat, {ImageAspect::DepthStencil, 0, 1, 0, 1}}, status);
    if (!status) return fail("create R5 world depth view", status);

    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    if (!status) return fail("create R5 world shader", status);
    BindingSetDesc bindings;
    bindings.shader = shader;
    bindings.group = 0;
    bindings.resources.push_back({0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
        uniform, 0, sizeof(world), {}, {}});
    for (std::uint16_t binding = 1; binding <= 6; ++binding)
        bindings.resources.push_back({binding, 0,
            ShaderPackageDesc::BindingType::CombinedImageSampler, {}, 0, 0,
            textureViews[binding - 1], sampler});
    BindingSetHandle bindingSet = device.createBindingSet(bindings, status);
    if (!status) return fail("create R5 world bindings", status);

    PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader;
    pipelineDesc.cullMode = CullMode::Back;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompare = CompareOp::GreaterEqual;
    pipelineDesc.colorFormats.assign(colorFormats.begin(), colorFormats.end());
    pipelineDesc.depthStencilFormat = depthFormat;
    pipelineDesc.blendStates.assign(3, BlendState{});
    pipelineDesc.vertexBuffers = {{0, sizeof(Vertex), VertexInputRate::PerVertex}};
    pipelineDesc.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, offsetof(Vertex, position)},
        {1, 0, VertexFormat::Float32x3, offsetof(Vertex, normal)},
        {2, 0, VertexFormat::Float32x2, offsetof(Vertex, texCoord)}};
    PipelineHandle pipeline = device.createPipeline(pipelineDesc, status);
    if (!status) return fail("create R5 world pipeline", status);

    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame())) return fail("begin R5 world frame", status);
    if (!(status = commands.copyBuffer(upload, vertex,
            std::array<BufferCopyRegion, 1>{{{vertexOffset, 0, sizeof(vertices)}}})) ||
        !(status = commands.copyBuffer(upload, index,
            std::array<BufferCopyRegion, 1>{{{indexOffset, 0, sizeof(indices)}}})) ||
        !(status = commands.copyBuffer(upload, uniform,
            std::array<BufferCopyRegion, 1>{{{worldOffset, 0, sizeof(world)}}})))
        return fail("upload R5 world buffers", status);
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        BufferImageCopyRegion copy;
        copy.bufferOffset = textureOffset + i * sizeof(texels.front());
        copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
        copy.imageExtent = {2, 2, 1};
        if (!(status = commands.copyBufferToImage(upload, textures[i],
                std::array<BufferImageCopyRegion, 1>{{copy}})))
            return fail("upload R5 world texture", status);
    }

    RenderingInfo rendering;
    rendering.semanticId = 0x52355f574f524c44ull; // "R5_WORLD"
    rendering.width = width;
    rendering.height = height;
    for (std::size_t i = 0; i < colors.size(); ++i)
        rendering.colors.push_back(
            {colorViews[i], colorFormats[i], LoadOp::Clear, StoreOp::Store, {}});
    rendering.depthStencil = AttachmentDesc{
        depthView, depthFormat, LoadOp::Clear, StoreOp::Store,
        {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};
    if (!(status = commands.beginRendering(rendering)) ||
        !(status = commands.bindPipeline(pipeline)) ||
        !(status = commands.bindBindingSet(0, bindingSet)) ||
        !(status = commands.setViewport({0.f, 0.f, static_cast<float>(width),
                                         static_cast<float>(height), 0.f, 1.f})) ||
        !(status = commands.setScissor({0, 0, width, height})) ||
        !(status = commands.bindVertexBuffer(0, vertex, 0)) ||
        !(status = commands.bindIndexBuffer(index, 0, IndexType::UInt16)) ||
        !(status = commands.drawIndexed({6, 1, 0, 0, 0})) ||
        !(status = commands.endRendering()))
        return fail("execute R5 world draw", status);
    for (std::size_t i = 0; i < colors.size(); ++i)
    {
        BufferImageCopyRegion copy;
        copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        if (!(status = commands.copyImageToBuffer(colors[i], readbacks[i],
                std::array<BufferImageCopyRegion, 1>{{copy}})))
            return fail("copy R5 world target", status);
    }
    if (!(status = commands.endFrame())) return fail("end R5 world frame", status);

    WorldFixtureResult result;
    result.depthStencilFormat = depthFormat;
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        std::vector<std::byte> pixels(width * height * 4);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do
        {
            status = device.readBuffer(readbacks[target], 0, pixels);
            if (status || status.code() != StatusCode::NotReady) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!status) return fail("read R5 world target", status);
        if (device.backend() == Backend::OpenGL)
        {
            constexpr std::size_t rowBytes = width * 4;
            for (std::uint32_t y = 0; y < height / 2; ++y)
                std::swap_ranges(pixels.begin() + y * rowBytes,
                    pixels.begin() + (y + 1) * rowBytes,
                    pixels.begin() + (height - 1 - y) * rowBytes);
        }
        result.colorSha256[target] = sha256(pixels);
        for (std::size_t pixel = 0; pixel < width * height; ++pixel)
            if (pixels[pixel * 4 + 3] != std::byte{0})
                ++result.shadedPixelCount[target];
        if (result.shadedPixelCount[target] != 48u * 48u)
            return {false, "R5 world target coverage mismatch", depthFormat,
                    result.colorSha256, result.shadedPixelCount};
        if (referenceHashes[target][0] && result.colorSha256[target] != referenceHashes[target])
            return {false, "R5 world target hash mismatch: " + result.colorSha256[target],
                    depthFormat, result.colorSha256, result.shadedPixelCount};
    }

    for (Status destroyStatus : {
        device.destroy(pipeline), device.destroy(bindingSet), device.destroy(shader),
        device.destroy(depthView), device.destroy(depth), device.destroy(sampler),
        device.destroy(upload), device.destroy(vertex), device.destroy(index),
        device.destroy(uniform)})
        if (!destroyStatus) return fail("destroy R5 world resource", destroyStatus);
    for (std::size_t i = 0; i < colors.size(); ++i)
    {
        if (!(status = device.destroy(colorViews[i])) ||
            !(status = device.destroy(colors[i])) ||
            !(status = device.destroy(readbacks[i])))
            return fail("destroy R5 world target", status);
    }
    for (std::size_t i = 0; i < textures.size(); ++i)
        if (!(status = device.destroy(textureViews[i])) ||
            !(status = device.destroy(textures[i])))
            return fail("destroy R5 world texture", status);
    if (!(status = device.waitIdle())) return fail("wait for R5 world retirement", status);
    result.passed = true;
    result.message = "R5c-e world fixture PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIWORLDFIXTURE_H
