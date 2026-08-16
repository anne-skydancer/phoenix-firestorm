/**
 * @file llghivisibilityfixture.h
 * @brief Backend-independent R4c dynamic instancing and occlusion workload.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIVISIBILITYFIXTURE_H
#define LL_LLGHIVISIBILITYFIXTURE_H

#include "ghi/core/llghihash.h"
#include "ghi/include/llghi.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace LL::GHI::Test
{

struct VisibilityFixtureResult
{
    bool passed = false;
    std::string message;
    Format depthStencilFormat = Format::Undefined;
    std::array<std::string, 2> colorSha256;
    std::array<std::uint64_t, 4> occlusionSamples{};
};

inline VisibilityFixtureResult runVisibilityFixture(
    Device& device,
    const ShaderPackageDesc& shaderPackage)
{
    auto fail = [](const char* operation, const Status& status)
    {
        return VisibilityFixtureResult{
            false, std::string(operation) + ": " + status.message()};
    };
    struct Vertex
    {
        float position[3];
        std::uint8_t color[4];
    };
    struct Instance
    {
        float offset[2];
        std::uint8_t color[4];
    };
    static_assert(sizeof(Vertex) == 16);
    static_assert(sizeof(Instance) == 12);

    constexpr std::array<Vertex, 8> vertices{{
        {{-0.18f, -0.18f, 0.75f}, {255, 255, 255, 255}},
        {{ 0.18f, -0.18f, 0.75f}, {255, 255, 255, 255}},
        {{ 0.18f,  0.18f, 0.75f}, {255, 255, 255, 255}},
        {{-0.18f,  0.18f, 0.75f}, {255, 255, 255, 255}},
        {{-0.18f, -0.18f, 0.25f}, {255, 255, 255, 255}},
        {{ 0.18f, -0.18f, 0.25f}, {255, 255, 255, 255}},
        {{ 0.18f,  0.18f, 0.25f}, {255, 255, 255, 255}},
        {{-0.18f,  0.18f, 0.25f}, {255, 255, 255, 255}},
    }};
    constexpr std::array<std::uint16_t, 6> indices{{0, 1, 2, 2, 3, 0}};
    constexpr std::array<Instance, 3> firstInstances{{
        {{-0.55f, -0.35f}, {255, 48, 32, 255}},
        {{ 0.00f,  0.35f}, {32, 255, 64, 255}},
        {{ 0.55f, -0.35f}, {48, 96, 255, 255}},
    }};
    constexpr std::array<Instance, 3> secondInstances{{
        {{-0.55f,  0.35f}, {255, 224, 32, 255}},
        {{ 0.00f, -0.35f}, {32, 255, 240, 255}},
        {{ 0.55f,  0.35f}, {240, 48, 255, 255}},
    }};

    constexpr std::uint64_t vertexOffset = 0;
    constexpr std::uint64_t indexOffset = vertexOffset + sizeof(vertices);
    constexpr std::uint64_t firstInstanceOffset = indexOffset + sizeof(indices);
    constexpr std::uint64_t secondInstanceOffset =
        firstInstanceOffset + sizeof(firstInstances);
    constexpr std::uint64_t uploadBytes =
        secondInstanceOffset + sizeof(secondInstances);
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr std::uint64_t pixelBytes =
        static_cast<std::uint64_t>(width) * height * 4;
    constexpr std::array<const char*, 2> referenceHashes{{
        "0b5cd61f5abad7022d68402f74b34ae539a2fb25fec6755a2864745d3bb150ae",
        "bec88ce09217d8c9456bdd6a656f5b53e0391efd37f17a60ab05c1df1a2df504",
    }};

    Status status = Status::success();
    BufferHandle upload = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status) return fail("create visibility upload buffer", status);
    BufferHandle vertex = device.createBuffer(
        {sizeof(vertices), ResourceUsage::Vertex | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create visibility vertex buffer", status);
    BufferHandle index = device.createBuffer(
        {sizeof(indices), ResourceUsage::Index | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create visibility index buffer", status);
    BufferHandle instance = device.createBuffer(
        {sizeof(firstInstances),
         ResourceUsage::Vertex | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create visibility instance buffer", status);

    std::array<std::byte, uploadBytes> uploadData{};
    std::memcpy(uploadData.data() + vertexOffset, vertices.data(), sizeof(vertices));
    std::memcpy(uploadData.data() + indexOffset, indices.data(), sizeof(indices));
    std::memcpy(uploadData.data() + firstInstanceOffset,
                firstInstances.data(), sizeof(firstInstances));
    std::memcpy(uploadData.data() + secondInstanceOffset,
                secondInstances.data(), sizeof(secondInstances));
    if (!(status = device.writeBuffer(upload, 0, uploadData)))
        return fail("write visibility upload buffer", status);

    ImageHandle color = device.createImage(
        {{width, height, 1}, Format::RGBA8UNorm,
         ResourceUsage::ColorAttachment | ResourceUsage::TransferSource,
         1, 1, 1}, status);
    if (!status) return fail("create visibility color target", status);
    ImageViewHandle colorView = device.createImageView(
        {color, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    if (!status) return fail("create visibility color view", status);
    std::array<BufferHandle, 2> readbacks;
    for (BufferHandle& readback : readbacks)
    {
        readback = device.createBuffer(
            {pixelBytes, ResourceUsage::TransferDestination, MemoryClass::Readback},
            status);
        if (!status) return fail("create visibility readback buffer", status);
    }

    const Format depthFormat = device.capabilities().preferredDepthStencilFormat;
    if (depthFormat == Format::Undefined)
        return {false, "device did not select a visibility depth/stencil format"};
    ImageHandle depth = device.createImage(
        {{width, height, 1}, depthFormat, ResourceUsage::DepthStencilAttachment,
         1, 1, 1}, status);
    if (!status) return fail("create visibility depth target", status);
    const ImageAspect depthAspect =
        depthFormat == Format::Depth24Stencil8 ||
        depthFormat == Format::Depth32FloatStencil8
            ? ImageAspect::DepthStencil : ImageAspect::Depth;
    ImageViewHandle depthView = device.createImageView(
        {depth, depthFormat, {depthAspect, 0, 1, 0, 1}}, status);
    if (!status) return fail("create visibility depth view", status);
    QueryPoolHandle queries = device.createQueryPool(
        {QueryType::Occlusion, 4}, status);
    if (!status) return fail("create visibility query pool", status);
    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    if (!status) return fail("create visibility shader package", status);

    PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader;
    pipelineDesc.cullMode = CullMode::Back;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompare = CompareOp::GreaterEqual;
    pipelineDesc.colorFormats = {Format::RGBA8UNorm};
    pipelineDesc.depthStencilFormat = depthFormat;
    pipelineDesc.blendStates = {BlendState{}};
    pipelineDesc.vertexBuffers = {
        {0, sizeof(Vertex), VertexInputRate::PerVertex},
        {1, sizeof(Instance), VertexInputRate::PerInstance},
    };
    pipelineDesc.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0},
        {1, 0, VertexFormat::UNorm8x4, 12},
        {2, 1, VertexFormat::Float32x2, 0},
        {3, 1, VertexFormat::UNorm8x4, 8},
    };
    PipelineHandle pipeline = device.createPipeline(pipelineDesc, status);
    if (!status) return fail("create visibility pipeline", status);

    RenderingInfo rendering;
    rendering.width = width;
    rendering.height = height;
    rendering.colors.push_back(
        {colorView, Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store,
         {{0.02f, 0.03f, 0.04f, 1.f}, 0.f, 0}});
    rendering.depthStencil = AttachmentDesc{
        depthView, depthFormat, LoadOp::Clear, StoreOp::Store,
        {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};

    CommandContext& commands = device.commandContext();
    for (std::uint32_t frameNumber = 0; frameNumber < 2; ++frameNumber)
    {
        if (!(status = commands.beginFrame()))
            return fail("begin visibility frame", status);
        if (frameNumber == 0)
        {
            const std::array<BufferCopyRegion, 1> vertexCopy{{
                {vertexOffset, 0, sizeof(vertices)}}};
            const std::array<BufferCopyRegion, 1> indexCopy{{
                {indexOffset, 0, sizeof(indices)}}};
            if (!(status = commands.copyBuffer(upload, vertex, vertexCopy)))
                return fail("upload visibility vertices", status);
            if (!(status = commands.copyBuffer(upload, index, indexCopy)))
                return fail("upload visibility indices", status);
        }
        const std::uint64_t sourceOffset = frameNumber == 0
            ? firstInstanceOffset : secondInstanceOffset;
        const std::array<BufferCopyRegion, 1> instanceCopy{{
            {sourceOffset, 0, sizeof(firstInstances)}}};
        if (!(status = commands.copyBuffer(upload, instance, instanceCopy)))
            return fail("update visibility instances", status);
        if (!(status = commands.resetQueryPool(queries, frameNumber * 2, 2)))
            return fail("reset visibility queries", status);

        rendering.semanticId = 0x5234635f56495330ull + frameNumber; // "R4c_VIS0/1"
        if (!(status = commands.beginRendering(rendering)))
            return fail("begin visibility rendering", status);
        if (!(status = commands.bindPipeline(pipeline)))
            return fail("bind visibility pipeline", status);
        if (!(status = commands.setViewport(
                  {0.f, 0.f, static_cast<float>(width),
                   static_cast<float>(height), 0.f, 1.f})))
            return fail("set visibility viewport", status);
        if (!(status = commands.setScissor({0, 0, width, height})))
            return fail("set visibility scissor", status);
        if (!(status = commands.bindIndexBuffer(index, 0, IndexType::UInt16)))
            return fail("bind visibility index buffer", status);
        if (!(status = commands.bindVertexBuffer(1, instance, 0)))
            return fail("bind visibility instance buffer", status);

        const std::uint32_t visibleQuery = frameNumber * 2;
        if (!(status = commands.bindVertexBuffer(0, vertex, 0)))
            return fail("bind visible geometry", status);
        if (!(status = commands.beginQuery(queries, visibleQuery)))
            return fail("begin visible occlusion query", status);
        if (!(status = commands.drawIndexed(
                  {static_cast<std::uint32_t>(indices.size()),
                   static_cast<std::uint32_t>(firstInstances.size()), 0, 0, 0})))
            return fail("draw visible instances", status);
        if (!(status = commands.endQuery(queries, visibleQuery)))
            return fail("end visible occlusion query", status);

        const std::uint32_t hiddenQuery = visibleQuery + 1;
        if (!(status = commands.bindVertexBuffer(
                  0, vertex, sizeof(Vertex) * 4)))
            return fail("bind occluded geometry", status);
        if (!(status = commands.beginQuery(queries, hiddenQuery)))
            return fail("begin hidden occlusion query", status);
        if (!(status = commands.drawIndexed(
                  {static_cast<std::uint32_t>(indices.size()),
                   static_cast<std::uint32_t>(firstInstances.size()), 0, 0, 0})))
            return fail("draw occluded instances", status);
        if (!(status = commands.endQuery(queries, hiddenQuery)))
            return fail("end hidden occlusion query", status);
        if (!(status = commands.endRendering()))
            return fail("end visibility rendering", status);

        BufferImageCopyRegion copy;
        copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        const std::array<BufferImageCopyRegion, 1> copies{{copy}};
        if (!(status = commands.copyImageToBuffer(
                  color, readbacks[frameNumber], copies)))
            return fail("copy visibility target to readback", status);
        if (!(status = commands.endFrame()))
            return fail("end visibility frame", status);
    }

    if (!(status = device.waitIdle())) return fail("wait for visibility work", status);

    VisibilityFixtureResult result;
    result.depthStencilFormat = depthFormat;
    if (!(status = device.getQueryResults(
              queries, 0, result.occlusionSamples, QueryReadMode::Wait)))
        return fail("read visibility query results", status);
    if (result.occlusionSamples[0] == 0 || result.occlusionSamples[2] == 0)
        return {false, "visible instanced geometry produced zero occlusion samples"};
    if (result.occlusionSamples[1] != 0 || result.occlusionSamples[3] != 0)
        return {false, "fully depth-occluded geometry produced visible samples"};

    for (std::size_t frameNumber = 0; frameNumber < readbacks.size(); ++frameNumber)
    {
        std::vector<std::byte> pixels(static_cast<std::size_t>(pixelBytes));
        if (!(status = device.readBuffer(readbacks[frameNumber], 0, pixels)))
            return fail("read visibility target", status);
        if (device.backend() == Backend::OpenGL)
        {
            const std::size_t rowBytes = width * 4;
            for (std::uint32_t y = 0; y < height / 2; ++y)
            {
                auto top = pixels.begin() + static_cast<std::ptrdiff_t>(y * rowBytes);
                auto bottom = pixels.begin() + static_cast<std::ptrdiff_t>(
                    (height - 1 - y) * rowBytes);
                std::swap_ranges(top, top + rowBytes, bottom);
            }
        }
        result.colorSha256[frameNumber] = sha256(pixels);
        if (result.colorSha256[frameNumber] != referenceHashes[frameNumber])
        {
            return {false,
                "visibility frame " + std::to_string(frameNumber) +
                    " SHA-256 mismatch: " + result.colorSha256[frameNumber],
                depthFormat, result.colorSha256, result.occlusionSamples};
        }
    }
    if (result.colorSha256[0] == result.colorSha256[1])
        return {false, "dynamic instance update did not change the rendered frame"};

    for (Status destroyStatus : {
        device.destroy(pipeline), device.destroy(shader), device.destroy(queries),
        device.destroy(depthView), device.destroy(depth), device.destroy(colorView),
        device.destroy(color), device.destroy(upload), device.destroy(vertex),
        device.destroy(index), device.destroy(instance), device.destroy(readbacks[0]),
        device.destroy(readbacks[1])})
    {
        if (!destroyStatus)
            return fail("destroy visibility fixture resource", destroyStatus);
    }
    if (!(status = device.waitIdle()))
        return fail("wait for visibility retirement", status);
    result.passed = true;
    result.message = "R4c dynamic instancing and occlusion fixture PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIVISIBILITYFIXTURE_H
