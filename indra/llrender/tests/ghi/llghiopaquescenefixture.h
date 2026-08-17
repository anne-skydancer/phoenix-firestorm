/**
 * @file llghiopaquescenefixture.h
 * @brief Native-peer replay of one production post-cull opaque scene packet.
 */

#ifndef LL_LLGHIOPAQUESCENEFIXTURE_H
#define LL_LLGHIOPAQUESCENEFIXTURE_H

#include "ghi/core/llghihash.h"
#include "ghi/include/llghi.h"
#include "ghi/include/llghiopaquescenepacket.h"

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

struct OpaqueSceneFixtureResult
{
    bool passed = false;
    std::string message;
    Format depthStencilFormat = Format::Undefined;
    std::array<std::string, 4> colorSha256;
    std::array<std::uint64_t, 4> nonClearPixels{};
    std::array<std::vector<std::byte>, 4> colorPixels;
};

inline OpaqueSceneFixtureResult runOpaqueSceneFixture(
    Device& device, ShaderPackageDesc shaderPackage,
    const OpaqueScenePacket& packet)
{
    auto fail = [](const char* operation, const Status& status)
    {
        return OpaqueSceneFixtureResult{
            false, std::string(operation) + ": " + status.message()};
    };
    if (packet.vertices.empty() || packet.indices.empty() || packet.draws.empty())
        return {false, "opaque scene packet contains no comparable draws"};

    constexpr std::uint32_t width = 512;
    constexpr std::uint32_t height = 512;
    constexpr std::array<Format, 4> colorFormats{{
        Format::RGBA8UNorm, Format::RGBA8UNorm,
        Format::RGBA16UNorm, Format::RGBA16Float}};
    constexpr std::array<std::uint32_t, 4> bytesPerPixel{{4, 4, 8, 8}};
    constexpr std::array<float, 16> material{{
        1.f, 1.f, 1.f, 1.f, 0.25f, 0.5f, 0.75f, 1.f,
        0.5f, 0.5f, 1.f, 1.f, 0.125f, 0.25f, 0.5f, 1.f}};

    // A separate binding set per draw preserves the package's reviewed static
    // binding contract. Dynamic offsets remain available for production batching.
    const std::uint64_t alignment = std::max<std::uint64_t>(
        16, device.capabilities().uniformBufferOffsetAlignment);
    const auto align = [alignment](std::uint64_t value)
    {
        return (value + alignment - 1) / alignment * alignment;
    };
    const std::uint64_t vertexBytes = packet.vertices.size() * sizeof(OpaqueSceneVertex);
    const std::uint64_t indexBytes = packet.indices.size() * sizeof(std::uint32_t);
    const std::uint64_t vertexOffset = 0;
    const std::uint64_t indexOffset = align(vertexBytes);
    const std::uint64_t transformOffset = align(indexOffset + indexBytes);
    const std::uint64_t transformStride = align(sizeof(packet.draws.front().transform));
    const std::uint64_t materialOffset =
        align(transformOffset + transformStride * packet.draws.size());
    const std::uint64_t uploadBytes = materialOffset + sizeof(material);

    Status status = Status::success();
    BufferHandle upload = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status) return fail("create scene upload buffer", status);
    BufferHandle vertices = device.createBuffer(
        {vertexBytes, ResourceUsage::Vertex | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create scene vertex buffer", status);
    BufferHandle indices = device.createBuffer(
        {indexBytes, ResourceUsage::Index | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create scene index buffer", status);
    BufferHandle transforms = device.createBuffer(
        {transformStride * packet.draws.size(),
         ResourceUsage::Uniform | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create scene transform buffer", status);
    BufferHandle materialBuffer = device.createBuffer(
        {sizeof(material), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create scene material buffer", status);

    std::vector<std::byte> uploadData(static_cast<std::size_t>(uploadBytes));
    std::memcpy(uploadData.data() + vertexOffset, packet.vertices.data(), vertexBytes);
    std::memcpy(uploadData.data() + indexOffset, packet.indices.data(), indexBytes);
    for (std::size_t draw = 0; draw < packet.draws.size(); ++draw)
        std::memcpy(uploadData.data() + transformOffset + transformStride * draw,
                    packet.draws[draw].transform.data(),
                    sizeof(packet.draws[draw].transform));
    std::memcpy(uploadData.data() + materialOffset, material.data(), sizeof(material));
    if (!(status = device.writeBuffer(upload, 0, uploadData)))
        return fail("write scene upload buffer", status);

    std::array<ImageHandle, 4> colors;
    std::array<ImageViewHandle, 4> colorViews;
    std::array<BufferHandle, 4> readbacks;
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        colors[target] = device.createImage(
            {{width, height, 1}, colorFormats[target],
             ResourceUsage::ColorAttachment | ResourceUsage::TransferSource,
             1, 1, 1}, status);
        if (!status) return fail("create scene color target", status);
        colorViews[target] = device.createImageView(
            {colors[target], colorFormats[target],
             {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (!status) return fail("create scene color view", status);
        readbacks[target] = device.createBuffer(
            {static_cast<std::uint64_t>(width) * height * bytesPerPixel[target],
             ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        if (!status) return fail("create scene readback", status);
    }
    const Format depthFormat = device.capabilities().preferredDepthStencilFormat;
    ImageHandle depth = device.createImage(
        {{width, height, 1}, depthFormat, ResourceUsage::DepthStencilAttachment,
         1, 1, 1}, status);
    if (!status) return fail("create scene depth target", status);
    ImageViewHandle depthView = device.createImageView(
        {depth, depthFormat, {ImageAspect::DepthStencil, 0, 1, 0, 1}}, status);
    if (!status) return fail("create scene depth view", status);

    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    if (!status) return fail("create scene shader", status);
    std::vector<BindingSetHandle> frameSets;
    frameSets.reserve(packet.draws.size());
    for (std::size_t draw = 0; draw < packet.draws.size(); ++draw)
    {
        BindingSetDesc desc;
        desc.shader = shader;
        desc.group = 0;
        desc.resources.push_back(
            {0, 0, ShaderPackageDesc::BindingType::UniformBuffer, transforms,
             transformStride * draw, sizeof(packet.draws[draw].transform), {}, {}});
        frameSets.push_back(device.createBindingSet(desc, status));
        if (!status) return fail("create scene frame binding", status);
    }
    BindingSetDesc materialDesc;
    materialDesc.shader = shader;
    materialDesc.group = 2;
    materialDesc.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         materialBuffer, 0, sizeof(material), {}, {}});
    BindingSetHandle materialSet = device.createBindingSet(materialDesc, status);
    if (!status) return fail("create scene material binding", status);

    PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader;
    pipelineDesc.cullMode = CullMode::Back;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompare = CompareOp::GreaterEqual;
    pipelineDesc.colorFormats.assign(colorFormats.begin(), colorFormats.end());
    pipelineDesc.depthStencilFormat = depthFormat;
    pipelineDesc.blendStates.assign(4, BlendState{});
    pipelineDesc.blendStates[1].colorWriteMask = 0x07;
    pipelineDesc.blendStates[2].colorWriteMask = 0x0b;
    pipelineDesc.blendStates[3].colorWriteMask = 0x0d;
    pipelineDesc.vertexBuffers = {{0, sizeof(OpaqueSceneVertex), VertexInputRate::PerVertex}};
    pipelineDesc.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0}, {1, 0, VertexFormat::UNorm8x4, 12}};
    PipelineHandle pipeline = device.createPipeline(pipelineDesc, status);
    if (!status) return fail("create scene pipeline", status);

    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame())) return fail("begin scene frame", status);
    const std::array<BufferCopyRegion, 1> vertexCopy{{{vertexOffset, 0, vertexBytes}}};
    const std::array<BufferCopyRegion, 1> indexCopy{{{indexOffset, 0, indexBytes}}};
    const std::array<BufferCopyRegion, 1> transformCopy{{
        {transformOffset, 0, transformStride * packet.draws.size()}}};
    const std::array<BufferCopyRegion, 1> materialCopy{{
        {materialOffset, 0, sizeof(material)}}};
    if (!(status = commands.copyBuffer(upload, vertices, vertexCopy)) ||
        !(status = commands.copyBuffer(upload, indices, indexCopy)) ||
        !(status = commands.copyBuffer(upload, transforms, transformCopy)) ||
        !(status = commands.copyBuffer(upload, materialBuffer, materialCopy)))
        return fail("upload scene resources", status);

    RenderingInfo rendering;
    rendering.semanticId = 0x5234645f4c495645ull; // "R4d_LIVE"
    rendering.width = width;
    rendering.height = height;
    for (std::size_t target = 0; target < colors.size(); ++target)
        rendering.colors.push_back(
            {colorViews[target], colorFormats[target], LoadOp::Clear, StoreOp::Store,
             {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
    rendering.depthStencil = AttachmentDesc{
        depthView, depthFormat, LoadOp::Clear, StoreOp::Store,
        {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};
    if (!(status = commands.beginRendering(rendering)))
        return fail("begin scene rendering", status);
    if (!(status = commands.bindPipeline(pipeline)) ||
        !(status = commands.bindBindingSet(2, materialSet)) ||
        !(status = commands.setViewport(
            {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f})) ||
        !(status = commands.setScissor({0, 0, width, height})) ||
        !(status = commands.bindVertexBuffer(0, vertices, 0)) ||
        !(status = commands.bindIndexBuffer(indices, 0, IndexType::UInt32)))
        return fail("bind scene rendering state", status);
    for (std::size_t draw = 0; draw < packet.draws.size(); ++draw)
    {
        if (!(status = commands.bindBindingSet(0, frameSets[draw])) ||
            !(status = commands.drawIndexed(
                {packet.draws[draw].indexCount, 1, packet.draws[draw].firstIndex, 0, 0})))
            return fail("draw scene packet", status);
    }
    if (!(status = commands.endRendering())) return fail("end scene rendering", status);
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        BufferImageCopyRegion region;
        region.imageSubresource = {ImageAspect::Color, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        const std::array<BufferImageCopyRegion, 1> copies{{region}};
        if (!(status = commands.copyImageToBuffer(colors[target], readbacks[target], copies)))
            return fail("copy scene target", status);
    }
    if (!(status = commands.endFrame())) return fail("end scene frame", status);

    OpaqueSceneFixtureResult result;
    result.depthStencilFormat = depthFormat;
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        std::vector<std::byte> pixels(
            static_cast<std::size_t>(width) * height * bytesPerPixel[target]);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do
        {
            status = device.readBuffer(readbacks[target], 0, pixels);
            if (status || status.code() != StatusCode::NotReady) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!status) return fail("read scene target", status);
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
        for (std::size_t pixel = 0; pixel < width * height; ++pixel)
        {
            const auto begin = pixels.begin() +
                static_cast<std::ptrdiff_t>(pixel * bytesPerPixel[target]);
            if (std::any_of(begin, begin + bytesPerPixel[target],
                            [](std::byte value) { return value != std::byte{0}; }))
                ++result.nonClearPixels[target];
        }
        result.colorPixels[target] = std::move(pixels);
    }

    for (BindingSetHandle set : frameSets)
        if (!(status = device.destroy(set))) return fail("destroy scene frame binding", status);
    for (Status destroyStatus : {
        device.destroy(pipeline), device.destroy(materialSet), device.destroy(shader),
        device.destroy(depthView), device.destroy(depth), device.destroy(upload),
        device.destroy(vertices), device.destroy(indices), device.destroy(transforms),
        device.destroy(materialBuffer)})
        if (!destroyStatus) return fail("destroy scene resource", destroyStatus);
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        if (!(status = device.destroy(colorViews[target])) ||
            !(status = device.destroy(colors[target])) ||
            !(status = device.destroy(readbacks[target])))
            return fail("destroy scene target", status);
    }
    if (!(status = device.waitIdle())) return fail("wait for scene retirement", status);
    result.passed = true;
    result.message = "R4d live opaque scene replay PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIOPAQUESCENEFIXTURE_H
