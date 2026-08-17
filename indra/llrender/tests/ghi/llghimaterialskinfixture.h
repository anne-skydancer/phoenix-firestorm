/**
 * @file llghimaterialskinfixture.h
 * @brief R5a textured PBR-material and explicit-skinning peer fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIMATERIALSKINFIXTURE_H
#define LL_LLGHIMATERIALSKINFIXTURE_H

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

struct MaterialSkinFixtureResult
{
    bool passed = false;
    std::string message;
    Format depthStencilFormat = Format::Undefined;
    std::array<std::string, 4> colorSha256;
    std::array<std::uint32_t, 4> shadedPixelCount{};
};

inline MaterialSkinFixtureResult runMaterialSkinFixture(
    Device& device, const ShaderPackageDesc& shaderPackage)
{
    auto fail = [](const char* operation, const Status& status)
    {
        return MaterialSkinFixtureResult{
            false, std::string(operation) + ": " + status.message()};
    };

    struct Vertex
    {
        float position[3];
        float normal[3];
        float tangent[4];
        float texCoord[2];
        std::uint8_t color[4];
        std::uint16_t joints[4];
        float weights[4];
    };
    static_assert(sizeof(Vertex) == 76);

    constexpr std::array<Vertex, 4> vertices{{
        {{-0.75f, -0.75f, 0.75f}, {.4472136f, 0.f, .8944272f},
         {.8944272f, 0.f, -.4472136f, 1.f},
         {-0.25f, -0.25f}, {255, 224, 192, 255}, {0, 1, 2, 3}, {.5f, .5f, 0.f, 0.f}},
        {{ 0.75f, -0.75f, 0.75f}, {.4472136f, 0.f, .8944272f},
         {.8944272f, 0.f, -.4472136f, 1.f},
         { 1.25f, -0.25f}, {224, 255, 192, 255}, {0, 1, 2, 3}, {.5f, .5f, 0.f, 0.f}},
        {{ 0.75f,  0.75f, 0.75f}, {.4472136f, 0.f, .8944272f},
         {.8944272f, 0.f, -.4472136f, 1.f},
         { 1.25f,  1.25f}, {192, 224, 255, 255}, {0, 1, 2, 3}, {.5f, .5f, 0.f, 0.f}},
        {{-0.75f,  0.75f, 0.75f}, {.4472136f, 0.f, .8944272f},
         {.8944272f, 0.f, -.4472136f, 1.f},
         {-0.25f,  1.25f}, {255, 255, 255, 255}, {0, 1, 2, 3}, {.5f, .5f, 0.f, 0.f}},
    }};
    constexpr std::array<std::uint16_t, 6> indices{{0, 1, 2, 2, 3, 0}};
    constexpr std::array<float, 16> frame{{
        1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}};
    constexpr std::array<float, 32> object{{
        .75f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, .5f, 0.f, .125f, -.0625f, 0.f, 1.f,
        1.333333333f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 2.f, 0.f, 0.f, 0.f, 0.f, 1.f}};
    constexpr std::array<float, 64> skin{{
        1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f, -.25f, 0.f, 0.f, 1.f,
        1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,  .25f, 0.f, 0.f, 1.f,
        1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f,
        1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}};
    constexpr std::array<float, 44> material{{
        .75f, .875f, 1.f, 1.f,
        .5f, .75f, 1.f, .625f,
        .8f, .75f, 0.f, 0.f,
        .125f, -.125f, .5f, .75f,
        -.25f, .125f, 1.25f, .5f,
        .25f, .25f, .75f, 1.25f,
        .125f, .25f, 1.f, .5f,
        0.f, 1.f, 0.f, 0.f,
        1.f, 0.f, 0.f, 0.f,
        0.f, -1.f, 0.f, 0.f,
        .70710678f, .70710678f, 0.f, 0.f}};
    constexpr std::array<std::array<std::uint8_t, 16>, 4> texels{{
        {{64, 128, 255, 255, 255, 64, 128, 255,
          128, 255, 64, 255, 224, 192, 96, 255}},
        {{128, 128, 255, 255, 160, 128, 248, 255,
          128, 160, 248, 255, 96, 128, 248, 255}},
        {{255, 64, 32, 255, 192, 128, 64, 255,
          128, 192, 96, 255, 64, 255, 128, 255}},
        {{32, 64, 128, 255, 128, 32, 64, 255,
          64, 128, 32, 255, 192, 96, 48, 255}},
    }};

    constexpr std::uint64_t vertexOffset = 0;
    constexpr std::uint64_t indexOffset = vertexOffset + sizeof(vertices);
    constexpr std::uint64_t frameOffset = indexOffset + sizeof(indices);
    constexpr std::uint64_t objectOffset = frameOffset + sizeof(frame);
    constexpr std::uint64_t skinOffset = objectOffset + sizeof(object);
    constexpr std::uint64_t materialOffset = skinOffset + sizeof(skin);
    constexpr std::uint64_t textureOffset = materialOffset + sizeof(material);
    constexpr std::uint64_t uploadBytes = textureOffset + sizeof(texels);
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr std::array<Format, 4> textureFormats{{
        Format::RGBA8SRGB, Format::RGBA8UNorm,
        Format::RGBA8UNorm, Format::RGBA8SRGB}};
    constexpr std::array<Format, 4> colorFormats{{
        Format::RGBA8UNorm, Format::RGBA8UNorm,
        Format::RGBA16UNorm, Format::RGBA16Float}};
    constexpr std::array<std::uint32_t, 4> colorBytes{{4, 4, 8, 8}};
    constexpr std::array<const char*, 4> referenceHashes{{
        "6009f38eca5792f3affb6f08896ff588cd03ab2639dd39197efe5fccd49262ef",
        "d7c4959f70b881adb21212fb23f078d44d5404d741c101bdb8c7c85e62af63f1",
        "51faf358a0eb7d9f37041f8926cded6a9fdfe26a7c6b21117c4a8b99aee3cf0b",
        "663b7e11df673d77c194801b17c8395b6bf09982f03eac1382926c9407457ab0",
    }};
    constexpr std::array<std::uint32_t, 4> referenceCoverage{{
        1728, 1728, 1728, 1728}};

    Status status = Status::success();
    BufferHandle upload = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status) return fail("create R5a upload buffer", status);
    BufferHandle vertex = device.createBuffer(
        {sizeof(vertices), ResourceUsage::Vertex | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R5a vertex buffer", status);
    BufferHandle index = device.createBuffer(
        {sizeof(indices), ResourceUsage::Index | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R5a index buffer", status);
    BufferHandle frameBuffer = device.createBuffer(
        {sizeof(frame), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R5a frame buffer", status);
    BufferHandle objectBuffer = device.createBuffer(
        {sizeof(object), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create I4 object buffer", status);
    BufferHandle skinBuffer = device.createBuffer(
        {sizeof(skin), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R5a skin buffer", status);
    BufferHandle materialBuffer = device.createBuffer(
        {sizeof(material), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R5a material buffer", status);

    std::vector<std::byte> uploadData(uploadBytes);
    std::memcpy(uploadData.data() + vertexOffset, vertices.data(), sizeof(vertices));
    std::memcpy(uploadData.data() + indexOffset, indices.data(), sizeof(indices));
    std::memcpy(uploadData.data() + frameOffset, frame.data(), sizeof(frame));
    std::memcpy(uploadData.data() + objectOffset, object.data(), sizeof(object));
    std::memcpy(uploadData.data() + skinOffset, skin.data(), sizeof(skin));
    std::memcpy(uploadData.data() + materialOffset, material.data(), sizeof(material));
    std::memcpy(uploadData.data() + textureOffset, texels.data(), sizeof(texels));
    if (!(status = device.writeBuffer(upload, 0, uploadData)))
        return fail("write R5a upload buffer", status);

    std::array<ImageHandle, 4> textures;
    std::array<ImageViewHandle, 4> textureViews;
    for (std::size_t texture = 0; texture < textures.size(); ++texture)
    {
        textures[texture] = device.createImage(
            {{2, 2, 1}, textureFormats[texture],
             ResourceUsage::Sampled | ResourceUsage::TransferDestination, 1, 1, 1}, status);
        if (!status) return fail("create R5a sampled image", status);
        textureViews[texture] = device.createImageView(
            {textures[texture], textureFormats[texture],
             {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (!status) return fail("create R5a sampled image view", status);
    }
    SamplerDesc clampDesc;
    clampDesc.minFilter = clampDesc.magFilter = clampDesc.mipFilter = Filter::Nearest;
    clampDesc.addressU = clampDesc.addressV = AddressMode::ClampToEdge;
    SamplerHandle clampSampler = device.createSampler(clampDesc, status);
    if (!status) return fail("create R5a clamp sampler", status);
    SamplerDesc repeatDesc = clampDesc;
    repeatDesc.addressU = repeatDesc.addressV = AddressMode::Repeat;
    SamplerHandle repeatSampler = device.createSampler(repeatDesc, status);
    if (!status) return fail("create R5a repeat sampler", status);

    std::array<ImageHandle, 4> colors;
    std::array<ImageViewHandle, 4> colorViews;
    std::array<BufferHandle, 4> readbacks;
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        colors[target] = device.createImage(
            {{width, height, 1}, colorFormats[target],
             ResourceUsage::ColorAttachment | ResourceUsage::TransferSource, 1, 1, 1}, status);
        if (!status) return fail("create R5a color target", status);
        colorViews[target] = device.createImageView(
            {colors[target], colorFormats[target], {ImageAspect::Color, 0, 1, 0, 1}}, status);
        if (!status) return fail("create R5a color view", status);
        readbacks[target] = device.createBuffer(
            {static_cast<std::uint64_t>(width) * height * colorBytes[target],
             ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
        if (!status) return fail("create R5a readback", status);
    }
    const Format depthFormat = device.capabilities().preferredDepthStencilFormat;
    ImageHandle depth = device.createImage(
        {{width, height, 1}, depthFormat, ResourceUsage::DepthStencilAttachment,
         1, 1, 1}, status);
    if (!status) return fail("create R5a depth target", status);
    ImageViewHandle depthView = device.createImageView(
        {depth, depthFormat, {ImageAspect::DepthStencil, 0, 1, 0, 1}}, status);
    if (!status) return fail("create R5a depth view", status);

    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    if (!status) return fail("create R5a shader", status);
    BindingSetDesc frameDesc{shader, 0, {{
        0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
        frameBuffer, 0, sizeof(frame), {}, {}}}};
    BindingSetHandle frameSet = device.createBindingSet(frameDesc, status);
    if (!status) return fail("create R5a frame bindings", status);
    BindingSetDesc skinDesc{shader, 1, {
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         objectBuffer, 0, sizeof(object), {}, {}},
        {1, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         skinBuffer, 0, sizeof(skin), {}, {}}}};
    BindingSetHandle skinSet = device.createBindingSet(skinDesc, status);
    if (!status) return fail("create R5a skin bindings", status);
    BindingSetDesc materialDesc;
    materialDesc.shader = shader;
    materialDesc.group = 2;
    materialDesc.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         materialBuffer, 0, sizeof(material), {}, {}});
    for (std::uint16_t binding = 1; binding <= 4; ++binding)
    {
        materialDesc.resources.push_back(
            {binding, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
             {}, 0, 0, textureViews[binding - 1],
             binding == 2 || binding == 3 ? repeatSampler : clampSampler});
    }
    BindingSetHandle materialSet = device.createBindingSet(materialDesc, status);
    if (!status) return fail("create R5a material bindings", status);

    PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader;
    pipelineDesc.cullMode = CullMode::Back;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompare = CompareOp::GreaterEqual;
    pipelineDesc.colorFormats.assign(colorFormats.begin(), colorFormats.end());
    pipelineDesc.depthStencilFormat = depthFormat;
    pipelineDesc.blendStates.assign(4, BlendState{});
    pipelineDesc.vertexBuffers = {{0, sizeof(Vertex), VertexInputRate::PerVertex}};
    pipelineDesc.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, offsetof(Vertex, position)},
        {1, 0, VertexFormat::Float32x3, offsetof(Vertex, normal)},
        {2, 0, VertexFormat::Float32x4, offsetof(Vertex, tangent)},
        {3, 0, VertexFormat::Float32x2, offsetof(Vertex, texCoord)},
        {4, 0, VertexFormat::UNorm8x4, offsetof(Vertex, color)},
        {5, 0, VertexFormat::UInt16x4, offsetof(Vertex, joints)},
        {6, 0, VertexFormat::Float32x4, offsetof(Vertex, weights)},
    };
    PipelineHandle pipeline = device.createPipeline(pipelineDesc, status);
    if (!status) return fail("create R5a pipeline", status);

    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame())) return fail("begin R5a frame", status);
    const std::array<BufferCopyRegion, 1> vertexCopy{{{vertexOffset, 0, sizeof(vertices)}}};
    const std::array<BufferCopyRegion, 1> indexCopy{{{indexOffset, 0, sizeof(indices)}}};
    const std::array<BufferCopyRegion, 1> frameCopy{{{frameOffset, 0, sizeof(frame)}}};
    const std::array<BufferCopyRegion, 1> objectCopy{{{objectOffset, 0, sizeof(object)}}};
    const std::array<BufferCopyRegion, 1> skinCopy{{{skinOffset, 0, sizeof(skin)}}};
    const std::array<BufferCopyRegion, 1> materialCopy{{{materialOffset, 0, sizeof(material)}}};
    if (!(status = commands.copyBuffer(upload, vertex, vertexCopy)) ||
        !(status = commands.copyBuffer(upload, index, indexCopy)) ||
        !(status = commands.copyBuffer(upload, frameBuffer, frameCopy)) ||
        !(status = commands.copyBuffer(upload, objectBuffer, objectCopy)) ||
        !(status = commands.copyBuffer(upload, skinBuffer, skinCopy)) ||
        !(status = commands.copyBuffer(upload, materialBuffer, materialCopy)))
        return fail("upload R5a buffers", status);
    for (std::size_t texture = 0; texture < textures.size(); ++texture)
    {
        BufferImageCopyRegion copy;
        copy.bufferOffset = textureOffset + texture * sizeof(texels.front());
        copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
        copy.imageExtent = {2, 2, 1};
        const std::array<BufferImageCopyRegion, 1> copies{{copy}};
        if (!(status = commands.copyBufferToImage(upload, textures[texture], copies)))
            return fail("upload R5a texture", status);
    }

    RenderingInfo rendering;
    rendering.semanticId = 0x5235615f4d41544cull; // "R5a_MATL"
    rendering.width = width;
    rendering.height = height;
    for (std::size_t target = 0; target < colors.size(); ++target)
        rendering.colors.push_back(
            {colorViews[target], colorFormats[target], LoadOp::Clear, StoreOp::Store, {}});
    rendering.depthStencil = AttachmentDesc{
        depthView, depthFormat, LoadOp::Clear, StoreOp::Store,
        {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}};
    if (!(status = commands.beginRendering(rendering)) ||
        !(status = commands.bindPipeline(pipeline)) ||
        !(status = commands.bindBindingSet(0, frameSet)) ||
        !(status = commands.bindBindingSet(1, skinSet)) ||
        !(status = commands.bindBindingSet(2, materialSet)) ||
        !(status = commands.setViewport({0.f, 0.f, static_cast<float>(width),
                                         static_cast<float>(height), 0.f, 1.f})) ||
        !(status = commands.setScissor({0, 0, width, height})) ||
        !(status = commands.bindVertexBuffer(0, vertex, 0)) ||
        !(status = commands.bindIndexBuffer(index, 0, IndexType::UInt16)) ||
        !(status = commands.drawIndexed({6, 1, 0, 0, 0})) ||
        !(status = commands.endRendering()))
        return fail("execute R5a material draw", status);
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        BufferImageCopyRegion copy;
        copy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        const std::array<BufferImageCopyRegion, 1> copies{{copy}};
        if (!(status = commands.copyImageToBuffer(colors[target], readbacks[target], copies)))
            return fail("copy R5a target", status);
    }
    if (!(status = commands.endFrame())) return fail("end R5a frame", status);

    MaterialSkinFixtureResult result;
    result.depthStencilFormat = depthFormat;
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        std::vector<std::byte> pixels(
            static_cast<std::size_t>(width) * height * colorBytes[target]);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do
        {
            status = device.readBuffer(readbacks[target], 0, pixels);
            if (status || status.code() != StatusCode::NotReady) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!status) return fail("read R5a target", status);
        if (device.backend() == Backend::OpenGL)
        {
            const std::size_t rowBytes = width * colorBytes[target];
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
            const auto first = pixels.begin() +
                static_cast<std::ptrdiff_t>(pixel * colorBytes[target]);
            if (std::any_of(first, first + colorBytes[target],
                            [](std::byte value) { return value != std::byte{0}; }))
                ++result.shadedPixelCount[target];
        }
        if (result.shadedPixelCount[target] != referenceCoverage[target])
            return {false, "R5a target coverage mismatch", depthFormat,
                    result.colorSha256, result.shadedPixelCount};
        if (result.colorSha256[target] != referenceHashes[target])
            return {false, "R5a target hash mismatch: " + result.colorSha256[target],
                    depthFormat, result.colorSha256, result.shadedPixelCount};
    }

    for (Status destroyStatus : {
        device.destroy(pipeline), device.destroy(frameSet), device.destroy(skinSet),
        device.destroy(materialSet), device.destroy(shader), device.destroy(depthView),
        device.destroy(depth), device.destroy(clampSampler), device.destroy(repeatSampler),
        device.destroy(upload), device.destroy(vertex), device.destroy(index),
        device.destroy(frameBuffer), device.destroy(objectBuffer),
        device.destroy(skinBuffer), device.destroy(materialBuffer)})
        if (!destroyStatus) return fail("destroy R5a resource", destroyStatus);
    for (std::size_t target = 0; target < colors.size(); ++target)
    {
        if (!(status = device.destroy(colorViews[target])) ||
            !(status = device.destroy(colors[target])) ||
            !(status = device.destroy(readbacks[target])) ||
            !(status = device.destroy(textureViews[target])) ||
            !(status = device.destroy(textures[target])))
            return fail("destroy R5a image resource", status);
    }
    if (!(status = device.waitIdle())) return fail("wait for R5a retirement", status);
    result.passed = true;
    result.message = "I4 material transform fixture PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHIMATERIALSKINFIXTURE_H
