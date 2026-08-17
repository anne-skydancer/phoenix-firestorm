/**
 * @file llghilegacyalphafixture.h
 * @brief Backend-independent R6 legacy alpha/mask/residual fixture.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHILEGACYALPHAFIXTURE_H
#define LL_LLGHILEGACYALPHAFIXTURE_H

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

struct LegacyAlphaFixtureResult
{
    bool passed = false;
    std::string message;
    std::string colorSha256;
    std::uint32_t shadedPixelCount = 0;
};

inline LegacyAlphaFixtureResult runLegacyAlphaFixture(
    Device& device, const ShaderPackageDesc& shaderPackage)
{
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    constexpr const char* referenceHash =
        "44565a0a457a61a7c15ea8fdc947be87cf4db91e0d49213c1d7a194a280a2929";
    constexpr std::uint32_t referenceCoverage = 2836;
    struct Vertex { float position[3]; float color[4]; };
    std::array<Vertex, 16> vertices{};
    const auto quad = [&](std::size_t index, float left, float bottom,
                          float right, float top,
                          const std::array<std::array<float, 4>, 4>& colors)
    {
        const std::array<std::array<float, 2>, 4> points{{
            {{left, bottom}}, {{right, bottom}}, {{right, top}}, {{left, top}}}};
        for (std::size_t corner = 0; corner < 4; ++corner)
        {
            Vertex& vertex = vertices[index * 4 + corner];
            vertex.position[0] = points[corner][0];
            vertex.position[1] = points[corner][1];
            vertex.position[2] = 0.f;
            std::copy(colors[corner].begin(), colors[corner].end(), vertex.color);
        }
    };
    // The source-over card extends behind the mask card so rejected mask
    // fragments must preserve prior color instead of writing transparent black.
    quad(0, -0.90f, -0.30f, 0.90f, 0.80f,
         {{{{0.95f, 0.25f, 0.08f, 0.45f}}, {{0.95f, 0.25f, 0.08f, 0.45f}},
            {{0.95f, 0.25f, 0.08f, 0.45f}}, {{0.95f, 0.25f, 0.08f, 0.45f}}}});
    quad(1, 0.0f, -0.30f, 0.90f, 0.80f,
         {{{{0.15f, 0.80f, 0.25f, 0.20f}}, {{0.15f, 0.80f, 0.25f, 0.20f}},
            {{0.15f, 0.80f, 0.25f, 0.80f}}, {{0.15f, 0.80f, 0.25f, 0.80f}}}});
    quad(2, -0.30f, -0.10f, 0.30f, 0.50f,
         {{{{0.10f, 0.70f, 0.95f, 0.25f}}, {{0.10f, 0.70f, 0.95f, 0.25f}},
            {{0.10f, 0.70f, 0.95f, 0.25f}}, {{0.10f, 0.70f, 0.95f, 0.25f}}}});
    quad(3, -0.70f, -0.85f, 0.70f, -0.25f,
         {{{{0.75f, 0.30f, 0.90f, 0.50f}}, {{0.75f, 0.30f, 0.90f, 0.50f}},
            {{0.75f, 0.30f, 0.90f, 0.50f}}, {{0.75f, 0.30f, 0.90f, 0.50f}}}});
    constexpr std::array<std::uint16_t, 6> indices{{0, 1, 2, 2, 3, 0}};
    struct alignas(16) LegacyUniform
    {
        std::uint32_t config[4];
        float params[4];
    };
    static_assert(sizeof(LegacyUniform) == 32);
    constexpr std::array<LegacyUniform, 4> uniforms{{
        LegacyUniform{{0u, 0u, 0u, 0u}, {0.5f, 0.30f, 0.f, 0.f}},
        LegacyUniform{{1u, 0u, 0u, 0u}, {0.5f, 0.30f, 0.f, 0.f}},
        LegacyUniform{{2u, 0u, 0u, 0u}, {0.5f, 0.30f, 0.f, 0.f}},
        LegacyUniform{{3u, 0u, 0u, 0u}, {0.5f, 0.30f, 0.f, 0.f}},
    }};
    constexpr std::uint64_t vertexOffset = 0;
    constexpr std::uint64_t indexOffset = sizeof(vertices);
    constexpr std::uint64_t uniformOffset = indexOffset + sizeof(indices);
    constexpr std::uint64_t uploadBytes = uniformOffset + sizeof(uniforms);
    constexpr std::uint64_t colorBytes = width * height * 4u;

    LegacyAlphaFixtureResult result;
    auto fail = [&](const char* operation, const Status& status)
    {
        result.message = std::string(operation) + ": " + status.message();
        return result;
    };
    Status status = Status::success();
    std::vector<std::byte> uploadData(uploadBytes);
    std::memcpy(uploadData.data() + vertexOffset, vertices.data(), sizeof(vertices));
    std::memcpy(uploadData.data() + indexOffset, indices.data(), sizeof(indices));
    std::memcpy(uploadData.data() + uniformOffset, uniforms.data(), sizeof(uniforms));
    BufferHandle upload = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (!status || !(status = device.writeBuffer(upload, 0, uploadData)))
        return fail("prepare R6 legacy-alpha upload", status);
    BufferHandle vertex = device.createBuffer(
        {sizeof(vertices), ResourceUsage::Vertex | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    BufferHandle index = device.createBuffer(
        {sizeof(indices), ResourceUsage::Index | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    std::array<BufferHandle, 4> uniformBuffers{};
    for (BufferHandle& uniform : uniformBuffers)
        uniform = device.createBuffer(
            {sizeof(LegacyUniform), ResourceUsage::Uniform | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
    if (!status) return fail("create R6 legacy-alpha buffers", status);
    ImageHandle color = device.createImage(
        {{width, height, 1}, Format::RGBA8UNorm,
         ResourceUsage::ColorAttachment | ResourceUsage::TransferSource, 1, 1, 1}, status);
    ImageViewHandle colorView = device.createImageView(
        {color, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    BufferHandle readback = device.createBuffer(
        {colorBytes, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
    ShaderPackageHandle shader = device.createShaderPackage(shaderPackage, status);
    if (!status) return fail("create R6 legacy-alpha resources", status);
    std::array<BindingSetHandle, 4> bindingSets{};
    for (std::size_t mode = 0; mode < bindingSets.size(); ++mode)
    {
        BindingSetDesc bindings;
        bindings.shader = shader;
        bindings.group = 0;
        bindings.resources = {{0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
            uniformBuffers[mode], 0, sizeof(LegacyUniform), {}, {}}};
        bindingSets[mode] = device.createBindingSet(bindings, status);
        if (!status) return fail("create R6 legacy-alpha bindings", status);
    }

    auto pipeline = [&](BlendState blend)
    {
        PipelineDesc desc;
        desc.shader = shader;
        desc.cullMode = CullMode::None;
        desc.depthTest = false;
        desc.depthWrite = false;
        desc.colorFormats = {Format::RGBA8UNorm};
        desc.blendStates = {blend};
        desc.vertexBuffers = {{0, sizeof(Vertex), VertexInputRate::PerVertex}};
        desc.vertexAttributes = {
            {0, 0, VertexFormat::Float32x3, offsetof(Vertex, position)},
            {1, 0, VertexFormat::Float32x4, offsetof(Vertex, color)}};
        return device.createPipeline(desc, status);
    };
    BlendState sourceOver;
    sourceOver.enabled = true;
    sourceOver.sourceColor = BlendFactor::SourceAlpha;
    sourceOver.destinationColor = BlendFactor::OneMinusSourceAlpha;
    sourceOver.sourceAlpha = BlendFactor::One;
    sourceOver.destinationAlpha = BlendFactor::OneMinusSourceAlpha;
    PipelineHandle sourceOverPipeline = pipeline(sourceOver);
    PipelineHandle maskPipeline = pipeline({});
    BlendState additive;
    additive.enabled = true;
    additive.sourceColor = additive.sourceAlpha = BlendFactor::One;
    additive.destinationColor = additive.destinationAlpha = BlendFactor::One;
    PipelineHandle particlePipeline = pipeline(additive);
    BlendState emissive = additive;
    emissive.sourceAlpha = BlendFactor::Zero;
    emissive.destinationAlpha = BlendFactor::One;
    PipelineHandle emissivePipeline = pipeline(emissive);
    if (!status) return fail("create R6 legacy-alpha pipelines", status);

    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame()) ||
        !(status = commands.copyBuffer(upload, vertex,
            std::array<BufferCopyRegion, 1>{{{vertexOffset, 0, sizeof(vertices)}}})) ||
        !(status = commands.copyBuffer(upload, index,
            std::array<BufferCopyRegion, 1>{{{indexOffset, 0, sizeof(indices)}}})))
        return fail("begin R6 legacy-alpha frame", status);
    for (std::size_t mode = 0; mode < uniformBuffers.size(); ++mode)
        if (!(status = commands.copyBuffer(upload, uniformBuffers[mode],
            std::array<BufferCopyRegion, 1>{{{
                uniformOffset + mode * sizeof(LegacyUniform), 0, sizeof(LegacyUniform)}}})))
            return fail("upload R6 legacy-alpha uniform", status);

    RenderingInfo rendering;
    rendering.semanticId = 0x52365f4c45474143ull;
    rendering.width = width;
    rendering.height = height;
    rendering.colors.push_back({colorView, Format::RGBA8UNorm, LoadOp::Clear,
        StoreOp::Store, {{0.f, 0.f, 0.f, 0.f}, 0.f, 0}});
    if (!(status = commands.beginRendering(rendering)) ||
        !(status = commands.setViewport(
            {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f})) ||
        !(status = commands.setScissor({0, 0, width, height})))
        return fail("begin R6 legacy-alpha rendering", status);
    const auto draw = [&](PipelineHandle drawPipeline, std::size_t mode, std::size_t quadIndex)
    {
        return (status = commands.bindPipeline(drawPipeline)) &&
               (status = commands.bindBindingSet(0, bindingSets[mode])) &&
               (status = commands.bindIndexBuffer(index, 0, IndexType::UInt16)) &&
               (status = commands.bindVertexBuffer(
                   0, vertex, quadIndex * 4u * sizeof(Vertex))) &&
               (status = commands.drawIndexed({6, 1, 0, 0, 0}));
    };
    if (!draw(sourceOverPipeline, 0, 0) ||
        !draw(maskPipeline, 1, 1) ||
        !draw(particlePipeline, 2, 2) ||
        !draw(sourceOverPipeline, 0, 3) ||
        !draw(emissivePipeline, 3, 3) ||
        !(status = commands.endRendering()))
        return fail("execute R6 legacy-alpha routes", status);
    BufferImageCopyRegion colorCopy;
    colorCopy.imageSubresource = {ImageAspect::Color, 0, 0, 1};
    colorCopy.imageExtent = {width, height, 1};
    if (!(status = commands.copyImageToBuffer(color, readback,
            std::array<BufferImageCopyRegion, 1>{{colorCopy}})) ||
        !(status = commands.endFrame()))
        return fail("finish R6 legacy-alpha frame", status);

    std::vector<std::byte> pixels(colorBytes);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do
    {
        status = device.readBuffer(readback, 0, pixels);
        if (status || status.code() != StatusCode::NotReady) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!status) return fail("read R6 legacy-alpha pixels", status);
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
    if (referenceCoverage && result.shadedPixelCount != referenceCoverage)
        return fail("R6 legacy-alpha coverage mismatch", Status::failure(
            StatusCode::BackendError, std::to_string(result.shadedPixelCount)));
    if (referenceHash[0] && result.colorSha256 != referenceHash)
        return fail("R6 legacy-alpha hash mismatch", Status::failure(
            StatusCode::BackendError, result.colorSha256));

    for (BindingSetHandle bindings : bindingSets) device.destroy(bindings);
    for (BufferHandle uniform : uniformBuffers) device.destroy(uniform);
    for (Status destroyStatus : {
        device.destroy(sourceOverPipeline), device.destroy(maskPipeline),
        device.destroy(particlePipeline), device.destroy(emissivePipeline),
        device.destroy(shader), device.destroy(colorView), device.destroy(color),
        device.destroy(readback), device.destroy(upload), device.destroy(vertex),
        device.destroy(index)})
        if (!destroyStatus) return fail("destroy R6 legacy-alpha resource", destroyStatus);
    if (!(status = device.waitIdle())) return fail("wait for R6 legacy-alpha retirement", status);
    result.passed = true;
    result.message = "R6 legacy alpha/mask/residual fixture PASS";
    return result;
}

} // namespace LL::GHI::Test

#endif // LL_LLGHILEGACYALPHAFIXTURE_H
