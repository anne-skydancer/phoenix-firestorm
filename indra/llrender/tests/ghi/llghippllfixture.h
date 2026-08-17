/**
 * @file llghippllfixture.h
 * @brief Backend-independent R6 bounded PPLL capture/resolve fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIPPLLFIXTURE_H
#define LL_LLGHIPPLLFIXTURE_H

#include "ghi/core/llghihash.h"
#include "ghi/include/llghi.h"
#include "ghi/include/llghialphacontract.h"

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

struct PPLLFixtureResult
{
    bool passed = false;
    std::string message;
    std::array<std::string, 2> colorSha256;
    std::array<std::uint32_t, 2> shadedPixelCount{};
    std::array<std::uint32_t, 2> allocatedNodeCount{};
    std::array<std::uint32_t, 2> overflowFragmentCount{};
};

inline PPLLFixtureResult runPPLLFixture(
    Device& device, const ShaderPackageDesc& shaderPackage)
{
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr std::uint32_t coverage = 48u * 48u;
    constexpr std::uint32_t layers = 5;
    constexpr std::uint32_t totalFragments = coverage * layers;
    constexpr std::array<std::uint32_t, 2> capacities{{
        totalFragments, coverage * 3u}};
    constexpr std::array<std::uint32_t, 2> exactLimits{{32u, 2u}};
    constexpr std::array<std::uint32_t, 2> expectedOverflow{{0u, coverage * 2u}};
    constexpr std::array<const char*, 2> referenceHashes{{
        "184f13a34ec9a56cbc5e7699e60c1a5d4e96ba3f9a26ad0ee9a301da7494b833",
        "458ea1a8f23eb54171a60fdf5e4500adb7fd5ad87e4556d16b488afce40398a6"}};

    struct Vertex
    {
        float position[3];
        float color[4];
    };
    // The first three layers are captured in the bounded case. The last two
    // are deliberately ordered far-to-near so their straight-alpha residual
    // is deterministic before the captured nearer set is resolved over it.
    constexpr std::array<float, layers> depths{{0.6f, 0.3f, 0.0f, -0.6f, -0.3f}};
    constexpr std::array<std::array<float, 4>, layers> colors{{
        {{0.95f, 0.15f, 0.10f, 0.35f}},
        {{0.10f, 0.85f, 0.20f, 0.45f}},
        {{0.15f, 0.25f, 0.95f, 0.55f}},
        {{0.90f, 0.80f, 0.10f, 0.25f}},
        {{0.75f, 0.15f, 0.80f, 0.40f}},
    }};
    std::array<Vertex, layers * 4u> vertices{};
    for (std::size_t layer = 0; layer < layers; ++layer)
    {
        const std::array<std::array<float, 2>, 4> corners{{
            {{-0.75f, -0.75f}}, {{0.75f, -0.75f}},
            {{0.75f, 0.75f}}, {{-0.75f, 0.75f}},
        }};
        for (std::size_t corner = 0; corner < corners.size(); ++corner)
        {
            Vertex& vertex = vertices[layer * 4u + corner];
            vertex.position[0] = corners[corner][0];
            vertex.position[1] = corners[corner][1];
            vertex.position[2] = depths[layer];
            std::copy(colors[layer].begin(), colors[layer].end(), vertex.color);
        }
    }
    constexpr std::array<std::uint16_t, 6> indices{{0, 1, 2, 2, 3, 0}};
    struct alignas(16) AlphaUniform
    {
        std::uint32_t config[4];
        float opaqueDepth[4];
    };
    static_assert(sizeof(AlphaUniform) == 32);

    struct CaseResources
    {
        BufferHandle captureUniform;
        BufferHandle resolveUniform;
        BufferHandle nodes;
        BufferHandle counter;
        BufferHandle counterReadback;
        BufferHandle colorReadback;
        ImageHandle head;
        ImageViewHandle headView;
        ImageHandle color;
        ImageViewHandle colorView;
        BindingSetHandle captureSet;
        BindingSetHandle resolveSet;
    };
    std::array<CaseResources, 2> cases{};
    PPLLFixtureResult result;
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

    if (!supportsPPLL(device.capabilities()))
        return mismatch("R6 PPLL fixture requires storage-image atomics and storage buffers");

    constexpr std::uint64_t vertexOffset = 0;
    constexpr std::uint64_t indexOffset = vertexOffset + sizeof(vertices);
    constexpr std::uint64_t fixedUploadBytes = indexOffset + sizeof(indices);
    constexpr std::uint64_t perCaseUploadBytes =
        sizeof(AlphaUniform) * 2u + width * height * sizeof(std::uint32_t) +
        2u * sizeof(std::uint32_t);
    constexpr std::uint64_t uploadBytes = fixedUploadBytes + 2u * perCaseUploadBytes;
    constexpr std::uint64_t colorBytes = width * height * 4u;

    Status status = Status::success();
    BufferHandle upload = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status) return fail("create R6 PPLL upload buffer", status);
    BufferHandle vertex = device.createBuffer(
        {sizeof(vertices), ResourceUsage::Vertex | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R6 PPLL vertex buffer", status);
    BufferHandle index = device.createBuffer(
        {sizeof(indices), ResourceUsage::Index | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R6 PPLL index buffer", status);

    std::vector<std::byte> uploadData(uploadBytes);
    std::memcpy(uploadData.data() + vertexOffset, vertices.data(), sizeof(vertices));
    std::memcpy(uploadData.data() + indexOffset, indices.data(), sizeof(indices));
    std::array<std::uint64_t, 2> captureOffsets{};
    std::array<std::uint64_t, 2> resolveOffsets{};
    std::array<std::uint64_t, 2> headOffsets{};
    std::array<std::uint64_t, 2> counterOffsets{};
    std::uint64_t cursor = fixedUploadBytes;
    for (std::size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex)
    {
        AlphaUniform capture{{0u, capacities[caseIndex], exactLimits[caseIndex], 0u},
                             {0.1f, 0.f, 0.f, 0.f}};
        AlphaUniform resolve{{1u, capacities[caseIndex], exactLimits[caseIndex], 0u},
                             {0.1f, 0.f, 0.f, 0.f}};
        captureOffsets[caseIndex] = cursor;
        std::memcpy(uploadData.data() + cursor, &capture, sizeof(capture));
        cursor += sizeof(capture);
        resolveOffsets[caseIndex] = cursor;
        std::memcpy(uploadData.data() + cursor, &resolve, sizeof(resolve));
        cursor += sizeof(resolve);
        headOffsets[caseIndex] = cursor;
        std::fill_n(reinterpret_cast<std::uint32_t*>(uploadData.data() + cursor),
                    width * height, 0xffffffffu);
        cursor += width * height * sizeof(std::uint32_t);
        counterOffsets[caseIndex] = cursor;
        std::fill_n(reinterpret_cast<std::uint32_t*>(uploadData.data() + cursor), 2, 0u);
        cursor += 2u * sizeof(std::uint32_t);
    }
    if (!(status = device.writeBuffer(upload, 0, uploadData)))
        return fail("write R6 PPLL upload buffer", status);

    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    if (!status) return fail("create R6 PPLL shader", status);
    for (std::size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex)
    {
        CaseResources& resource = cases[caseIndex];
        resource.captureUniform = device.createBuffer(
            {sizeof(AlphaUniform), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
        if (!status) return fail("create R6 PPLL capture uniform", status);
        resource.resolveUniform = device.createBuffer(
            {sizeof(AlphaUniform), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
        if (!status) return fail("create R6 PPLL resolve uniform", status);
        resource.nodes = device.createBuffer(
            {static_cast<std::uint64_t>(capacities[caseIndex]) * ALPHA_PPLL_NODE_BYTES,
             ResourceUsage::Storage, MemoryClass::DeviceLocal}, status);
        if (!status) return fail("create R6 PPLL node buffer", status);
        resource.counter = device.createBuffer(
            {8u, ResourceUsage::Storage | ResourceUsage::TransferDestination |
                     ResourceUsage::TransferSource,
             MemoryClass::DeviceLocal}, status);
        if (!status) return fail("create R6 PPLL counter", status);
        resource.counterReadback = device.createBuffer(
            {8u, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        if (!status) return fail("create R6 PPLL counter readback", status);
        resource.colorReadback = device.createBuffer(
            {colorBytes, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        if (!status) return fail("create R6 PPLL color readback", status);
        resource.head = device.createImage(
            {{width, height, 1}, Format::R32UInt,
             ResourceUsage::Storage | ResourceUsage::TransferDestination, 1, 1, 1}, status);
        if (!status) return fail("create R6 PPLL head image", status);
        resource.headView = device.createImageView(
            {resource.head, Format::R32UInt, {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (!status) return fail("create R6 PPLL head view", status);
        resource.color = device.createImage(
            {{width, height, 1}, Format::RGBA8UNorm,
             ResourceUsage::ColorAttachment | ResourceUsage::TransferSource, 1, 1, 1}, status);
        if (!status) return fail("create R6 PPLL color target", status);
        resource.colorView = device.createImageView(
            {resource.color, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (!status) return fail("create R6 PPLL color view", status);

        auto createSet = [&](BufferHandle uniform)
        {
            BindingSetDesc bindings;
            bindings.shader = shader;
            bindings.group = 0;
            bindings.resources = {
                {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
                 uniform, 0, sizeof(AlphaUniform), {}, {}},
                {1, 0, ShaderPackageDesc::BindingType::StorageImage,
                 {}, 0, 0, resource.headView, {}},
                {2, 0, ShaderPackageDesc::BindingType::StorageBuffer,
                 resource.nodes, 0,
                 static_cast<std::uint64_t>(capacities[caseIndex]) * ALPHA_PPLL_NODE_BYTES,
                 {}, {}},
                {3, 0, ShaderPackageDesc::BindingType::StorageBuffer,
                 resource.counter, 0, 8u, {}, {}},
            };
            return device.createBindingSet(bindings, status);
        };
        resource.captureSet = createSet(resource.captureUniform);
        if (!status) return fail("create R6 PPLL capture bindings", status);
        resource.resolveSet = createSet(resource.resolveUniform);
        if (!status) return fail("create R6 PPLL resolve bindings", status);
    }

    PipelineDesc captureDesc;
    captureDesc.shader = shader;
    captureDesc.cullMode = CullMode::None;
    captureDesc.depthTest = false;
    captureDesc.depthWrite = false;
    captureDesc.colorFormats = {Format::RGBA8UNorm};
    BlendState captureBlend;
    captureBlend.enabled = true;
    captureBlend.sourceColor = BlendFactor::SourceAlpha;
    captureBlend.destinationColor = BlendFactor::OneMinusSourceAlpha;
    captureBlend.sourceAlpha = BlendFactor::One;
    captureBlend.destinationAlpha = BlendFactor::OneMinusSourceAlpha;
    captureDesc.blendStates = {captureBlend};
    captureDesc.vertexBuffers = {{0, sizeof(Vertex), VertexInputRate::PerVertex}};
    captureDesc.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, offsetof(Vertex, position)},
        {1, 0, VertexFormat::Float32x4, offsetof(Vertex, color)},
    };
    PipelineHandle capturePipeline = device.createPipeline(captureDesc, status);
    if (!status) return fail("create R6 PPLL capture pipeline", status);
    PipelineDesc resolveDesc = captureDesc;
    BlendState resolveBlend;
    resolveBlend.enabled = true;
    resolveBlend.sourceColor = BlendFactor::One;
    resolveBlend.destinationColor = BlendFactor::OneMinusSourceAlpha;
    resolveBlend.sourceAlpha = BlendFactor::One;
    resolveBlend.destinationAlpha = BlendFactor::OneMinusSourceAlpha;
    resolveDesc.blendStates = {resolveBlend};
    PipelineHandle resolvePipeline = device.createPipeline(resolveDesc, status);
    if (!status) return fail("create R6 PPLL resolve pipeline", status);

    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame())) return fail("begin R6 PPLL frame", status);
    if (!(status = commands.copyBuffer(upload, vertex,
            std::array<BufferCopyRegion, 1>{{{vertexOffset, 0, sizeof(vertices)}}})) ||
        !(status = commands.copyBuffer(upload, index,
            std::array<BufferCopyRegion, 1>{{{indexOffset, 0, sizeof(indices)}}})))
        return fail("upload R6 PPLL geometry", status);
    for (std::size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex)
    {
        CaseResources& resource = cases[caseIndex];
        if (!(status = commands.copyBuffer(upload, resource.captureUniform,
                std::array<BufferCopyRegion, 1>{{{captureOffsets[caseIndex], 0,
                                                 sizeof(AlphaUniform)}}})) ||
            !(status = commands.copyBuffer(upload, resource.resolveUniform,
                std::array<BufferCopyRegion, 1>{{{resolveOffsets[caseIndex], 0,
                                                 sizeof(AlphaUniform)}}})) ||
            !(status = commands.copyBuffer(upload, resource.counter,
                std::array<BufferCopyRegion, 1>{{{counterOffsets[caseIndex], 0, 8u}}})))
            return fail("upload R6 PPLL case buffers", status);
        BufferImageCopyRegion headCopy;
        headCopy.bufferOffset = headOffsets[caseIndex];
        headCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
        headCopy.imageExtent = {width, height, 1};
        if (!(status = commands.copyBufferToImage(upload, resource.head,
                std::array<BufferImageCopyRegion, 1>{{headCopy}})))
            return fail("initialize R6 PPLL head image", status);
    }

    auto beginPass = [&](CaseResources& resource, LoadOp load,
                         PipelineHandle pipeline, BindingSetHandle bindings,
                         std::uint64_t semanticId)
    {
        RenderingInfo rendering;
        rendering.semanticId = semanticId;
        rendering.width = width;
        rendering.height = height;
        rendering.colors.push_back({resource.colorView, Format::RGBA8UNorm,
            load, StoreOp::Store, {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
        if (!(status = commands.beginRendering(rendering)) ||
            !(status = commands.bindPipeline(pipeline)) ||
            !(status = commands.bindBindingSet(0, bindings)) ||
            !(status = commands.setViewport(
                {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f})) ||
            !(status = commands.setScissor({0, 0, width, height})) ||
            !(status = commands.bindIndexBuffer(index, 0, IndexType::UInt16)))
            return false;
        return true;
    };

    for (std::size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex)
    {
        CaseResources& resource = cases[caseIndex];
        for (std::size_t layer = 0; layer < layers; ++layer)
        {
            if (!beginPass(resource, layer == 0 ? LoadOp::Clear : LoadOp::Load,
                           capturePipeline, resource.captureSet,
                           0x52365f50434c0000ull + caseIndex * 0x100ull + layer) ||
                !(status = commands.bindVertexBuffer(
                    0, vertex, layer * 4u * sizeof(Vertex))) ||
                !(status = commands.drawIndexed({6, 1, 0, 0, 0})) ||
                !(status = commands.endRendering()) ||
                !(status = commands.resourceBarrier(ResourceBarrier::StorageWriteToRead)))
                return fail("execute R6 PPLL capture pass", status);
        }
        if (!beginPass(resource, LoadOp::Load, resolvePipeline, resource.resolveSet,
                       0x52365f5052530000ull + caseIndex) ||
            !(status = commands.bindVertexBuffer(0, vertex, 0)) ||
            !(status = commands.drawIndexed({6, 1, 0, 0, 0})) ||
            !(status = commands.endRendering()))
            return fail("execute R6 PPLL resolve pass", status);

        BufferImageCopyRegion colorCopy;
        colorCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
        colorCopy.imageExtent = {width, height, 1};
        if (!(status = commands.copyImageToBuffer(resource.color, resource.colorReadback,
                std::array<BufferImageCopyRegion, 1>{{colorCopy}})) ||
            !(status = commands.copyBuffer(resource.counter, resource.counterReadback,
                std::array<BufferCopyRegion, 1>{{{0, 0, 8u}}})))
            return fail("read back R6 PPLL evidence", status);
    }
    if (!(status = commands.endFrame())) return fail("end R6 PPLL frame", status);

    auto waitRead = [&](BufferHandle buffer, std::span<std::byte> destination)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do
        {
            status = device.readBuffer(buffer, 0, destination);
            if (status || status.code() != StatusCode::NotReady) return status;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return status;
    };
    for (std::size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex)
    {
        std::vector<std::byte> pixels(colorBytes);
        if (!(status = waitRead(cases[caseIndex].colorReadback, pixels)))
            return fail("read R6 PPLL color target", status);
        if (device.backend() == Backend::OpenGL)
        {
            constexpr std::size_t rowBytes = width * 4u;
            for (std::uint32_t y = 0; y < height / 2u; ++y)
                std::swap_ranges(pixels.begin() + y * rowBytes,
                    pixels.begin() + (y + 1u) * rowBytes,
                    pixels.begin() + (height - 1u - y) * rowBytes);
        }
        result.colorSha256[caseIndex] = sha256(pixels);
        for (std::size_t pixel = 0; pixel < width * height; ++pixel)
            if (pixels[pixel * 4u + 3u] != std::byte{0})
                ++result.shadedPixelCount[caseIndex];
        if (result.shadedPixelCount[caseIndex] != coverage)
            return mismatch("R6 PPLL coverage mismatch");

        std::array<std::byte, 8> counters{};
        if (!(status = waitRead(cases[caseIndex].counterReadback, counters)))
            return fail("read R6 PPLL counters", status);
        std::memcpy(&result.allocatedNodeCount[caseIndex], counters.data(), 4u);
        std::memcpy(&result.overflowFragmentCount[caseIndex], counters.data() + 4u, 4u);
        if (result.allocatedNodeCount[caseIndex] != totalFragments ||
            result.overflowFragmentCount[caseIndex] != expectedOverflow[caseIndex])
            return mismatch("R6 PPLL allocation/overflow counter mismatch");
        if (referenceHashes[caseIndex][0] &&
            result.colorSha256[caseIndex] != referenceHashes[caseIndex])
            return mismatch("R6 PPLL color hash mismatch: " + result.colorSha256[caseIndex]);
    }

    for (CaseResources& resource : cases)
    {
        for (Status destroyStatus : {
            device.destroy(resource.captureSet), device.destroy(resource.resolveSet),
            device.destroy(resource.headView), device.destroy(resource.colorView),
            device.destroy(resource.head), device.destroy(resource.color),
            device.destroy(resource.captureUniform), device.destroy(resource.resolveUniform),
            device.destroy(resource.nodes), device.destroy(resource.counter),
            device.destroy(resource.counterReadback), device.destroy(resource.colorReadback)})
            if (!destroyStatus) return fail("destroy R6 PPLL case resource", destroyStatus);
    }
    for (Status destroyStatus : {
        device.destroy(capturePipeline), device.destroy(resolvePipeline),
        device.destroy(shader), device.destroy(upload), device.destroy(vertex),
        device.destroy(index)})
        if (!destroyStatus) return fail("destroy R6 PPLL shared resource", destroyStatus);
    if (!(status = device.waitIdle())) return fail("wait for R6 PPLL retirement", status);

    result.passed = true;
    result.message = "R6 PPLL capture/resolve fixture PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIPPLLFIXTURE_H
