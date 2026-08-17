/**
 * @file llghidepthpeelfixture.h
 * @brief Backend-independent R6 reverse-Z depth-peeling fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIDEPTHPEELFIXTURE_H
#define LL_LLGHIDEPTHPEELFIXTURE_H

#include "ghi/core/llghihash.h"
#include "ghi/include/llghi.h"

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

struct DepthPeelFixtureResult
{
    bool passed = false;
    std::string message;
    std::string colorSha256;
    std::uint32_t shadedPixelCount = 0;
    std::uint32_t peeledLayers = 0;
    bool residualTailRendered = false;
};

inline DepthPeelFixtureResult runDepthPeelFixture(
    Device& device, const ShaderPackageDesc& shaderPackage)
{
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr std::uint32_t coverage = 48u * 48u;
    constexpr std::uint32_t layers = 5;
    constexpr std::uint32_t maximumPeeledLayers = 4;
    constexpr const char* referenceHash =
        "1ee268e3a82ff0bc733b8998e35839f8ec8e23387b3fb18592e082380ce2e9e1";
    struct Vertex { float position[3]; float color[4]; };
    constexpr std::array<float, layers> depths{{0.6f, 0.3f, 0.0f, -0.3f, -0.6f}};
    constexpr std::array<std::array<float, 4>, layers> colors{{
        {{0.95f, 0.15f, 0.10f, 0.35f}},
        {{0.10f, 0.85f, 0.20f, 0.45f}},
        {{0.15f, 0.25f, 0.95f, 0.55f}},
        {{0.90f, 0.80f, 0.10f, 0.25f}},
        {{0.75f, 0.15f, 0.80f, 0.40f}},
    }};
    std::array<Vertex, layers * 4u> vertices{};
    constexpr std::array<std::array<float, 2>, 4> corners{{
        {{-0.75f, -0.75f}}, {{0.75f, -0.75f}},
        {{0.75f, 0.75f}}, {{-0.75f, 0.75f}},
    }};
    for (std::size_t layer = 0; layer < layers; ++layer)
        for (std::size_t corner = 0; corner < corners.size(); ++corner)
        {
            Vertex& vertex = vertices[layer * 4u + corner];
            vertex.position[0] = corners[corner][0];
            vertex.position[1] = corners[corner][1];
            vertex.position[2] = depths[layer];
            std::copy(colors[layer].begin(), colors[layer].end(), vertex.color);
        }
    constexpr std::array<std::uint16_t, 6> indices{{0, 1, 2, 2, 3, 0}};
    struct alignas(16) PeelUniform
    {
        std::uint32_t config[4];
        float opaqueDepth[4];
    };
    static_assert(sizeof(PeelUniform) == 32);
    constexpr std::uint32_t passCount = maximumPeeledLayers + 1u;
    std::array<PeelUniform, passCount> uniforms{};
    for (std::uint32_t pass = 0; pass < passCount; ++pass)
        uniforms[pass] = {{pass, pass == maximumPeeledLayers ? 1u : 0u, 0u, 0u},
                          {0.1f, 0.f, 0.f, 0.f}};

    DepthPeelFixtureResult result;
    auto fail = [&](const char* operation, const Status& status)
    {
        result.message = std::string(operation) + ": " + status.message();
        return result;
    };
    auto mismatch = [&](std::string message)
    {
        result.message = std::move(message);
        return result;
    };
    Status status = Status::success();
    const Format depthFormat = device.capabilities().preferredDepthStencilFormat;
    if (depthFormat == Format::Undefined)
        return mismatch("R6 depth peeling requires a depth/stencil format");

    constexpr std::uint64_t vertexOffset = 0;
    constexpr std::uint64_t indexOffset = vertexOffset + sizeof(vertices);
    constexpr std::uint64_t uniformOffset = indexOffset + sizeof(indices);
    constexpr std::uint64_t uploadBytes = uniformOffset + sizeof(uniforms);
    constexpr std::uint64_t colorBytes = width * height * 4u;
    std::vector<std::byte> uploadData(uploadBytes);
    std::memcpy(uploadData.data() + vertexOffset, vertices.data(), sizeof(vertices));
    std::memcpy(uploadData.data() + indexOffset, indices.data(), sizeof(indices));
    std::memcpy(uploadData.data() + uniformOffset, uniforms.data(), sizeof(uniforms));

    BufferHandle upload = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status) return fail("create R6 depth-peel upload", status);
    if (!(status = device.writeBuffer(upload, 0, uploadData)))
        return fail("write R6 depth-peel upload", status);
    BufferHandle vertex = device.createBuffer(
        {sizeof(vertices), ResourceUsage::Vertex | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R6 depth-peel vertex buffer", status);
    BufferHandle index = device.createBuffer(
        {sizeof(indices), ResourceUsage::Index | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R6 depth-peel index buffer", status);
    std::array<BufferHandle, passCount> uniformBuffers{};
    for (BufferHandle& uniform : uniformBuffers)
    {
        uniform = device.createBuffer(
            {sizeof(PeelUniform), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
        if (!status) return fail("create R6 depth-peel uniform", status);
    }
    ImageHandle color = device.createImage(
        {{width, height, 1}, Format::RGBA8UNorm,
         ResourceUsage::ColorAttachment | ResourceUsage::TransferSource, 1, 1, 1}, status);
    if (!status) return fail("create R6 depth-peel color", status);
    ImageViewHandle colorView = device.createImageView(
        {color, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    if (!status) return fail("create R6 depth-peel color view", status);
    BufferHandle readback = device.createBuffer(
        {colorBytes, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
    if (!status) return fail("create R6 depth-peel readback", status);

    std::array<ImageHandle, 2> depth{};
    std::array<ImageViewHandle, 2> depthViews{};
    std::array<ImageViewHandle, 2> sampledDepthViews{};
    for (std::size_t i = 0; i < depth.size(); ++i)
    {
        depth[i] = device.createImage(
            {{width, height, 1}, depthFormat,
             ResourceUsage::DepthStencilAttachment | ResourceUsage::Sampled,
             1, 1, 1}, status);
        if (!status) return fail("create R6 depth-peel depth target", status);
        depthViews[i] = device.createImageView(
            {depth[i], depthFormat, {ImageAspect::DepthStencil, 0, 1, 0, 1}}, status);
        if (!status) return fail("create R6 depth-peel depth view", status);
        sampledDepthViews[i] = device.createImageView(
            {depth[i], depthFormat, {ImageAspect::Depth, 0, 1, 0, 1}}, status);
        if (!status) return fail("create R6 depth-peel sampled depth view", status);
    }
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = samplerDesc.magFilter = samplerDesc.mipFilter = Filter::Nearest;
    samplerDesc.addressU = samplerDesc.addressV = AddressMode::ClampToEdge;
    SamplerHandle sampler = device.createSampler(samplerDesc, status);
    if (!status) return fail("create R6 depth-peel sampler", status);

    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    if (!status) return fail("create R6 depth-peel shader", status);
    std::array<BindingSetHandle, passCount> bindingSets{};
    for (std::uint32_t pass = 0; pass < passCount; ++pass)
    {
        const std::uint32_t priorIndex = pass == 0u ? 1u : (pass - 1u) & 1u;
        BindingSetDesc bindings;
        bindings.shader = shader;
        bindings.group = 0;
        bindings.resources = {
            {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
             uniformBuffers[pass], 0, sizeof(PeelUniform), {}, {}},
            {1, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
             {}, 0, 0, sampledDepthViews[priorIndex], sampler},
        };
        bindingSets[pass] = device.createBindingSet(bindings, status);
        if (!status) return fail("create R6 depth-peel bindings", status);
    }

    PipelineDesc peelDesc;
    peelDesc.shader = shader;
    peelDesc.cullMode = CullMode::None;
    peelDesc.depthTest = true;
    peelDesc.depthWrite = true;
    peelDesc.depthCompare = CompareOp::Greater;
    peelDesc.colorFormats = {Format::RGBA8UNorm};
    peelDesc.depthStencilFormat = depthFormat;
    BlendState behindBlend;
    behindBlend.enabled = true;
    behindBlend.sourceColor = BlendFactor::OneMinusDestinationAlpha;
    behindBlend.destinationColor = BlendFactor::One;
    behindBlend.sourceAlpha = BlendFactor::OneMinusDestinationAlpha;
    behindBlend.destinationAlpha = BlendFactor::One;
    peelDesc.blendStates = {behindBlend};
    peelDesc.vertexBuffers = {{0, sizeof(Vertex), VertexInputRate::PerVertex}};
    peelDesc.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, offsetof(Vertex, position)},
        {1, 0, VertexFormat::Float32x4, offsetof(Vertex, color)},
    };
    PipelineHandle peelPipeline = device.createPipeline(peelDesc, status);
    if (!status) return fail("create R6 depth-peel pipeline", status);
    PipelineDesc residualDesc = peelDesc;
    residualDesc.depthTest = false;
    residualDesc.depthWrite = false;
    residualDesc.depthStencilFormat.reset();
    PipelineHandle residualPipeline = device.createPipeline(residualDesc, status);
    if (!status) return fail("create R6 depth-peel residual pipeline", status);

    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame())) return fail("begin R6 depth-peel frame", status);
    if (!(status = commands.copyBuffer(upload, vertex,
            std::array<BufferCopyRegion, 1>{{{vertexOffset, 0, sizeof(vertices)}}})) ||
        !(status = commands.copyBuffer(upload, index,
            std::array<BufferCopyRegion, 1>{{{indexOffset, 0, sizeof(indices)}}})))
        return fail("upload R6 depth-peel geometry", status);
    for (std::uint32_t pass = 0; pass < passCount; ++pass)
        if (!(status = commands.copyBuffer(upload, uniformBuffers[pass],
                std::array<BufferCopyRegion, 1>{{{
                    uniformOffset + pass * sizeof(PeelUniform), 0, sizeof(PeelUniform)}}})))
            return fail("upload R6 depth-peel uniforms", status);

    auto setDrawState = [&](PipelineHandle pipeline, BindingSetHandle bindings)
    {
        return (status = commands.bindPipeline(pipeline)) &&
               (status = commands.bindBindingSet(0, bindings)) &&
               (status = commands.setViewport(
                   {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f})) &&
               (status = commands.setScissor({0, 0, width, height})) &&
               (status = commands.bindIndexBuffer(index, 0, IndexType::UInt16));
    };
    for (std::uint32_t pass = 0; pass < maximumPeeledLayers; ++pass)
    {
        RenderingInfo rendering;
        rendering.semanticId = 0x52365f5045454c00ull + pass;
        rendering.width = width;
        rendering.height = height;
        rendering.colors.push_back({colorView, Format::RGBA8UNorm,
            pass == 0u ? LoadOp::Clear : LoadOp::Load, StoreOp::Store,
            {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
        rendering.depthStencil = AttachmentDesc{
            depthViews[pass & 1u], depthFormat, LoadOp::Clear, StoreOp::Store,
            {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};
        if (!(status = commands.beginRendering(rendering)) ||
            !setDrawState(peelPipeline, bindingSets[pass]))
            return fail("begin R6 depth-peel pass", status);
        for (std::uint32_t layer = 0; layer < layers; ++layer)
            if (!(status = commands.bindVertexBuffer(
                    0, vertex, layer * 4u * sizeof(Vertex))) ||
                !(status = commands.drawIndexed({6, 1, 0, 0, 0})))
                return fail("draw R6 depth-peel candidates", status);
        if (!(status = commands.endRendering()) ||
            !(status = commands.resourceBarrier(
                ResourceBarrier::DepthAttachmentWriteToSampledRead)))
            return fail("finish R6 depth-peel pass", status);
        ++result.peeledLayers;
    }

    RenderingInfo residual;
    residual.semanticId = 0x52365f5045454c54ull;
    residual.width = width;
    residual.height = height;
    residual.colors.push_back({colorView, Format::RGBA8UNorm,
        LoadOp::Load, StoreOp::Store, {}});
    if (!(status = commands.beginRendering(residual)) ||
        !setDrawState(residualPipeline, bindingSets[maximumPeeledLayers]))
        return fail("begin R6 depth-peel residual", status);
    // Legacy residual replay is back-to-front. Four exact peels leave only the
    // farthest layer in this fixture, while the loop preserves the real route.
    for (std::uint32_t reverse = layers; reverse > 0u; --reverse)
        if (!(status = commands.bindVertexBuffer(
                0, vertex, (reverse - 1u) * 4u * sizeof(Vertex))) ||
            !(status = commands.drawIndexed({6, 1, 0, 0, 0})))
            return fail("draw R6 depth-peel residual", status);
    if (!(status = commands.endRendering()))
        return fail("finish R6 depth-peel residual", status);
    result.residualTailRendered = true;

    BufferImageCopyRegion colorCopy;
    colorCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
    colorCopy.imageExtent = {width, height, 1};
    if (!(status = commands.copyImageToBuffer(color, readback,
            std::array<BufferImageCopyRegion, 1>{{colorCopy}})) ||
        !(status = commands.endFrame()))
        return fail("read back R6 depth-peel result", status);

    std::vector<std::byte> pixels(colorBytes);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do
    {
        status = device.readBuffer(readback, 0, pixels);
        if (status || status.code() != StatusCode::NotReady) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!status) return fail("read R6 depth-peel pixels", status);
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
    if (result.shadedPixelCount != coverage)
        return mismatch("R6 depth-peel coverage mismatch");
    if (referenceHash[0] && result.colorSha256 != referenceHash)
        return mismatch("R6 depth-peel hash mismatch: " + result.colorSha256);

    for (BindingSetHandle binding : bindingSets)
        if (!(status = device.destroy(binding)))
            return fail("destroy R6 depth-peel binding", status);
    for (BufferHandle uniform : uniformBuffers)
        if (!(status = device.destroy(uniform)))
            return fail("destroy R6 depth-peel uniform", status);
    for (std::size_t i = 0; i < depth.size(); ++i)
        if (!(status = device.destroy(sampledDepthViews[i])) ||
            !(status = device.destroy(depthViews[i])) ||
            !(status = device.destroy(depth[i])))
            return fail("destroy R6 depth-peel depth resource", status);
    for (Status destroyStatus : {
        device.destroy(peelPipeline), device.destroy(residualPipeline),
        device.destroy(shader), device.destroy(sampler), device.destroy(colorView),
        device.destroy(color), device.destroy(readback), device.destroy(upload),
        device.destroy(vertex), device.destroy(index)})
        if (!destroyStatus) return fail("destroy R6 depth-peel resource", destroyStatus);
    if (!(status = device.waitIdle())) return fail("wait for R6 depth-peel retirement", status);

    result.passed = true;
    result.message = "R6 depth-peeling fixture PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIDEPTHPEELFIXTURE_H
