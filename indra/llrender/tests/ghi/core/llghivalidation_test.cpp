/**
 * @file llghivalidation_test.cpp
 * @brief Tests for the R0 GHI validation and semantic trace contracts.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "lltut.h"

#include "ghi/core/llghihandlepool.h"
#include "ghi/core/llghishaderpackage.h"
#include "ghi/core/llghivalidation.h"
#include "ghi/include/llghirendererinfo.h"
#include "tests/ghi/llghiresourcefixture.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tut
{

struct LLGHIValidationFixture
{
    static std::string recordTrace(std::uint32_t index_count)
    {
        using namespace LL::GHI;

        DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
        ensure("validation device creation", created.status.ok() && created.device);

        auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
        ensure("validation device type", device != nullptr);

        Status status = Status::success();
        BufferHandle vertices = device->createBuffer(
            {1024, ResourceUsage::Vertex | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal},
            status);
        ensure("vertex buffer creation", status.ok() && vertices);

        BufferHandle indices = device->createBuffer(
            {512, ResourceUsage::Index | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal},
            status);
        ensure("index buffer creation", status.ok() && indices);

        ImageHandle color = device->createImage(
            {{1280, 720, 1}, Format::RGBA8UNorm,
             ResourceUsage::ColorAttachment | ResourceUsage::Sampled,
             1, 1, 1},
            status);
        ensure("color image creation", status.ok() && color);
        ImageViewHandle color_view = device->createImageView(
            {color, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
        ensure("color view creation", status.ok() && color_view);

        ShaderPackageHandle shader = device->createShaderPackage({}, status);
        ensure("shader package creation", status.ok() && shader);

        PipelineDesc pipeline_desc;
        pipeline_desc.shader = shader;
        pipeline_desc.colorFormats = {Format::RGBA8UNorm};
        pipeline_desc.blendStates = {BlendState{}};
        PipelineHandle pipeline = device->createPipeline(pipeline_desc, status);
        ensure("pipeline creation", status.ok() && pipeline);

        RenderingInfo pass;
        pass.semanticId = 0x52305f474849ull; // "R0_GHI"
        pass.width = 1280;
        pass.height = 720;
        pass.colors.push_back(
            {color_view, Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store, {}});

        CommandContext& commands = device->commandContext();
        ensure("begin frame", commands.beginFrame().ok());
        ensure("begin rendering", commands.beginRendering(pass).ok());
        ensure("bind pipeline", commands.bindPipeline(pipeline).ok());
        ensure("bind vertex buffer", commands.bindVertexBuffer(0, vertices, 0).ok());
        ensure("bind index buffer", commands.bindIndexBuffer(indices, 0, IndexType::UInt16).ok());
        ensure("draw indexed", commands.drawIndexed({index_count, 1, 0, 0, 0}).ok());
        ensure("end rendering", commands.endRendering().ok());
        ensure("end frame", commands.endFrame().ok());

        return device->semanticTrace().sha256();
    }
};

using LLGHIValidationFactory = test_group<LLGHIValidationFixture>;
using LLGHIValidationObject = LLGHIValidationFactory::object;
LLGHIValidationFactory gLLGHIValidationFactory("LLGHIValidation");

template<> template<>
void LLGHIValidationObject::test<1>()
{
    using namespace LL::GHI;

    HandlePool<BufferTag> pool;
    BufferHandle first = pool.allocate();
    ensure("allocated handle is live", pool.isLive(first));
    ensure("release succeeds", pool.release(first));
    ensure("released handle is stale", !pool.isLive(first));

    BufferHandle replacement = pool.allocate();
    ensure_equals("slot is reused", replacement.index(), first.index());
    ensure("generation advances", replacement.generation() != first.generation());
    ensure("stale release is rejected", !pool.release(first));
}

template<> template<>
void LLGHIValidationObject::test<2>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    ensure("validation device creation", created.status.ok() && created.device);
    CommandContext& commands = created.device->commandContext();

    ensure(
        "draw outside a pass is rejected",
        commands.draw({3, 1, 0, 0}).code() == StatusCode::InvalidState);
    ensure("begin frame", commands.beginFrame().ok());
    ensure(
        "nested frame is rejected",
        commands.beginFrame().code() == StatusCode::InvalidState);
    ensure("end frame", commands.endFrame().ok());
}

template<> template<>
void LLGHIValidationObject::test<3>()
{
    const std::string first = recordTrace(36);
    const std::string second = recordTrace(36);
    const std::string changed = recordTrace(39);

    ensure_equals("SHA-256 is 64 hexadecimal characters", first.size(), std::size_t{64});
    ensure_equals(
        "R0 semantic trace matches its recorded contract hash",
        first,
        std::string{"005b073be88be2293cf4c5a5ae9c21d40c34eb195c16e5c0aec9f4335cc370bb"});
    ensure_equals("equivalent command streams hash identically", first, second);
    ensure("semantic command changes alter the hash", first != changed);
}

template<> template<>
void LLGHIValidationObject::test<4>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    ensure("validation device creation", created.status.ok() && created.device);
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("validation device type", device != nullptr);

    Status status = Status::success();
    ImageHandle color = device->createImage(
        {{64, 64, 1}, Format::RGBA8UNorm, ResourceUsage::ColorAttachment, 1, 1, 1},
        status);
    ensure("color image creation", status.ok() && color);
    ImageViewHandle color_view = device->createImageView(
        {color, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    ensure("color view creation", status.ok() && color_view);
    ShaderPackageHandle shader = device->createShaderPackage({}, status);
    ensure("shader package creation", status.ok() && shader);

    PipelineDesc incompatible;
    incompatible.shader = shader;
    incompatible.colorFormats = {Format::BGRA8UNorm};
    incompatible.blendStates = {BlendState{}};
    PipelineHandle pipeline = device->createPipeline(incompatible, status);
    ensure("pipeline creation", status.ok() && pipeline);

    RenderingInfo pass;
    pass.width = 64;
    pass.height = 64;
    pass.colors.push_back(
        {color_view, Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store, {}});

    CommandContext& commands = device->commandContext();
    ensure("begin frame", commands.beginFrame().ok());
    ensure("begin rendering", commands.beginRendering(pass).ok());
    ensure(
        "incompatible pipeline is rejected",
        commands.bindPipeline(pipeline).code() == StatusCode::InvalidArgument);
    ensure("end rendering", commands.endRendering().ok());
    ensure("end frame", commands.endFrame().ok());
}

template<> template<>
void LLGHIValidationObject::test<5>()
{
    using namespace LL::GHI;

    clearRendererSnapshot();
    RendererSnapshot snapshot;
    snapshot.identity.backend = Backend::Vulkan;
    snapshot.identity.provider = RendererProvider::NativeVulkan;
    snapshot.identity.vendor = DeviceVendor::AMD;
    snapshot.identity.apiName = "Vulkan";
    snapshot.identity.apiVersion = {1, 4, 0, {}};
    snapshot.identity.deviceName = "AMD Radeon RX 9070 XT";
    snapshot.identity.stableDeviceId = "LUID:0011223344556677";
    snapshot.identity.vendorName = "AMD";
    ensure("complete renderer snapshot", snapshot.complete());

    publishRendererSnapshot(snapshot);
    const auto active = activeRendererSnapshot();
    ensure("published renderer snapshot is available", active.has_value());
    ensure("published renderer snapshot is copied", *active == snapshot);
    ensure_equals(
        "renderer summary is backend neutral",
        formatRendererSummary(active->identity),
        std::string{"Vulkan 1.4 (AMD Radeon RX 9070 XT - Native Vulkan)"});

    clearRendererSnapshot();
    ensure("renderer snapshot clears on lifecycle shutdown", !activeRendererSnapshot());
}

template<> template<>
void LLGHIValidationObject::test<6>()
{
    using namespace LL::GHI;

    RendererSnapshot snapshot;
    snapshot.identity.backend = Backend::Vulkan;
    snapshot.identity.provider = RendererProvider::NativeVulkan;
    snapshot.identity.vendor = DeviceVendor::AMD;
    snapshot.identity.apiName = "Vulkan";
    snapshot.identity.apiVersion = {1, 4, 349, {}};
    snapshot.identity.rendererName = "AMD Radeon RX 9070 XT";
    snapshot.identity.deviceName = snapshot.identity.rendererName;
    snapshot.identity.stableDeviceId = "luid:0011223344556677";
    snapshot.identity.vendorName = "AMD";
    snapshot.identity.dedicatedVideoMemoryBytes = 16ull * 1024ull * 1024ull * 1024ull;
    snapshot.capabilities.maxSampledImagesPerStage = 32;
    snapshot.capabilities.maxVaryingVectors = 32;
    snapshot.capabilities.baselineGraphicsPipeline = true;
    snapshot.capabilities.advancedGraphicsPipeline = true;

    const RendererSupportInfo info = makeRendererSupportInfo(snapshot);
    ensure_equals("support API", info.api, std::string{"Vulkan"});
    ensure_equals("support API version", info.apiVersion, std::string{"1.4.349"});
    ensure_equals("support backend", info.backend, std::string{"Vulkan"});
    ensure_equals("support provider", info.provider, std::string{"Native Vulkan"});
    ensure_equals("support renderer", info.renderer, snapshot.identity.rendererName);
    ensure_equals("support vendor", info.vendor, std::string{"AMD"});
    ensure_equals("support memory", info.videoMemoryBytes,
                  snapshot.identity.dedicatedVideoMemoryBytes);
    ensure("semantic baseline capability", snapshot.capabilities.baselineGraphicsPipeline);
    ensure("semantic advanced capability", snapshot.capabilities.advancedGraphicsPipeline);
}

template<> template<>
void LLGHIValidationObject::test<7>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("validation device", created.status.ok() && device);

    Status status = Status::success();
    BufferHandle upload = device->createBuffer(
        {64, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    ensure("upload buffer", status.ok() && upload);
    BufferHandle local = device->createBuffer(
        {64, ResourceUsage::TransferSource | ResourceUsage::TransferDestination,
         MemoryClass::DeviceLocal}, status);
    ensure("device-local buffer", status.ok() && local);
    BufferHandle readback = device->createBuffer(
        {64, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
    ensure("readback buffer", status.ok() && readback);

    std::array<std::byte, 16> source{};
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        source[i] = static_cast<std::byte>(i * 7);
    }
    ensure("write upload buffer", device->writeBuffer(upload, 8, source).ok());
    ensure("device-local host write rejected",
           device->writeBuffer(local, 0, source).code() == StatusCode::InvalidArgument);

    CommandContext& commands = device->commandContext();
    ensure("begin transfer frame", commands.beginFrame().ok());
    const std::array<BufferCopyRegion, 1> unaligned_copy{{{1, 4, 4}}};
    ensure("unaligned transfer rejected",
           commands.copyBuffer(upload, local, unaligned_copy).code() ==
               StatusCode::InvalidArgument);
    const std::array<BufferCopyRegion, 1> upload_copy{{{8, 4, source.size()}}};
    ensure("upload copy", commands.copyBuffer(upload, local, upload_copy).ok());
    const std::array<BufferCopyRegion, 1> readback_copy{{{4, 12, source.size()}}};
    ensure("readback copy", commands.copyBuffer(local, readback, readback_copy).ok());
    std::array<std::byte, 16> premature{};
    ensure("active-frame readback rejected",
           device->readBuffer(readback, 12, premature).code() == StatusCode::InvalidState);
    ensure("end transfer frame", commands.endFrame().ok());

    std::array<std::byte, 16> result{};
    ensure("readback", device->readBuffer(readback, 12, result).ok());
    ensure("buffer round trip is exact", result == source);
}

template<> template<>
void LLGHIValidationObject::test<8>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("validation device", created.status.ok() && device);
    Status status = Status::success();

    BufferHandle upload = device->createBuffer(
        {64, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    BufferHandle readback = device->createBuffer(
        {16, ResourceUsage::TransferDestination, MemoryClass::Readback}, status);
    ImageHandle image = device->createImage(
        {{4, 4, 1}, Format::RGBA8UNorm,
         ResourceUsage::TransferSource | ResourceUsage::TransferDestination |
             ResourceUsage::Sampled,
         3, 1, 1},
        status);
    ensure("mipped image", status.ok() && upload && readback && image);
    ImageViewHandle view = device->createImageView(
        {image, Format::RGBA8UNorm, {ImageAspect::Color, 0, 3, 0, 1}}, status);
    ensure("full image view", status.ok() && view);
    ensure("image cannot die before its view",
           device->destroy(image).code() == StatusCode::InvalidState);

    std::array<std::byte, 64> pixels{};
    for (std::size_t texel = 0; texel < 16; ++texel)
    {
        pixels[texel * 4 + 0] = std::byte{10};
        pixels[texel * 4 + 1] = std::byte{20};
        pixels[texel * 4 + 2] = std::byte{30};
        pixels[texel * 4 + 3] = std::byte{40};
    }
    ensure("write image upload", device->writeBuffer(upload, 0, pixels).ok());

    BufferImageCopyRegion base;
    base.imageSubresource = {ImageAspect::Color, 0, 0, 1};
    base.imageExtent = {4, 4, 1};
    BufferImageCopyRegion last_mip;
    last_mip.imageSubresource = {ImageAspect::Color, 2, 0, 1};
    last_mip.imageExtent = {1, 1, 1};
    CommandContext& commands = device->commandContext();
    ensure("begin image frame", commands.beginFrame().ok());
    ensure("upload image", commands.copyBufferToImage(upload, image, {&base, 1}).ok());
    ensure("generate mipmaps",
           commands.generateMipmaps(image, {ImageAspect::Color, 0, 3, 0, 1}).ok());
    ensure("read final mip",
           commands.copyImageToBuffer(image, readback, {&last_mip, 1}).ok());
    ensure("end image frame", commands.endFrame().ok());

    std::array<std::byte, 4> result{};
    ensure("mip readback", device->readBuffer(readback, 0, result).ok());
    ensure("mip generation preserves constant color",
           result == std::array<std::byte, 4>{
               std::byte{10}, std::byte{20}, std::byte{30}, std::byte{40}});
    ensure("destroy image view", device->destroy(view).ok());
    ensure("destroy image after view", device->destroy(image).ok());
}

template<> template<>
void LLGHIValidationObject::test<9>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("validation device", created.status.ok() && device);
    Status status = Status::success();
    QueryPoolHandle pool = device->createQueryPool({QueryType::Timestamp, 2}, status);
    ensure("timestamp pool", status.ok() && pool);

    std::array<std::uint64_t, 2> results{};
    ensure("unwritten query is not ready",
           device->getQueryResults(pool, 0, results).code() == StatusCode::NotReady);
    CommandContext& commands = device->commandContext();
    ensure("begin query frame", commands.beginFrame().ok());
    ensure("reset queries", commands.resetQueryPool(pool, 0, 2).ok());
    ensure("first timestamp", commands.writeTimestamp(pool, 0).ok());
    ensure("second timestamp", commands.writeTimestamp(pool, 1).ok());
    ensure("timestamp reuse requires reset",
           commands.writeTimestamp(pool, 1).code() == StatusCode::InvalidState);
    ensure("end query frame", commands.endFrame().ok());
    ensure("query results", device->getQueryResults(pool, 0, results).ok());
    ensure("timestamps are monotonic", results[0] < results[1]);
}

template<> template<>
void LLGHIValidationObject::test<10>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("validation device", created.status.ok() && device);
    Status status = Status::success();
    BufferHandle buffer = device->createBuffer(
        {32, ResourceUsage::Vertex, MemoryClass::DeviceLocal}, status);
    ensure("buffer", status.ok() && buffer);
    ensure("destroy queues retirement", device->destroy(buffer).ok());
    ensure("destroy invalidates handle immediately", !device->isLive(buffer));
    ensure_equals("one pending retirement", device->pendingRetirementCount(), std::size_t{1});

    CommandContext& commands = device->commandContext();
    ensure("begin first retirement frame", commands.beginFrame().ok());
    ensure("end first retirement frame", commands.endFrame().ok());
    ensure_equals("retirement respects in-flight window",
                  device->pendingRetirementCount(), std::size_t{1});
    ensure("begin second retirement frame", commands.beginFrame().ok());
    ensure("end second retirement frame", commands.endFrame().ok());
    ensure_equals("retirement collected", device->pendingRetirementCount(), std::size_t{0});
}

template<> template<>
void LLGHIValidationObject::test<11>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("validation device", created.status.ok() && device);
    Status status = Status::success();
    BufferHandle indices = device->createBuffer(
        {24, ResourceUsage::Index, MemoryClass::DeviceLocal}, status);
    ImageHandle color = device->createImage(
        {{32, 32, 1}, Format::RGBA8UNorm, ResourceUsage::ColorAttachment, 1, 1, 1},
        status);
    ImageViewHandle color_view = device->createImageView(
        {color, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    ShaderPackageHandle shader = device->createShaderPackage({}, status);
    PipelineDesc pipeline_desc;
    pipeline_desc.shader = shader;
    pipeline_desc.colorFormats = {Format::RGBA8UNorm};
    pipeline_desc.blendStates = {BlendState{}};
    PipelineHandle pipeline = device->createPipeline(pipeline_desc, status);
    ensure("index fixture resources", status.ok() && indices && color && color_view && shader && pipeline);

    RenderingInfo pass;
    pass.width = 32;
    pass.height = 32;
    pass.colors.push_back({color_view, Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store, {}});
    CommandContext& commands = device->commandContext();
    ensure("begin index frame", commands.beginFrame().ok());
    ensure("begin index pass", commands.beginRendering(pass).ok());
    ensure("bind index pipeline", commands.bindPipeline(pipeline).ok());
    ensure("bind 16-bit indices", commands.bindIndexBuffer(indices, 0, IndexType::UInt16).ok());
    ensure("16-bit draw", commands.drawIndexed({12, 1, 0, 0, 0}).ok());
    ensure("bind 32-bit indices", commands.bindIndexBuffer(indices, 0, IndexType::UInt32).ok());
    ensure("32-bit draw", commands.drawIndexed({6, 1, 0, 0, 0}).ok());
    ensure("32-bit overrun rejected",
           commands.drawIndexed({7, 1, 0, 0, 0}).code() == StatusCode::InvalidArgument);
    ensure("end index pass", commands.endRendering().ok());
    ensure("end index frame", commands.endFrame().ok());
}

template<> template<>
void LLGHIValidationObject::test<12>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("validation device", created.status.ok() && device);
    const std::array<std::pair<Format, ImageAspect>, 8> formats{{
        {Format::R8UNorm, ImageAspect::Color},
        {Format::RGBA8UNorm, ImageAspect::Color},
        {Format::BGRA8SRGB, ImageAspect::Color},
        {Format::RGBA16Float, ImageAspect::Color},
        {Format::R32UInt, ImageAspect::Color},
        {Format::Depth16UNorm, ImageAspect::Depth},
        {Format::Depth24Stencil8, ImageAspect::DepthStencil},
        {Format::Depth32Float, ImageAspect::Depth},
    }};

    for (const auto& [format, aspect] : formats)
    {
        Status status = Status::success();
        ImageHandle image = device->createImage(
            {{8, 8, 1}, format, ResourceUsage::Sampled, 1, 1, 1}, status);
        ensure("representative image format", status.ok() && image);
        ImageViewHandle view = device->createImageView(
            {image, format, {aspect, 0, 1, 0, 1}}, status);
        ensure("representative image view", status.ok() && view);
        ensure("destroy representative view", device->destroy(view).ok());
        ensure("destroy representative image", device->destroy(image).ok());
    }

    Status status = Status::success();
    ImageHandle color = device->createImage(
        {{8, 8, 1}, Format::RGBA8UNorm, ResourceUsage::Sampled, 1, 1, 1}, status);
    ImageViewHandle invalid = device->createImageView(
        {color, Format::RGBA8UNorm, {ImageAspect::Depth, 0, 1, 0, 1}}, status);
    ensure("incompatible image aspect rejected",
           !invalid && status.code() == StatusCode::InvalidArgument);
}

template<> template<>
void LLGHIValidationObject::test<13>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    ensure("validation device", created.status.ok() && created.device);
    const Test::ResourceFixtureResult result =
        Test::runResourceFixture(*created.device);
    ensure(result.message, result.passed);
}

template<> template<>
void LLGHIValidationObject::test<14>()
{
    using namespace LL::GHI;

    ShaderPackageDesc package;
    package.semanticHash[0] = 0x52;
    ShaderPackageDesc::StageArtifact vertex;
    vertex.stage = ShaderPackageDesc::Stage::Vertex;
    vertex.artifacts = {
        {ShaderPackageDesc::TargetProfile::OpenGL41, "vertex41", {}, {}},
        {ShaderPackageDesc::TargetProfile::OpenGL46, "vertex46", {}, {}},
        {ShaderPackageDesc::TargetProfile::VulkanSpirV13, "", {0x07230203u}, {}},
    };
    ShaderPackageDesc::StageArtifact fragment;
    fragment.stage = ShaderPackageDesc::Stage::Fragment;
    fragment.artifacts = {
        {ShaderPackageDesc::TargetProfile::OpenGL41, "fragment41", {}, {}},
        {ShaderPackageDesc::TargetProfile::OpenGL46, "fragment46", {}, {}},
        {ShaderPackageDesc::TargetProfile::VulkanSpirV13, "", {0x07230203u}, {}},
    };
    package.stages = {vertex, fragment};
    package.bindings = {
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         ShaderPackageDesc::StageVisibility::Vertex |
             ShaderPackageDesc::StageVisibility::Fragment,
         1, true, "FrameData"},
        {2, 3, ShaderPackageDesc::BindingType::CombinedImageSampler,
         ShaderPackageDesc::StageVisibility::Fragment, 1, false, "colorTexture"},
    };
    package.vertexInputs = {
        {0, VertexFormat::Float32x3},
        {1, VertexFormat::Float32x2},
    };

    ShaderPackageDesc equivalent = package;
    ensure("equivalent shader packages compare equal", equivalent == package);
    equivalent.stages[1].entryPoint = "changed";
    ensure("shader package identity includes stage entry points", equivalent != package);
    ensure_equals("R3 package carries three target profiles",
                  package.stages[0].artifacts.size(), std::size_t{3});

    PipelineDesc pipeline;
    pipeline.vertexBuffers = {{0, 20, VertexInputRate::PerVertex}};
    pipeline.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0},
        {1, 0, VertexFormat::Float32x2, 12},
    };
    pipeline.specializationConstants = {{7, {}, 4}};
    ensure_equals("R3 vertex layout count", pipeline.vertexAttributes.size(), std::size_t{2});

    BindingSetDesc bindings;
    bindings.group = 2;
    bindings.resources.push_back(
        {3, 0, ShaderPackageDesc::BindingType::CombinedImageSampler, {}, 0, 0, {}, {}});
    ensure_equals("R3 binding group is explicit", bindings.group, std::uint8_t{2});
    ensure_equals("R3 binding number is explicit", bindings.resources[0].binding,
                  std::uint16_t{3});

    SemanticTrace first_trace;
    first_trace.setViewport({0.f, 0.f, 64.f, 64.f, 0.f, 1.f});
    first_trace.setScissor({0, 0, 64, 64});
    SemanticTrace second_trace;
    second_trace.setViewport({0.f, 0.f, 64.f, 64.f, 0.f, 1.f});
    second_trace.setScissor({0, 0, 64, 64});
    ensure_equals("R3 dynamic state traces deterministically",
                  first_trace.sha256(), second_trace.sha256());
    second_trace.setScissor({0, 0, 32, 64});
    ensure("R3 dynamic state changes semantic trace",
           first_trace.sha256() != second_trace.sha256());
}

#ifdef LL_GHI_R3_SHADER_PACKAGE
template<> template<>
void LLGHIValidationObject::test<15>()
{
    using namespace LL::GHI;

    ShaderPackageDesc package;
    Status status = loadShaderPackage(LL_GHI_R3_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());
    ensure_equals("offline package schema", package.schemaVersion,
                  ShaderPackageDesc::CURRENT_SCHEMA_VERSION);
    ensure_equals("offline package stage count", package.stages.size(), std::size_t{2});
    ensure_equals("offline package target profile count",
                  package.stages.front().artifacts.size(), std::size_t{3});
    ensure_equals("offline package binding count", package.bindings.size(), std::size_t{2});
    ensure_equals("offline package vertex input count",
                  package.vertexInputs.size(), std::size_t{2});

    std::ifstream input(LL_GHI_R3_SHADER_PACKAGE, std::ios::binary);
    std::string corrupted((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    const std::string marker = "\"artifact_hash\":\"";
    const std::size_t hash = corrupted.find(marker);
    ensure("artifact hash present", hash != std::string::npos);
    const std::size_t digit = hash + marker.size();
    corrupted[digit] = corrupted[digit] == '0' ? '1' : '0';
    ShaderPackageDesc rejected;
    status = decodeShaderPackage(corrupted, rejected);
    ensure("corrupt artifact rejected",
           !status && status.code() == StatusCode::InvalidArgument);
}
#endif

} // namespace tut
