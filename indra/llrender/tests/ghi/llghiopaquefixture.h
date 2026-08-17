/**
 * @file llghiopaquefixture.h
 * @brief Backend-independent R4 static opaque deferred workload.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIOPAQUEFIXTURE_H
#define LL_LLGHIOPAQUEFIXTURE_H

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

struct OpaqueFixtureResult
{
    bool passed = false;
    std::string message;
    Format depthStencilFormat = Format::Undefined;
    std::array<std::string, 4> colorSha256;
};

inline OpaqueFixtureResult runOpaqueFixture(
    Device& device,
    const ShaderPackageDesc& shaderPackage)
{
    auto fail = [](const char* operation, const Status& status)
    {
        return OpaqueFixtureResult{
            false, std::string(operation) + ": " + status.message()};
    };
    struct Vertex
    {
        float position[3];
        std::uint8_t color[4];
    };
    static_assert(sizeof(Vertex) == 16);
    constexpr std::array<Vertex, 8> vertices{{
        {{-0.75f, -0.75f, 0.25f}, {255, 64, 32, 255}},
        {{ 0.75f, -0.75f, 0.25f}, {255, 64, 32, 255}},
        {{ 0.75f,  0.75f, 0.25f}, {255, 64, 32, 255}},
        {{-0.75f,  0.75f, 0.25f}, {255, 64, 32, 255}},
        {{-0.375f, -0.375f, 0.75f}, {32, 255, 64, 255}},
        {{ 0.375f, -0.375f, 0.75f}, {32, 255, 64, 255}},
        {{ 0.375f,  0.375f, 0.75f}, {32, 255, 64, 255}},
        {{-0.375f,  0.375f, 0.75f}, {32, 255, 64, 255}},
    }};
    constexpr std::array<std::uint16_t, 12> indices{{
        0, 1, 2, 2, 3, 0,
        4, 5, 6, 6, 7, 4,
    }};
    constexpr std::array<float, 16> transform{{
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f,
    }};
    constexpr std::array<float, 16> material{{
        1.f, 1.f, 1.f, 1.f,
        0.25f, 0.5f, 0.75f, 1.f,
        0.5f, 0.5f, 1.f, 1.f,
        0.125f, 0.25f, 0.5f, 1.f,
    }};
    constexpr std::uint64_t vertexOffset = 0;
    constexpr std::uint64_t indexOffset = vertexOffset + sizeof(vertices);
    constexpr std::uint64_t frameOffset = indexOffset + sizeof(indices);
    constexpr std::uint64_t materialOffset = frameOffset + sizeof(transform);
    constexpr std::uint64_t uploadBytes = materialOffset + sizeof(material);
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr std::array<Format, 4> colorFormats{{
        Format::RGBA8UNorm,
        Format::RGBA8UNorm,
        Format::RGBA16UNorm,
        Format::RGBA16Float,
    }};
    constexpr std::array<std::uint32_t, 4> bytesPerPixel{{4, 4, 8, 8}};
    constexpr std::array<const char*, 4> referenceHashes{{
        "4976657fd6e31431fd86dbd9d96624938294208ade4bb9a4a7da04ee816e3c64",
        "24fd8e8c34e313665797af0b0b5ade49486a0f71dc3e58cceba14d0f7efd3ce6",
        "43daa53d0fb687a99cb761a89248b1eef93340d86633634f686501327c99169b",
        "f03533b1c62faf9e85948f8e93260f935e1e16c5fefca87e4db17fe2cfe84803",
    }};

    Status status = Status::success();
    BufferHandle upload = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status) return fail("create opaque upload buffer", status);
    BufferHandle vertex = device.createBuffer(
        {sizeof(vertices), ResourceUsage::Vertex | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create opaque vertex buffer", status);
    BufferHandle index = device.createBuffer(
        {sizeof(indices), ResourceUsage::Index | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create opaque index buffer", status);
    BufferHandle frame = device.createBuffer(
        {sizeof(transform), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create opaque frame buffer", status);
    BufferHandle materialBuffer = device.createBuffer(
        {sizeof(material), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create opaque material buffer", status);

    std::array<std::byte, uploadBytes> uploadData{};
    std::memcpy(uploadData.data() + vertexOffset, vertices.data(), sizeof(vertices));
    std::memcpy(uploadData.data() + indexOffset, indices.data(), sizeof(indices));
    std::memcpy(uploadData.data() + frameOffset, transform.data(), sizeof(transform));
    std::memcpy(uploadData.data() + materialOffset, material.data(), sizeof(material));
    if (!(status = device.writeBuffer(upload, 0, uploadData)))
        return fail("write opaque upload buffer", status);

    std::array<ImageHandle, 4> colors;
    std::array<ImageViewHandle, 4> colorViews;
    std::array<BufferHandle, 4> readbacks;
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        colors[target] = device.createImage(
            {{width, height, 1}, colorFormats[target],
             ResourceUsage::ColorAttachment | ResourceUsage::TransferSource,
             1, 1, 1}, status);
        if (!status) return fail("create opaque color target", status);
        colorViews[target] = device.createImageView(
            {colors[target], colorFormats[target],
             {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (!status) return fail("create opaque color view", status);
        readbacks[target] = device.createBuffer(
            {static_cast<std::uint64_t>(width) * height * bytesPerPixel[target],
             ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        if (!status) return fail("create opaque readback buffer", status);
    }

    const Format depthFormat = device.capabilities().preferredDepthStencilFormat;
    if (depthFormat == Format::Undefined)
        return {false, "device did not select an opaque depth/stencil format"};
    ImageHandle depth = device.createImage(
        {{width, height, 1}, depthFormat, ResourceUsage::DepthStencilAttachment,
         1, 1, 1}, status);
    if (!status) return fail("create opaque depth target", status);
    ImageViewHandle depthView = device.createImageView(
        {depth, depthFormat, {ImageAspect::DepthStencil, 0, 1, 0, 1}}, status);
    if (!status) return fail("create opaque depth view", status);

    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    if (!status) return fail("create opaque shader package", status);
    BindingSetDesc frameSetDesc;
    frameSetDesc.shader = shader;
    frameSetDesc.group = 0;
    frameSetDesc.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         frame, 0, sizeof(transform), {}, {}});
    BindingSetHandle frameSet = device.createBindingSet(frameSetDesc, status);
    if (!status) return fail("create opaque frame bindings", status);
    BindingSetDesc materialSetDesc;
    materialSetDesc.shader = shader;
    materialSetDesc.group = 2;
    materialSetDesc.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         materialBuffer, 0, sizeof(material), {}, {}});
    BindingSetHandle materialSet = device.createBindingSet(materialSetDesc, status);
    if (!status) return fail("create opaque material bindings", status);

    PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader;
    pipelineDesc.cullMode = CullMode::Back;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompare = CompareOp::GreaterEqual;
    pipelineDesc.colorFormats.assign(colorFormats.begin(), colorFormats.end());
    pipelineDesc.depthStencilFormat = depthFormat;
    pipelineDesc.blendStates.assign(4, BlendState{});
    // Distinct masks prove that attachment state is not collapsed to target 0.
    pipelineDesc.blendStates[1].colorWriteMask = 0x07;
    pipelineDesc.blendStates[2].colorWriteMask = 0x0b;
    pipelineDesc.blendStates[3].colorWriteMask = 0x0d;
    pipelineDesc.vertexBuffers = {{0, sizeof(Vertex), VertexInputRate::PerVertex}};
    pipelineDesc.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0},
        {1, 0, VertexFormat::UNorm8x4, 12},
    };
    PipelineHandle pipeline = device.createPipeline(pipelineDesc, status);
    if (!status) return fail("create opaque pipeline", status);

    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame())) return fail("begin opaque frame", status);
    const std::array<BufferCopyRegion, 1> vertexCopy{{{vertexOffset, 0, sizeof(vertices)}}};
    const std::array<BufferCopyRegion, 1> indexCopy{{{indexOffset, 0, sizeof(indices)}}};
    const std::array<BufferCopyRegion, 1> frameCopy{{{frameOffset, 0, sizeof(transform)}}};
    const std::array<BufferCopyRegion, 1> materialCopy{{{materialOffset, 0, sizeof(material)}}};
    if (!(status = commands.copyBuffer(upload, vertex, vertexCopy)))
        return fail("upload opaque vertices", status);
    if (!(status = commands.copyBuffer(upload, index, indexCopy)))
        return fail("upload opaque indices", status);
    if (!(status = commands.copyBuffer(upload, frame, frameCopy)))
        return fail("upload opaque frame data", status);
    if (!(status = commands.copyBuffer(upload, materialBuffer, materialCopy)))
        return fail("upload opaque material data", status);

    RenderingInfo rendering;
    rendering.semanticId = 0x52345f4f504151ull; // "R4_OPAQ"
    rendering.width = width;
    rendering.height = height;
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        const float component = 0.05f * static_cast<float>(target + 1);
        rendering.colors.push_back(
            {colorViews[target], colorFormats[target], LoadOp::Clear, StoreOp::Store,
             {{component, component, component, component}, 0.f, 0}});
    }
    rendering.depthStencil = AttachmentDesc{
        depthView, depthFormat, LoadOp::Clear, StoreOp::Store,
        {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};
    if (!(status = commands.beginRendering(rendering)))
        return fail("begin opaque rendering", status);
    if (!(status = commands.bindPipeline(pipeline))) return fail("bind opaque pipeline", status);
    if (!(status = commands.bindBindingSet(0, frameSet)))
        return fail("bind opaque frame resources", status);
    if (!(status = commands.bindBindingSet(2, materialSet)))
        return fail("bind opaque material resources", status);
    if (!(status = commands.setViewport({0.f, 0.f, static_cast<float>(width),
                                         static_cast<float>(height), 0.f, 1.f})))
        return fail("set opaque viewport", status);
    if (!(status = commands.setScissor({0, 0, width, height})))
        return fail("set opaque scissor", status);
    if (!(status = commands.bindVertexBuffer(0, vertex, 0)))
        return fail("bind opaque vertex buffer", status);
    if (!(status = commands.bindIndexBuffer(index, 0, IndexType::UInt16)))
        return fail("bind opaque index buffer", status);
    if (!(status = commands.drawIndexed({static_cast<std::uint32_t>(indices.size()),
                                         1, 0, 0, 0})))
        return fail("draw opaque geometry", status);
    if (!(status = commands.endRendering())) return fail("end opaque rendering", status);
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        BufferImageCopyRegion copy;
        copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        const std::array<BufferImageCopyRegion, 1> copies{{copy}};
        if (!(status = commands.copyImageToBuffer(colors[target], readbacks[target], copies)))
            return fail("copy opaque target to readback", status);
    }
    if (!(status = commands.endFrame())) return fail("end opaque frame", status);

    OpaqueFixtureResult result;
    result.depthStencilFormat = depthFormat;
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        std::vector<std::byte> pixels(
            static_cast<std::size_t>(width) * height * bytesPerPixel[target]);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do
        {
            status = device.readBuffer(readbacks[target], 0, pixels);
            if (status || status.code() != StatusCode::NotReady) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!status) return fail("read opaque target", status);
        if (device.backend() == Backend::OpenGL)
        {
            const std::size_t rowBytes = width * bytesPerPixel[target];
            for (std::uint32_t y = 0; y < height / 2; ++y)
            {
                auto top = pixels.begin() + static_cast<std::ptrdiff_t>(y * rowBytes);
                auto bottom = pixels.begin() + static_cast<std::ptrdiff_t>(
                    (height - 1 - y) * rowBytes);
                std::swap_ranges(top, top + rowBytes, bottom);
            }
        }
        result.colorSha256[target] = sha256(pixels);
        if (result.colorSha256[target] != referenceHashes[target])
        {
            return {false,
                "opaque target " + std::to_string(target) +
                    " SHA-256 mismatch: " + result.colorSha256[target],
                depthFormat, result.colorSha256};
        }
    }

    for (Status destroyStatus : {
        device.destroy(pipeline), device.destroy(frameSet), device.destroy(materialSet),
        device.destroy(shader), device.destroy(depthView), device.destroy(depth),
        device.destroy(upload), device.destroy(vertex), device.destroy(index),
        device.destroy(frame), device.destroy(materialBuffer)})
    {
        if (!destroyStatus) return fail("destroy opaque fixture resource", destroyStatus);
    }
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        if (!(status = device.destroy(colorViews[target])))
            return fail("destroy opaque color view", status);
        if (!(status = device.destroy(colors[target])))
            return fail("destroy opaque color target", status);
        if (!(status = device.destroy(readbacks[target])))
            return fail("destroy opaque readback", status);
    }
    if (!(status = device.waitIdle())) return fail("wait for opaque retirement", status);
    result.passed = true;
    result.message = "R4 static opaque deferred fixture PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIOPAQUEFIXTURE_H
