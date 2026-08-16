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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace LL::GHI::Test
{

struct DrawFixtureResult
{
    bool passed = false;
    std::string message;
    Format depthStencilFormat = Format::Undefined;
};

inline DrawFixtureResult runDrawFixture(
    Device& device,
    const ShaderPackageDesc& shaderPackage)
{
    auto fail = [](const char* operation, const Status& status)
    {
        return DrawFixtureResult{false,
            std::string(operation) + ": " + status.message(), Format::Undefined};
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
        {{64, 64, 1}, Format::RGBA8UNorm,
         ResourceUsage::ColorAttachment | ResourceUsage::TransferSource, 1, 1, 1}, status);
    if (!status) return fail("create color target", status);
    ImageViewHandle colorView = device.createImageView(
        {color, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    if (!status) return fail("create color target view", status);

    const Format depthFormat = device.capabilities().preferredDepthStencilFormat;
    if (depthFormat == Format::Undefined)
        return {false, "device did not select a depth/stencil format", depthFormat};
    ImageHandle depth = device.createImage(
        {{64, 64, 1}, depthFormat, ResourceUsage::DepthStencilAttachment, 1, 1, 1}, status);
    if (!status) return fail("create depth/stencil target", status);
    ImageViewHandle depthView = device.createImageView(
        {depth, depthFormat, {ImageAspect::DepthStencil, 0, 1, 0, 1}}, status);
    if (!status) return fail("create depth/stencil target view", status);

    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
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
    pipelineDesc.cullMode = CullMode::None;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompare = CompareOp::GreaterEqual;
    pipelineDesc.colorFormats = {Format::RGBA8UNorm};
    pipelineDesc.depthStencilFormat = depthFormat;
    pipelineDesc.blendStates = {BlendState{}};
    pipelineDesc.vertexBuffers = {{0, sizeof(Vertex), VertexInputRate::PerVertex}};
    pipelineDesc.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0},
        {1, 0, VertexFormat::Float32x2, 12},
    };
    PipelineHandle pipeline = device.createPipeline(pipelineDesc, status);
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
    rendering.width = 64;
    rendering.height = 64;
    rendering.colors.push_back(
        {colorView, Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store,
         {{0.02f, 0.03f, 0.05f, 1.f}, 0.f, 0}});
    rendering.depthStencil = AttachmentDesc{
        depthView, depthFormat, LoadOp::Clear, StoreOp::Store,
        {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};
    if (!(status = commands.beginRendering(rendering))) return fail("begin rendering", status);
    if (!(status = commands.bindPipeline(pipeline))) return fail("bind pipeline", status);
    if (!(status = commands.bindBindingSet(0, frameSet))) return fail("bind frame resources", status);
    if (!(status = commands.bindBindingSet(2, materialSet))) return fail("bind material resources", status);
    if (!(status = commands.setViewport({0.f, 0.f, 64.f, 64.f, 0.f, 1.f})))
        return fail("set viewport", status);
    if (!(status = commands.setScissor({0, 0, 64, 64}))) return fail("set scissor", status);
    if (!(status = commands.bindVertexBuffer(0, vertex, 0))) return fail("bind vertex buffer", status);
    if (!(status = commands.bindIndexBuffer(index, 0, IndexType::UInt16)))
        return fail("bind index buffer", status);
    if (!(status = commands.drawIndexed({6, 1, 0, 0, 0}))) return fail("draw indexed", status);
    if (!(status = commands.endRendering())) return fail("end rendering", status);
    if (!(status = commands.endFrame())) return fail("end draw frame", status);

    for (Status destroyStatus : {
        device.destroy(pipeline), device.destroy(frameSet), device.destroy(materialSet),
        device.destroy(shader), device.destroy(textureView), device.destroy(sampler),
        device.destroy(colorView), device.destroy(depthView), device.destroy(texture),
        device.destroy(color), device.destroy(depth), device.destroy(upload),
        device.destroy(vertex), device.destroy(index), device.destroy(uniform)})
    {
        if (!destroyStatus) return fail("destroy draw fixture resource", destroyStatus);
    }
    if (!(status = device.waitIdle())) return fail("wait for draw fixture retirement", status);
    return {true, "R3 indexed draw fixture PASS", depthFormat};
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIDRAWFIXTURE_H
