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
#include "ghi/core/llghipipelinecache.h"
#include "ghi/core/llghishaderpackage.h"
#include "ghi/core/llghivalidation.h"
#include "ghi/include/llghimaterialscenepacket.h"
#include "ghi/include/llghialphacontract.h"
#include "ghi/include/llghioffscreencontract.h"
#include "ghi/include/llghiopaqueoffscreenprobe.h"
#include "ghi/include/llghiopaquepacketconsumer.h"
#include "ghi/include/llghiopaquescenepacket.h"
#include "ghi/include/llghirendererinfo.h"
#include "ghi/include/llghiworldcontract.h"
#include "tests/ghi/llghidrawfixture.h"
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
    static LL::GHI::ShaderPackageDesc makeUnboundShaderPackage()
    {
        using namespace LL::GHI;
        ShaderPackageDesc package;
        package.semanticHash[0] = 1;
        package.toolchainHash[0] = 1;
        package.stages = {
            {ShaderPackageDesc::Stage::Vertex, "main", {{
                ShaderPackageDesc::TargetProfile::OpenGL41, "void main(){}", {}, {}}}},
            {ShaderPackageDesc::Stage::Fragment, "main", {{
                ShaderPackageDesc::TargetProfile::OpenGL41, "void main(){}", {}, {}}}},
        };
        return package;
    }

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

        ShaderPackageHandle shader = device->createShaderPackage(makeUnboundShaderPackage(), status);
        ensure("shader package creation", status.ok() && shader);

        PipelineDesc pipeline_desc;
        pipeline_desc.shader = shader;
        pipeline_desc.depthTest = false;
        pipeline_desc.depthWrite = false;
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
        ensure("set viewport", commands.setViewport({0.f, 0.f, 1280.f, 720.f, 0.f, 1.f}).ok());
        ensure("set scissor", commands.setScissor({0, 0, 1280, 720}).ok());
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
        std::string{"9be9019fbebb2c829325f6dbc51eb3c3dd9d786ea1133518720d07026beefd2a"});
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
    ShaderPackageHandle shader = device->createShaderPackage(makeUnboundShaderPackage(), status);
    ensure("shader package creation", status.ok() && shader);

    PipelineDesc incompatible;
    incompatible.shader = shader;
    incompatible.depthTest = false;
    incompatible.depthWrite = false;
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
    ShaderPackageHandle shader = device->createShaderPackage(makeUnboundShaderPackage(), status);
    PipelineDesc pipeline_desc;
    pipeline_desc.shader = shader;
    pipeline_desc.depthTest = false;
    pipeline_desc.depthWrite = false;
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
    ensure("set index viewport", commands.setViewport({0.f, 0.f, 32.f, 32.f, 0.f, 1.f}).ok());
    ensure("set index scissor", commands.setScissor({0, 0, 32, 32}).ok());
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
    const std::array<std::pair<Format, ImageAspect>, 11> formats{{
        {Format::R8UNorm, ImageAspect::Color},
        {Format::RGBA8UNorm, ImageAspect::Color},
        {Format::BGRA8SRGB, ImageAspect::Color},
        {Format::RGB10A2UNorm, ImageAspect::Color},
        {Format::RGBA16UNorm, ImageAspect::Color},
        {Format::RGB16Float, ImageAspect::Color},
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
        {ShaderPackageDesc::TargetProfile::OpenGL44, "vertex44", {}, {}},
        {ShaderPackageDesc::TargetProfile::VulkanSpirV13, "", {0x07230203u}, {}},
    };
    ShaderPackageDesc::StageArtifact fragment;
    fragment.stage = ShaderPackageDesc::Stage::Fragment;
    fragment.artifacts = {
        {ShaderPackageDesc::TargetProfile::OpenGL41, "fragment41", {}, {}},
        {ShaderPackageDesc::TargetProfile::OpenGL44, "fragment44", {}, {}},
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
        {0, ShaderValueType::Float3},
        {1, ShaderValueType::Float2},
    };
    package.fragmentOutputs = {{0, ShaderValueType::Float4}};

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
    ensure_equals("offline package fragment output count",
                  package.fragmentOutputs.size(), std::size_t{1});

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

template<> template<>
void LLGHIValidationObject::test<16>()
{
    using namespace LL::GHI;

    ShaderPackageDesc package;
    Status status = loadShaderPackage(LL_GHI_R3_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());
    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* validation = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R3 fixture validation device", created.status.ok() && validation);
    const Test::DrawFixtureResult result = Test::runDrawFixture(*validation, package);
    ensure(result.message, result.passed);
    ensure("R3 capability-selected depth/stencil format",
           result.depthStencilFormat == Format::Depth24Stencil8);
    ensure_equals("R3 indexed fixture semantic hash",
                  validation->semanticTrace().sha256(),
                  std::string{"a12dd9256c090e3f8575508fd39f73d35f151b40ab970eef2c8e72d8ad0e906b"});
}

template<> template<>
void LLGHIValidationObject::test<17>()
{
    using namespace LL::GHI;

    ShaderPackageDesc package;
    Status status = loadShaderPackage(LL_GHI_R3_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());
    package.bindings.front().dynamicOffset = true;
    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R3 negative validation device", created.status.ok() && device);
    ShaderPackageHandle shader = device->createShaderPackage(package, status);
    ensure("create reflected package", status.ok() && shader);

    BufferHandle wrongUsage = device->createBuffer(
        {64, ResourceUsage::Vertex, MemoryClass::DeviceLocal}, status);
    BindingSetDesc wrongSet;
    wrongSet.shader = shader;
    wrongSet.group = 0;
    wrongSet.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         wrongUsage, 0, 64, {}, {}});
    BindingSetHandle rejected = device->createBindingSet(wrongSet, status);
    ensure("uniform binding rejects wrong buffer usage",
           !rejected && status.code() == StatusCode::InvalidHandle);

    BufferHandle uniform = device->createBuffer(
        {64, ResourceUsage::Uniform, MemoryClass::DeviceLocal}, status);
    ensure("create uniform buffer", status.ok() && uniform);
    wrongSet.resources.front().buffer = uniform;
    BindingSetHandle frameSet = device->createBindingSet(wrongSet, status);
    ensure("create complete frame binding set", status.ok() && frameSet);
    ensure("binding resource lifetime enforced",
           device->destroy(uniform).code() == StatusCode::InvalidState);

    PipelineDesc missingInputs;
    missingInputs.shader = shader;
    missingInputs.colorFormats = {Format::RGBA8UNorm};
    missingInputs.blendStates = {BlendState{}};
    PipelineHandle rejectedPipeline = device->createPipeline(missingInputs, status);
    ensure("pipeline rejects missing reflected vertex inputs",
           !rejectedPipeline && status.code() == StatusCode::InvalidArgument);

    PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader;
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    pipelineDesc.colorFormats = {Format::RGBA8UNorm};
    pipelineDesc.blendStates = {BlendState{}};
    pipelineDesc.vertexBuffers = {{0, 20, VertexInputRate::PerVertex}};
    pipelineDesc.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0},
        {1, 0, VertexFormat::Float32x2, 12},
    };
    PipelineHandle pipeline = device->createPipeline(pipelineDesc, status);
    ensure("create pipeline for dynamic binding validation", status.ok() && pipeline);
    const std::array<std::uint32_t, 1> alignedOffset{{0}};
    const std::array<std::uint32_t, 1> misalignedOffset{{1}};
    ensure("aligned dynamic offset accepted",
           device->validateBindingSetForPipeline(
               pipeline, 0, frameSet, alignedOffset).ok());
    ensure("missing dynamic offset rejected",
           device->validateBindingSetForPipeline(
               pipeline, 0, frameSet, {}).code() == StatusCode::InvalidArgument);
    ensure("misaligned dynamic offset rejected",
           device->validateBindingSetForPipeline(
               pipeline, 0, frameSet, misalignedOffset).code() == StatusCode::InvalidArgument);

    ensure("destroy dynamic-validation pipeline", device->destroy(pipeline).ok());
    ensure("destroy frame binding set", device->destroy(frameSet).ok());
    ensure("destroy uniform after binding set", device->destroy(uniform).ok());
    ensure("destroy wrong-usage buffer", device->destroy(wrongUsage).ok());
    ensure("destroy reflected package", device->destroy(shader).ok());
    ensure("wait for R3 negative retirements", device->waitIdle().ok());
}

template<> template<>
void LLGHIValidationObject::test<18>()
{
    using namespace LL::GHI;

    ShaderPackageDesc package;
    Status status = loadShaderPackage(LL_GHI_R3_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());
    PipelineDesc pipeline;
    pipeline.cullMode = CullMode::Back;
    pipeline.depthTest = true;
    pipeline.depthWrite = true;
    pipeline.depthCompare = CompareOp::GreaterEqual;
    pipeline.colorFormats = {Format::RGBA8UNorm};
    pipeline.depthStencilFormat = Format::Depth24Stencil8;
    pipeline.blendStates = {BlendState{}};
    pipeline.vertexBuffers = {{0, 20, VertexInputRate::PerVertex}};
    pipeline.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0},
        {1, 0, VertexFormat::Float32x2, 12},
    };
    const PipelineCacheDomain domain{"device-a", "driver-a"};
    const auto identity = pipelineCacheIdentity(
        package, pipeline, ShaderPackageDesc::TargetProfile::OpenGL44,
        Backend::OpenGL, domain);
    ensure_equals("pipeline cache identity is deterministic", identity,
        pipelineCacheIdentity(package, pipeline,
            ShaderPackageDesc::TargetProfile::OpenGL44,
            Backend::OpenGL, domain));
    ensure_equals("pipeline cache identity is SHA-256", identity.size(), std::size_t{64});

    ShaderPackageDesc changedPackage = package;
    changedPackage.semanticHash[0] ^= 1;
    ensure("semantic source change invalidates pipeline cache",
        identity != pipelineCacheIdentity(changedPackage, pipeline,
            ShaderPackageDesc::TargetProfile::OpenGL44,
            Backend::OpenGL, domain));
    changedPackage = package;
    changedPackage.toolchainHash[0] ^= 1;
    ensure("toolchain change invalidates pipeline cache",
        identity != pipelineCacheIdentity(changedPackage, pipeline,
            ShaderPackageDesc::TargetProfile::OpenGL44,
            Backend::OpenGL, domain));

    PipelineDesc changedPipeline = pipeline;
    changedPipeline.colorFormats = {Format::RGBA8SRGB};
    ensure("pipeline state change invalidates pipeline cache",
        identity != pipelineCacheIdentity(package, changedPipeline,
            ShaderPackageDesc::TargetProfile::OpenGL44,
            Backend::OpenGL, domain));
    ensure("target profile change invalidates pipeline cache",
        identity != pipelineCacheIdentity(package, pipeline,
            ShaderPackageDesc::TargetProfile::OpenGL41,
            Backend::OpenGL, domain));
    ensure("backend change invalidates pipeline cache",
        identity != pipelineCacheIdentity(package, pipeline,
            ShaderPackageDesc::TargetProfile::VulkanSpirV13,
            Backend::Vulkan, domain));
    ensure("device change invalidates pipeline cache",
        identity != pipelineCacheIdentity(package, pipeline,
            ShaderPackageDesc::TargetProfile::OpenGL44,
            Backend::OpenGL, {"device-b", "driver-a"}));
    ensure("driver change invalidates pipeline cache",
        identity != pipelineCacheIdentity(package, pipeline,
            ShaderPackageDesc::TargetProfile::OpenGL44,
            Backend::OpenGL, {"device-a", "driver-b"}));
}

template<> template<>
void LLGHIValidationObject::test<19>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R4 vertex-interface validation device", created.status.ok() && device);

    ShaderPackageDesc package = makeUnboundShaderPackage();
    package.vertexInputs = {
        {0, ShaderValueType::Float3},
        {1, ShaderValueType::Float4},
        {2, ShaderValueType::UInt4},
    };
    package.fragmentOutputs = {{0, ShaderValueType::Float4}};
    Status status = Status::success();
    ShaderPackageHandle shader = device->createShaderPackage(package, status);
    ensure("R4 reflected shader package", status.ok() && shader);

    PipelineDesc compatible;
    compatible.shader = shader;
    compatible.depthTest = false;
    compatible.depthWrite = false;
    compatible.colorFormats = {Format::RGBA8UNorm};
    compatible.blendStates = {BlendState{}};
    compatible.vertexBuffers = {{0, 24, VertexInputRate::PerVertex}};
    compatible.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0},
        // Packed normalized colors deliver a float4 shader value.
        {1, 0, VertexFormat::UNorm8x4, 12},
        // Packed joint indices deliver a uvec4 shader value.
        {2, 0, VertexFormat::UInt16x4, 16},
    };
    PipelineHandle pipeline = device->createPipeline(compatible, status);
    ensure("normalized colors and packed joints satisfy reflected values",
           status.ok() && pipeline);

    PipelineDesc incompatible = compatible;
    incompatible.vertexAttributes[1].format = VertexFormat::UInt32;
    PipelineHandle rejected = device->createPipeline(incompatible, status);
    ensure("storage with the wrong delivered value shape is rejected",
           !rejected && status.code() == StatusCode::InvalidArgument);
}

template<> template<>
void LLGHIValidationObject::test<20>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R4 MRT validation device", created.status.ok() && device);
    ensure("R4 fixture supports four color targets",
           device->capabilities().maxColorAttachments >= 4);
    ensure("R4 fixture supports independent attachment state",
           device->capabilities().independentBlend);

    Status status = Status::success();
    std::array<ImageHandle, 4> images;
    std::array<ImageViewHandle, 4> views;
    const std::array<Format, 4> targetFormats{{
        Format::RGBA8UNorm,
        Format::RGBA8UNorm,
        Format::RGBA16UNorm,
        Format::RGBA16Float,
    }};
    for (std::size_t index = 0; index < images.size(); ++index)
    {
        images[index] = device->createImage(
            {{32, 32, 1}, targetFormats[index],
             ResourceUsage::ColorAttachment, 1, 1, 1}, status);
        ensure("R4 MRT image", status.ok() && images[index]);
        views[index] = device->createImageView(
            {images[index], targetFormats[index],
             {ImageAspect::Color, 0, 1, 0, 1}}, status);
        ensure("R4 MRT image view", status.ok() && views[index]);
    }

    ShaderPackageDesc package = makeUnboundShaderPackage();
    package.fragmentOutputs = {
        {0, ShaderValueType::Float4},
        {1, ShaderValueType::Float4},
        {2, ShaderValueType::Float4},
        {3, ShaderValueType::Float4},
    };
    ShaderPackageHandle shader = device->createShaderPackage(package, status);
    ensure("R4 MRT shader package", status.ok() && shader);

    PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader;
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    pipelineDesc.colorFormats.assign(targetFormats.begin(), targetFormats.end());
    pipelineDesc.blendStates.assign(4, BlendState{});
    PipelineHandle pipeline = device->createPipeline(pipelineDesc, status);
    ensure("four reflected outputs match four color targets", status.ok() && pipeline);

    PipelineDesc missingTarget = pipelineDesc;
    missingTarget.colorFormats.pop_back();
    missingTarget.blendStates.pop_back();
    PipelineHandle rejected = device->createPipeline(missingTarget, status);
    ensure("missing reflected fragment target is rejected",
           !rejected && status.code() == StatusCode::InvalidArgument);

    PipelineDesc wrongType = pipelineDesc;
    wrongType.colorFormats[3] = Format::R32UInt;
    rejected = device->createPipeline(wrongType, status);
    ensure("fragment output numeric type mismatch is rejected",
           !rejected && status.code() == StatusCode::InvalidArgument);

    RenderingInfo pass;
    pass.semanticId = 0x52345f4d5254ull; // "R4_MRT"
    pass.width = 32;
    pass.height = 32;
    for (std::size_t index = 0; index < views.size(); ++index)
    {
        pass.colors.push_back(
            {views[index], targetFormats[index], LoadOp::Clear, StoreOp::Store, {}});
    }

    CommandContext& commands = device->commandContext();
    ensure("begin R4 MRT frame", commands.beginFrame().ok());
    RenderingInfo aliased = pass;
    aliased.colors[3].view = aliased.colors[0].view;
    ensure("aliased MRT attachment is rejected",
           commands.beginRendering(aliased).code() == StatusCode::InvalidArgument);
    ensure("begin four-target rendering", commands.beginRendering(pass).ok());
    ensure("bind four-target pipeline", commands.bindPipeline(pipeline).ok());
    ensure("end four-target rendering", commands.endRendering().ok());
    ensure("end R4 MRT frame", commands.endFrame().ok());
}

template<> template<>
void LLGHIValidationObject::test<21>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("validation device", created.status.ok() && device);
    ensure("validation advertises occlusion queries",
           device->capabilities().occlusionQueries);

    Status status = Status::success();
    QueryPoolHandle timestamps = device->createQueryPool(
        {QueryType::Timestamp, 1}, status);
    ensure("timestamp pool", status.ok() && timestamps);
    QueryPoolHandle occlusion = device->createQueryPool(
        {QueryType::Occlusion, 2}, status);
    ensure("occlusion pool", status.ok() && occlusion);
    ImageHandle color = device->createImage(
        {{32, 32, 1}, Format::RGBA8UNorm, ResourceUsage::ColorAttachment,
         1, 1, 1}, status);
    ImageViewHandle colorView = device->createImageView(
        {color, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    ShaderPackageHandle shader = device->createShaderPackage(
        makeUnboundShaderPackage(), status);
    PipelineDesc pipelineDesc;
    pipelineDesc.shader = shader;
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    pipelineDesc.colorFormats = {Format::RGBA8UNorm};
    pipelineDesc.blendStates = {BlendState{}};
    PipelineHandle pipeline = device->createPipeline(pipelineDesc, status);
    ensure("occlusion fixture resources",
           status.ok() && color && colorView && shader && pipeline);

    RenderingInfo pass;
    pass.semanticId = 0x5234635f4f4343ull; // "R4c_OCC"
    pass.width = 32;
    pass.height = 32;
    pass.colors.push_back(
        {colorView, Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store, {}});

    CommandContext& commands = device->commandContext();
    ensure("begin occlusion frame", commands.beginFrame().ok());
    ensure("reset occlusion queries", commands.resetQueryPool(occlusion, 0, 2).ok());
    ensure("occlusion query requires rendering",
           commands.beginQuery(occlusion, 0).code() == StatusCode::InvalidState);
    ensure("begin occlusion pass", commands.beginRendering(pass).ok());
    ensure("timestamp pool rejected by beginQuery",
           commands.beginQuery(timestamps, 0).code() == StatusCode::InvalidArgument);
    ensure("begin visible query", commands.beginQuery(occlusion, 0).ok());
    ensure("queries cannot overlap",
           commands.beginQuery(occlusion, 1).code() == StatusCode::InvalidState);
    ensure("active query blocks pass end",
           commands.endRendering().code() == StatusCode::InvalidState);
    ensure("bind occlusion pipeline", commands.bindPipeline(pipeline).ok());
    ensure("set occlusion viewport",
           commands.setViewport({0.f, 0.f, 32.f, 32.f, 0.f, 1.f}).ok());
    ensure("set occlusion scissor", commands.setScissor({0, 0, 32, 32}).ok());
    ensure("instanced query draw", commands.draw({3, 4, 0, 0}).ok());
    ensure("wrong query cannot end",
           commands.endQuery(occlusion, 1).code() == StatusCode::InvalidState);
    ensure("end visible query", commands.endQuery(occlusion, 0).ok());
    ensure("query reuse requires reset",
           commands.beginQuery(occlusion, 0).code() == StatusCode::InvalidState);
    ensure("begin empty query", commands.beginQuery(occlusion, 1).ok());
    ensure("end empty query", commands.endQuery(occlusion, 1).ok());
    ensure("end occlusion pass", commands.endRendering().ok());
    ensure("timestamp operation rejects occlusion pool",
           commands.writeTimestamp(occlusion, 0).code() == StatusCode::InvalidArgument);
    std::array<std::uint64_t, 2> results{};
    ensure("query reads rejected during frame",
           device->getQueryResults(occlusion, 0, results).code() ==
               StatusCode::InvalidState);
    ensure("end occlusion frame", commands.endFrame().ok());
    ensure("read occlusion results",
           device->getQueryResults(occlusion, 0, results).ok());
    ensure_equals("visible query records four instances", results[0], std::uint64_t{4});
    ensure_equals("empty query records zero", results[1], std::uint64_t{0});
}
#endif

template<> template<>
void LLGHIValidationObject::test<22>()
{
    using namespace LL::GHI;

    OpaqueScenePacket source;
    source.sourceWidth = 1920;
    source.sourceHeight = 1080;
    source.frameId = 42;
    source.sceneEpoch = 7;
    source.productionOcclusionEnabled = true;
    source.statistics = {2, 4, 1, 2, 0, 1, 0};
    source.vertices = {
        {{{-1.f, -1.f, 0.5f}}, {{255, 0, 0, 255}}},
        {{{ 1.f, -1.f, 0.5f}}, {{0, 255, 0, 255}}},
        {{{ 1.f,  1.f, 0.5f}}, {{0, 0, 255, 255}}},
        {{{-1.f,  1.f, 0.5f}}, {{255, 255, 255, 255}}},
    };
    source.indices = {0, 1, 2, 2, 3, 0};
    OpaqueSceneDraw draw;
    draw.firstIndex = 0;
    draw.indexCount = 6;
    draw.transform = {{
        1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}};
    draw.semanticId = 0x5234645f4c495645ull; // "R4d_LIVE"
    draw.flags = OpaqueSceneDrawFlags::Fullbright;
    source.draws.push_back(draw);

    std::vector<std::byte> first;
    std::vector<std::byte> second;
    ensure("encode opaque scene packet", encodeOpaqueScenePacket(source, first).ok());
    ensure("opaque scene encoding is deterministic",
           encodeOpaqueScenePacket(source, second).ok() && first == second);
    OpaqueScenePacket decoded;
    ensure("decode opaque scene packet", decodeOpaqueScenePacket(first, decoded).ok());
    ensure("opaque scene packet round trips exactly", decoded == source);
    ensure_equals("opaque scene packet has a stable schema hash",
                  opaqueScenePacketSha256(source),
                  std::string{"1021c6d0a2bcc00dd1e67612773ddce948714736cab127ee4989457baa068468"});

    first.pop_back();
    ensure("truncated opaque packet rejected",
           decodeOpaqueScenePacket(first, decoded).code() == StatusCode::InvalidArgument);
    source.indices[0] = 99;
    ensure("out-of-range opaque index rejected",
           encodeOpaqueScenePacket(source, first).code() == StatusCode::InvalidArgument);
}

#ifdef LL_GHI_R5A_SHADER_PACKAGE
template<> template<>
void LLGHIValidationObject::test<23>()
{
    using namespace LL::GHI;

    ShaderPackageDesc package;
    Status status = loadShaderPackage(LL_GHI_R5A_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());
    ensure_equals("R5a reflected binding count", package.bindings.size(), std::size_t{7});
    ensure_equals("R5a reflected vertex input count",
                  package.vertexInputs.size(), std::size_t{7});
    ensure_equals("R5a reflected deferred output count",
                  package.fragmentOutputs.size(), std::size_t{4});

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R5a validation device", created.status.ok() && device);
    ShaderPackageHandle shader = device->createShaderPackage(package, status);
    ensure("R5a shader package", status.ok() && shader);
    BufferHandle material = device->createBuffer(
        {48, ResourceUsage::Uniform, MemoryClass::DeviceLocal}, status);
    ImageHandle image = device->createImage(
        {{2, 2, 1}, Format::RGBA8UNorm, ResourceUsage::Sampled, 1, 1, 1}, status);
    ImageViewHandle view = device->createImageView(
        {image, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    SamplerHandle sampler = device->createSampler({}, status);
    ensure("R5a material resources", status.ok() && material && image && view && sampler);

    BindingSetDesc materialSet;
    materialSet.shader = shader;
    materialSet.group = 2;
    materialSet.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         material, 0, 48, {}, {}});
    for (std::uint16_t binding = 1; binding < 4; ++binding)
        materialSet.resources.push_back(
            {binding, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
             {}, 0, 0, view, sampler});
    BindingSetHandle rejected = device->createBindingSet(materialSet, status);
    ensure("R5a incomplete texture set is rejected",
           !rejected && status.code() == StatusCode::InvalidArgument);
    materialSet.resources.push_back(
        {4, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
         {}, 0, 0, view, sampler});
    BindingSetHandle accepted = device->createBindingSet(materialSet, status);
    ensure("R5a complete four-texture material set is accepted",
           status.ok() && accepted);

    PipelineDesc pipeline;
    pipeline.shader = shader;
    pipeline.depthTest = false;
    pipeline.depthWrite = false;
    pipeline.colorFormats = {
        Format::RGBA8UNorm, Format::RGBA8UNorm,
        Format::RGBA16UNorm, Format::RGBA16Float};
    pipeline.blendStates.assign(4, BlendState{});
    pipeline.vertexBuffers = {{0, 76, VertexInputRate::PerVertex}};
    pipeline.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0},
        {1, 0, VertexFormat::Float32x3, 12},
        {2, 0, VertexFormat::Float32x4, 24},
        {3, 0, VertexFormat::Float32x2, 40},
        {4, 0, VertexFormat::UNorm8x4, 48},
        {5, 0, VertexFormat::UInt16x4, 52},
        {6, 0, VertexFormat::Float32x4, 60},
    };
    PipelineHandle compatible = device->createPipeline(pipeline, status);
    ensure("R5a packed material/skin vertex layout is accepted",
           status.ok() && compatible);
    pipeline.vertexAttributes[5].format = VertexFormat::Float32x4;
    PipelineHandle wrongJoints = device->createPipeline(pipeline, status);
    ensure("R5a floating joint indices are rejected",
           !wrongJoints && status.code() == StatusCode::InvalidArgument);
}
#endif

template<> template<>
void LLGHIValidationObject::test<24>()
{
    using namespace LL::GHI;

    auto digest = [](std::uint8_t seed)
    {
        ResourceDigest value{};
        for (std::size_t i = 0; i < value.size(); ++i)
            value[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
        return value;
    };

    MaterialScenePacket source;
    source.frameId = 101;
    source.sceneEpoch = 12;
    source.resourceEpoch = 4;
    MaterialTextureResource texture;
    texture.sourceIdentity = digest(1);
    texture.contentIdentity = digest(33);
    texture.colorSpace = TextureColorSpace::SRGB;
    texture.width = 2;
    texture.height = 1;
    texture.components = 4;
    texture.discardLevel = 2;
    texture.decodedPixels = {
        std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3},
        std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}};
    source.textures.push_back(texture);

    MaterialResource material;
    material.identity = digest(65);
    material.model = MaterialModel::MetallicRoughness;
    material.alphaMode = MaterialAlphaMode::Mask;
    material.baseColor = {{0.25f, 0.5f, 0.75f, 1.f}};
    material.emissive = {{0.1f, 0.2f, 0.3f}};
    material.metallic = 0.8f;
    material.roughness = 0.35f;
    material.alphaCutoff = 0.42f;
    material.doubleSided = true;
    material.textures.push_back({TextureSemantic::BaseColor, 0, 0,
                                 {{0.1f, 0.2f, 2.f, 3.f, 0.4f}}});
    source.materials.push_back(material);

    SkinResource skin;
    skin.identity = digest(97);
    skin.jointCount = 1;
    skin.matrixPalette = {1.f, 0.f, 0.f, 2.f, 0.f, 1.f,
                          0.f, 3.f, 0.f, 0.f, 1.f, 4.f};
    source.skins.push_back(skin);
    source.draws.push_back({0x5235625f4c495645ull, 0, 0,
                            ResourceComparability::Comparable});

    std::vector<std::byte> first;
    std::vector<std::byte> second;
    ensure("encode material scene packet",
           encodeMaterialScenePacket(source, first).ok());
    ensure("material scene encoding is deterministic",
           encodeMaterialScenePacket(source, second).ok() && first == second);
    MaterialScenePacket decoded;
    ensure("decode material scene packet",
           decodeMaterialScenePacket(first, decoded).ok());
    ensure("material scene packet round trips exactly", decoded == source);
    ensure_equals("material scene packet has a stable schema hash",
                  materialScenePacketSha256(source),
                  std::string{"976419b26a480a9e3bf16ea017498181047720af6a5441e121929cc95eab9b14"});

    first.pop_back();
    ensure("truncated material scene packet rejected",
           decodeMaterialScenePacket(first, decoded).code() == StatusCode::InvalidArgument);
    source.textures[0].contentIdentity = {};
    ensure("pixel payload without content identity rejected",
           encodeMaterialScenePacket(source, first).code() == StatusCode::InvalidArgument);
}

#ifdef LL_GHI_R5_WORLD_SHADER_PACKAGE
template<> template<>
void LLGHIValidationObject::test<25>()
{
    using namespace LL::GHI;

    ensure("R5 terrain depth precedes material",
           WORLD_PASS_ORDER[0] == WorldPass::TerrainDepth);
    ensure("R5 lighting follows shadow inputs",
           WORLD_PASS_ORDER[4] == WorldPass::DeferredLighting);
    ensure("R5 water follows atmosphere and reflection",
           WORLD_PASS_ORDER[7] == WorldPass::WaterSurface);
    ensure("R5 underwater is the final environment classification",
           WORLD_PASS_ORDER[8] == WorldPass::Underwater);

    ShaderPackageDesc package;
    Status status = loadShaderPackage(LL_GHI_R5_WORLD_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());
    ensure_equals("R5 world reflected binding count",
                  package.bindings.size(), std::size_t{7});
    ensure_equals("R5 world reflected vertex input count",
                  package.vertexInputs.size(), std::size_t{3});
    ensure_equals("R5 world reflected output count",
                  package.fragmentOutputs.size(), std::size_t{3});

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R5 world validation device", created.status.ok() && device);
    ShaderPackageHandle shader = device->createShaderPackage(package, status);
    ensure("R5 world shader package", status.ok() && shader);
    BufferHandle world = device->createBuffer(
        {352, ResourceUsage::Uniform, MemoryClass::DeviceLocal}, status);
    ImageHandle image = device->createImage(
        {{2, 2, 1}, Format::RGBA8UNorm, ResourceUsage::Sampled, 1, 1, 1}, status);
    ImageViewHandle view = device->createImageView(
        {image, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    SamplerHandle sampler = device->createSampler({}, status);
    ensure("R5 world resources", status.ok() && world && image && view && sampler);

    BindingSetDesc worldSet;
    worldSet.shader = shader;
    worldSet.group = 0;
    worldSet.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         world, 0, 352, {}, {}});
    for (std::uint16_t binding = 1; binding < 6; ++binding)
        worldSet.resources.push_back(
            {binding, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
             {}, 0, 0, view, sampler});
    BindingSetHandle rejected = device->createBindingSet(worldSet, status);
    ensure("R5 world incomplete terrain/environment resources rejected",
           !rejected && status.code() == StatusCode::InvalidArgument);
    worldSet.resources.push_back(
        {6, 0, ShaderPackageDesc::BindingType::CombinedImageSampler,
         {}, 0, 0, view, sampler});
    BindingSetHandle accepted = device->createBindingSet(worldSet, status);
    ensure("R5 world complete resources accepted", status.ok() && accepted);

    PipelineDesc pipeline;
    pipeline.shader = shader;
    pipeline.depthTest = true;
    pipeline.depthWrite = true;
    pipeline.depthCompare = CompareOp::GreaterEqual;
    pipeline.colorFormats.assign(3, Format::RGBA8UNorm);
    pipeline.depthStencilFormat = Format::Depth24Stencil8;
    pipeline.blendStates.assign(3, BlendState{});
    pipeline.vertexBuffers = {{0, 32, VertexInputRate::PerVertex}};
    pipeline.vertexAttributes = {
        {0, 0, VertexFormat::Float32x3, 0},
        {1, 0, VertexFormat::Float32x3, 12},
        {2, 0, VertexFormat::Float32x2, 24}};
    PipelineHandle compatible = device->createPipeline(pipeline, status);
    ensure("R5 world terrain/lighting/environment pipeline accepted",
           status.ok() && compatible);
    pipeline.colorFormats.pop_back();
    pipeline.blendStates.pop_back();
    PipelineHandle missingEnvironment = device->createPipeline(pipeline, status);
    ensure("R5 world pipeline without environment target rejected",
           !missingEnvironment && status.code() == StatusCode::InvalidArgument);
}
#endif

template<> template<>
void LLGHIValidationObject::test<26>()
{
    using namespace LL::GHI;

    AlphaRoutingState ppll{AlphaMethod::PPLL, true, true, false};
    AlphaRoutingState peel{AlphaMethod::DepthPeeling, true, true, false};
    ensure("R6 masks remain masks",
        routeAlphaSubmission({AlphaViewPhase::MainPostWater,
                              AlphaSubmissionClass::Mask}, ppll).route ==
        AlphaRoute::Mask);
    ensure("R6 particles remain on residual legacy alpha",
        routeAlphaSubmission({AlphaViewPhase::MainPostWater,
                              AlphaSubmissionClass::Particle}, ppll).route ==
        AlphaRoute::LegacyResidual);
    ensure("R6 custom blends remain on residual legacy alpha",
        routeAlphaSubmission({AlphaViewPhase::MainPostWater,
                              AlphaSubmissionClass::CustomBlend}, peel).route ==
        AlphaRoute::LegacyResidual);
    ensure("R6 pre-water alpha never enters PPLL",
        routeAlphaSubmission({AlphaViewPhase::PreWater,
                              AlphaSubmissionClass::StandardBlend}, ppll).route ==
        AlphaRoute::LegacySorted);
    ensure("R6 mirrors never enter depth peeling",
        routeAlphaSubmission({AlphaViewPhase::Reflection,
                              AlphaSubmissionClass::StandardBlend}, peel).route ==
        AlphaRoute::LegacySorted);
    AlphaSubmission fullbright{AlphaViewPhase::MainPostWater,
        AlphaSubmissionClass::StandardBlend, false, true, true};
    ensure("R6 fullbright alpha is captured once with one emissive replay",
        routeAlphaSubmission(fullbright, ppll) ==
            AlphaRoutingDecision{AlphaRoute::PPLLCapture, true});
    ensure("R6 depth peeling accepts ordinary cards",
        routeAlphaSubmission({AlphaViewPhase::MainPostWater,
                              AlphaSubmissionClass::StandardBlend}, peel).route ==
        AlphaRoute::DepthPeelExact);
    peel.transientLoad = true;
    ensure("R6 transient load falls back to legacy",
        routeAlphaSubmission({AlphaViewPhase::MainPostWater,
                              AlphaSubmissionClass::StandardBlend}, peel).route ==
        AlphaRoute::LegacySorted);

    const AlphaPPLLAllocation planned = planAlphaPPLLAllocation(
        2560, 1369, 512ull << 20, {});
    ensure("R6 default PPLL allocation is usable", planned.usable);
    ensure_equals("R6 PPLL exact layer clamp", planned.exactLayersPerPixel, 24u);
    ensure("R6 rejects sub-pixel-average PPLL pools",
        !planAlphaPPLLAllocation(2560, 1369, 32, {}).usable);
    ensure("R6 depth peeling keeps accepted defaults",
        clampAlphaDepthPeelPolicy({4, 2}) == AlphaDepthPeelPolicy{4, 2});
    ensure("R6 depth peeling clamps pathological settings",
        clampAlphaDepthPeelPolicy({0, 100}) == AlphaDepthPeelPolicy{1, 50});

    RendererCapabilities capabilities;
    capabilities.advancedGraphicsPipeline = false;
    capabilities.storageImageAtomics = true;
    capabilities.maxStorageBuffersPerStage = 2;
    ensure("R6 PPLL semantic capabilities accepted", supportsPPLL(capabilities));
    capabilities.maxStorageBuffersPerStage = 1;
    ensure("R6 PPLL requires node and counter storage buffers",
        !supportsPPLL(capabilities));
    capabilities.maxStorageBuffersPerStage = 2;
    capabilities.storageImageAtomics = false;
    ensure("R6 PPLL requires storage image atomics", !supportsPPLL(capabilities));

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R6 validation device", created.status.ok() && device);
    ensure("R6 barrier outside a frame rejected",
        device->commandContext().resourceBarrier(
            ResourceBarrier::StorageWriteToRead).code() == StatusCode::InvalidState);
    ensure("R6 barrier frame begins", device->commandContext().beginFrame().ok());
    ensure("R6 storage dependency barrier accepted",
        device->commandContext().resourceBarrier(
            ResourceBarrier::StorageWriteToRead).ok());
    ensure("R6 depth-peel dependency barrier accepted",
        device->commandContext().resourceBarrier(
            ResourceBarrier::DepthAttachmentWriteToSampledRead).ok());
    ensure("R6 barrier frame ends", device->commandContext().endFrame().ok());
}

#if defined(LL_GHI_R6_PPLL_SHADER_PACKAGE) && \
    defined(LL_GHI_R6_PEEL_SHADER_PACKAGE) && \
    defined(LL_GHI_R6_LEGACY_SHADER_PACKAGE)
template<> template<>
void LLGHIValidationObject::test<27>()
{
    using namespace LL::GHI;

    ShaderPackageDesc ppllPackage;
    Status status = loadShaderPackage(LL_GHI_R6_PPLL_SHADER_PACKAGE, ppllPackage);
    ensure(status.message(), status.ok());
    ensure_equals("R6 PPLL reflected binding count",
                  ppllPackage.bindings.size(), std::size_t{4});
    ensure("R6 PPLL reflection includes storage image",
        ppllPackage.bindings[1].type == ShaderPackageDesc::BindingType::StorageImage);
    ensure("R6 PPLL reflection includes compact node storage",
        ppllPackage.bindings[2].type == ShaderPackageDesc::BindingType::StorageBuffer);

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R6 package validation device", created.status.ok() && device);
    ShaderPackageHandle ppllShader = device->createShaderPackage(ppllPackage, status);
    ensure("R6 PPLL package accepted", status.ok() && ppllShader);
    BufferHandle uniform = device->createBuffer(
        {32, ResourceUsage::Uniform, MemoryClass::DeviceLocal}, status);
    BufferHandle nodes = device->createBuffer(
        {4096, ResourceUsage::Storage, MemoryClass::DeviceLocal}, status);
    BufferHandle counter = device->createBuffer(
        {8, ResourceUsage::Storage, MemoryClass::DeviceLocal}, status);
    ImageHandle head = device->createImage(
        {{16, 16, 1}, Format::R32UInt, ResourceUsage::Storage, 1, 1, 1}, status);
    ImageViewHandle headView = device->createImageView(
        {head, Format::R32UInt, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    ensure("R6 PPLL validation resources", status.ok());
    BindingSetDesc ppllSet;
    ppllSet.shader = ppllShader;
    ppllSet.group = 0;
    ppllSet.resources = {
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         uniform, 0, 32, {}, {}},
        {1, 0, ShaderPackageDesc::BindingType::StorageImage,
         {}, 0, 0, headView, {}},
        {2, 0, ShaderPackageDesc::BindingType::StorageBuffer,
         nodes, 0, 4096, {}, {}},
    };
    BindingSetHandle incomplete = device->createBindingSet(ppllSet, status);
    ensure("R6 PPLL missing counter is rejected",
        !incomplete && status.code() == StatusCode::InvalidArgument);
    ppllSet.resources.push_back(
        {3, 0, ShaderPackageDesc::BindingType::StorageBuffer,
         counter, 0, 8, {}, {}});
    BindingSetHandle complete = device->createBindingSet(ppllSet, status);
    ensure("R6 PPLL complete storage contract accepted", status.ok() && complete);

    ShaderPackageDesc peelPackage;
    status = loadShaderPackage(LL_GHI_R6_PEEL_SHADER_PACKAGE, peelPackage);
    ensure(status.message(), status.ok());
    ensure_equals("R6 depth-peel reflected binding count",
                  peelPackage.bindings.size(), std::size_t{2});
    ensure("R6 depth-peel reflection samples prior depth",
        peelPackage.bindings[1].type ==
            ShaderPackageDesc::BindingType::CombinedImageSampler);

    ShaderPackageDesc legacyPackage;
    status = loadShaderPackage(LL_GHI_R6_LEGACY_SHADER_PACKAGE, legacyPackage);
    ensure(status.message(), status.ok());
    ensure_equals("R6 legacy-alpha reflected binding count",
                  legacyPackage.bindings.size(), std::size_t{1});
    ensure("R6 legacy-alpha reflection includes route parameters",
        legacyPackage.bindings[0].type ==
            ShaderPackageDesc::BindingType::UniformBuffer);
}
#endif

template<> template<>
void LLGHIValidationObject::test<28>()
{
    using namespace LL::GHI;

    const OffscreenPassDesc mainPass;
    ensure("R7 main-view contract accepted", validOffscreenPass(mainPass));
    OffscreenPassDesc probePass;
    probePass.view = RenderViewClass::ReflectionProbe;
    probePass.recursionDepth = 1;
    probePass.face = CubeFace::PositiveY;
    probePass.probePhase = ProbePhase::DirectLighting;
    probePass.arrayLayer = cubeArrayLayer(1, CubeFace::PositiveY);
    probePass.updateEpoch = 17;
    ensure("R7 cube face/layer contract accepted", validOffscreenPass(probePass));
    ensure_equals("R7 cube array layer mapping", probePass.arrayLayer,
                  std::uint16_t{8});
    ensure("R7 semantic identity is deterministic",
           offscreenSemanticId(probePass) == offscreenSemanticId(probePass));
    OffscreenPassDesc differentEpoch = probePass;
    ++differentEpoch.updateEpoch;
    ensure("R7 semantic identity includes scene update epoch",
           offscreenSemanticId(probePass) != offscreenSemanticId(differentEpoch));
    OffscreenPassDesc invalidLayer = probePass;
    ++invalidLayer.arrayLayer;
    ensure("R7 cube face/layer mismatch rejected",
           !validOffscreenPass(invalidLayer));
    OffscreenPassDesc recursive = probePass;
    recursive.recursionDepth = 2;
    ensure("R7 recursive offscreen render rejected",
           !validOffscreenPass(recursive));
    ensure("R7 main view may schedule a probe",
           maySpawnPass(RenderViewClass::Main, RenderViewClass::ReflectionProbe));
    ensure("R7 probe may not schedule a mirror",
           !maySpawnPass(RenderViewClass::ReflectionProbe, RenderViewClass::Mirror));

    AlphaRoutingState ppll{AlphaMethod::PPLL, true, true, false};
    for (RenderViewClass view : {
             RenderViewClass::ReflectionProbe, RenderViewClass::HeroProbe,
             RenderViewClass::Mirror, RenderViewClass::CubeSnapshot,
             RenderViewClass::Impostor, RenderViewClass::DynamicTexture,
             RenderViewClass::MediaSurface})
    {
        ensure("R7 offscreen alpha remains on the legacy path",
            routeAlphaSubmission({alphaPhaseForView(view),
                                  AlphaSubmissionClass::StandardBlend}, ppll).route ==
                AlphaRoute::LegacySorted);
    }

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R7 validation device", created.status.ok() && device);
    ensure("R7 cube-array capability is explicit",
           device->capabilities().cubeMapArrays);
    Status status = Status::success();
    ImageDesc invalidCube;
    invalidCube.extent = {32, 16, 1};
    invalidCube.format = Format::RGBA8UNorm;
    invalidCube.usage = ResourceUsage::ColorAttachment | ResourceUsage::Sampled;
    invalidCube.arrayLayers = 6;
    invalidCube.cubeCompatible = true;
    ensure("R7 nonsquare cube image rejected",
        !device->createImage(invalidCube, status) &&
        status.code() == StatusCode::InvalidArgument);
    invalidCube.extent = {32, 32, 1};
    invalidCube.arrayLayers = 7;
    ensure("R7 incomplete cube layer group rejected",
        !device->createImage(invalidCube, status) &&
        status.code() == StatusCode::InvalidArgument);

    ImageDesc cube = invalidCube;
    cube.arrayLayers = 12;
    cube.mipLevels = 3;
    cube.usage = cube.usage | ResourceUsage::TransferSource |
                 ResourceUsage::TransferDestination;
    ImageHandle image = device->createImage(cube, status);
    ensure("R7 cube-compatible array accepted", status.ok() && image);
    ImageViewHandle cubeArray = device->createImageView(
        {image, Format::RGBA8UNorm, {ImageAspect::Color, 0, 3, 0, 12},
         ImageViewType::TextureCubeArray}, status);
    ensure("R7 cube-array sampling view accepted", status.ok() && cubeArray);
    ImageViewHandle face = device->createImageView(
        {image, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 7, 1},
         ImageViewType::Texture2D}, status);
    ensure("R7 single cube-face attachment view accepted", status.ok() && face);
    ImageViewHandle invalidCubeView = device->createImageView(
        {image, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 1, 6},
         ImageViewType::TextureCube}, status);
    ensure("R7 misaligned cube sampling view rejected",
        !invalidCubeView && status.code() == StatusCode::InvalidArgument);
    ensure("R7 barrier frame begins", device->commandContext().beginFrame().ok());
    ensure("R7 color attachment dependency barrier accepted",
        device->commandContext().resourceBarrier(
            ResourceBarrier::ColorAttachmentWriteToSampledRead).ok());
    ensure("R7 barrier frame ends", device->commandContext().endFrame().ok());

#if defined(LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE)
    ShaderPackageDesc package;
    status = loadShaderPackage(LL_GHI_R7_OFFSCREEN_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());
    ensure_equals("R7 offscreen reflected binding count",
                  package.bindings.size(), std::size_t{3});
    ensure("R7 probe binding is sampled",
        package.bindings[0].type ==
            ShaderPackageDesc::BindingType::CombinedImageSampler);
    ensure("R7 media binding is sampled",
        package.bindings[2].type ==
            ShaderPackageDesc::BindingType::CombinedImageSampler);
#endif
}

template<> template<>
void LLGHIValidationObject::test<29>()
{
    using namespace LL::GHI;

    const DeviceFaultReport lost = makeDeviceFaultReport(
        Backend::Vulkan, "present", Status::failure(
            StatusCode::DeviceLost, "VK_ERROR_DEVICE_LOST"), 73);
    ensure("R8 device loss requires device recreation",
        lost.severity == DeviceFaultSeverity::DeviceRecreationRequired);
    ensure_equals("R8 device fault retains frame serial", lost.frameSerial,
                  std::uint64_t{73});
    const DeviceFaultReport pending = makeDeviceFaultReport(
        Backend::OpenGL, "snapshot", Status::failure(
            StatusCode::NotReady, "readback pending"), 74);
    ensure("R8 not-ready remains retryable",
        pending.severity == DeviceFaultSeverity::Retryable);
    const DeviceFaultReport backendFailure = makeDeviceFaultReport(
        Backend::OpenGL, "draw", Status::failure(
            StatusCode::BackendError, "GL error"), 75);
    ensure("R8 backend failure is reported as fatal",
        backendFailure.severity == DeviceFaultSeverity::Fatal);

    ProductionEligibilityEvidence evidence;
    auto eligibility = evaluateProductionEligibility(evidence);
    ensure("R8 incomplete evidence is not complete", !eligibility.evidenceComplete);
    ensure("R8 incomplete evidence cannot select production Vulkan",
        !eligibility.productionSelectable);
    ensure_equals("R8 all evidence gates start pending", eligibility.pending.size(),
        static_cast<std::size_t>(ProductionGate::Count));
    evidence.gates.fill(ProductionGateState::Pass);
    eligibility = evaluateProductionEligibility(evidence);
    ensure("R8 passing evidence is complete", eligibility.evidenceComplete);
    ensure("R8 evidence alone cannot approve production selection",
        !eligibility.productionSelectable);
    evidence.explicitProductionApproval = true;
    eligibility = evaluateProductionEligibility(evidence);
    ensure("R8 explicit approval after complete evidence permits selection",
        eligibility.productionSelectable);
    evidence.gates[static_cast<std::size_t>(
        ProductionGate::LinuxHardwareCoverage)] = ProductionGateState::Fail;
    eligibility = evaluateProductionEligibility(evidence);
    ensure("R8 failed hardware coverage revokes eligibility",
        !eligibility.evidenceComplete && !eligibility.productionSelectable);
    ensure_equals("R8 failed gate is identified", eligibility.failed.size(),
                  std::size_t{1});

    const AlphaRoutingState ppll{AlphaMethod::PPLL, true, true, false};
    ensure("R8 HUD alpha stays on the legacy path",
        routeAlphaSubmission({AlphaViewPhase::HUD,
                              AlphaSubmissionClass::StandardBlend}, ppll).route ==
            AlphaRoute::LegacySorted);

#if defined(LL_GHI_R8_INTERACTION_SHADER_PACKAGE)
    ShaderPackageDesc package;
    Status status = loadShaderPackage(
        LL_GHI_R8_INTERACTION_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());
    ensure_equals("R8 interaction reflected binding count",
                  package.bindings.size(), std::size_t{1});
    ensure_equals("R8 interaction reflected output count",
                  package.fragmentOutputs.size(), std::size_t{3});
    ensure("R8 selection ID output is unsigned integer",
        package.fragmentOutputs[1].type == ShaderValueType::UInt);
    ensure("R8 pick-depth output is floating point",
        package.fragmentOutputs[2].type == ShaderValueType::Float);

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R8 validation device", created.status.ok() && device);
    ShaderPackageHandle shader = device->createShaderPackage(package, status);
    PipelineDesc pipeline;
    pipeline.shader = shader;
    pipeline.cullMode = CullMode::None;
    pipeline.depthTest = false;
    pipeline.depthWrite = false;
    pipeline.colorFormats = {
        Format::RGBA8UNorm, Format::R32UInt, Format::R32Float};
    pipeline.blendStates.resize(3);
    pipeline.blendStates[0].enabled = true;
    pipeline.blendStates[0].sourceColor = BlendFactor::SourceAlpha;
    pipeline.blendStates[0].destinationColor = BlendFactor::OneMinusSourceAlpha;
    PipelineHandle interactionPipeline = device->createPipeline(pipeline, status);
    ensure("R8 UI color plus ID/depth pipeline accepted",
           status.ok() && interactionPipeline);
#endif
}

template<> template<>
void LLGHIValidationObject::test<30>()
{
    using namespace LL::GHI;

    OpaqueScenePacket packet;
    packet.sourceWidth = 1280;
    packet.sourceHeight = 720;
    packet.frameId = 81420;
    packet.sceneEpoch = 1;
    packet.statistics = {1, 1, 1, 1, 0, 0, 0};
    packet.vertices = {
        {{{-1.f, -1.f, 0.5f}}, {{255, 0, 0, 255}}},
        {{{ 1.f, -1.f, 0.5f}}, {{0, 255, 0, 255}}},
        {{{ 0.f,  1.f, 0.5f}}, {{0, 0, 255, 255}}},
    };
    packet.indices = {0, 1, 2};
    OpaqueSceneDraw draw;
    draw.indexCount = 3;
    draw.transform = {{
        1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}};
    draw.semanticId = 0x49315f4c495645ull; // "I1_LIVE"
    packet.draws.push_back(draw);

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("I1 validation device", created.status.ok() && device);
    OpaquePacketTransferResult transfer;
    OpaquePacketTransferLimits limits;
    Status status = consumeOpaquePacketTransfer(*device, packet, limits, transfer);
    ensure(status.message(), status.ok());
    ensure_equals("I1 transfer retains frame identity", transfer.frameId,
                  packet.frameId);
    ensure_equals("I1 transfer draw count", transfer.draws, std::uint32_t{1});
    ensure_equals("I1 transfer vertex count", transfer.vertices, std::uint32_t{3});
    ensure_equals("I1 transfer index count", transfer.indices, std::uint32_t{3});
    ensure("I1 transfer records a deterministic packet hash",
           !transfer.packetSha256.empty());
    ensure("I1 transfer emits a non-rendering semantic command stream",
           !device->semanticTrace().bytes().empty());
    ensure_equals("I1 transfer retires four temporary buffers",
                  device->pendingRetirementCount(), std::size_t{4});
    ensure("I1 transfer retirement drains", device->waitIdle().ok());
    ensure_equals("I1 transfer retirement queue is empty",
                  device->pendingRetirementCount(), std::size_t{0});

    limits.maxDraws = 0;
    status = consumeOpaquePacketTransfer(*device, packet, limits, transfer);
    ensure("I1 zero transfer limit is rejected",
           status.code() == StatusCode::InvalidArgument);
}

template<> template<>
void LLGHIValidationObject::test<31>()
{
    using namespace LL::GHI;

#if defined(LL_GHI_R4_SHADER_PACKAGE)
    ShaderPackageDesc package;
    Status status = loadShaderPackage(LL_GHI_R4_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());

    OpaqueScenePacket packet;
    packet.sourceWidth = 1280;
    packet.sourceHeight = 720;
    packet.frameId = 81421;
    packet.sceneEpoch = 2;
    packet.statistics = {1, 1, 1, 1, 0, 0, 0};
    packet.vertices = {
        {{{-1.f, -1.f, 0.5f}}, {{255, 0, 0, 255}}},
        {{{ 1.f, -1.f, 0.5f}}, {{0, 255, 0, 255}}},
        {{{ 0.f,  1.f, 0.5f}}, {{0, 0, 255, 255}}},
    };
    packet.indices = {0, 1, 2};
    OpaqueSceneDraw draw;
    draw.indexCount = 3;
    draw.transform = {{
        1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}};
    draw.semanticId = 0x49325f4c495645ull; // "I2_LIVE"
    packet.draws.push_back(draw);

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("I2 validation device", created.status.ok() && device);
    {
        OpaqueOffscreenProbe probe(*device, std::move(package));
        OpaquePacketTransferLimits limits;
        status = probe.submit(packet, limits);
        ensure(status.message(), status.ok());
        ensure("I2 has one asynchronous sample pending", probe.pending());
        status = probe.submit(packet, limits);
        ensure("I2 rejects overlapping submissions",
               status.code() == StatusCode::NotReady);
        OpaqueOffscreenProbeResult result;
        status = probe.poll(result);
        ensure(status.message(), status.ok());
        ensure("I2 sample is complete after nonblocking validation poll",
               !probe.pending());
        ensure_equals("I2 retains frame identity", result.frameId,
                      packet.frameId);
        ensure_equals("I2 draw count", result.draws, std::uint32_t{1});
        ensure("I2 retains deterministic packet identity",
               !result.packetSha256.empty());
        for (const std::string& hash : result.colorSha256)
            ensure("I2 produces an offscreen attachment hash", !hash.empty());
        ensure("I2 shutdown invalidates private resources",
               probe.shutdown().ok());
    }
    ensure("I2 deferred resources drain", device->waitIdle().ok());
    ensure_equals("I2 retirement queue is empty",
                  device->pendingRetirementCount(), std::size_t{0});
#endif
}

template<> template<>
void LLGHIValidationObject::test<32>()
{
    using namespace LL::GHI;

    ensure_equals("R5b2 canonical material vertex size",
                  sizeof(MaterialSceneVertex), std::size_t{76});
    MaterialScenePacket packet;
    packet.sourceWidth = 1280;
    packet.sourceHeight = 720;
    packet.frameId = 81422;
    packet.sceneEpoch = 3;
    packet.resourceEpoch = 1;
    MaterialResource material;
    for (std::size_t index = 0; index < material.identity.size(); ++index)
        material.identity[index] = static_cast<std::byte>(index + 1);
    material.model = MaterialModel::MetallicRoughness;
    material.alphaMode = MaterialAlphaMode::Opaque;
    material.baseColor = {{0.75f, 0.875f, 1.f, 1.f}};
    material.emissive = {{0.1f, 0.2f, 0.3f}};
    material.metallic = 0.625f;
    material.roughness = 0.8f;
    packet.materials.push_back(material);

    MaterialSceneVertex vertex;
    vertex.position = {{-0.75f, -0.75f, 0.75f}};
    vertex.texCoord = {{0.f, 0.f}};
    packet.vertices.push_back(vertex);
    vertex.position = {{0.75f, -0.75f, 0.75f}};
    vertex.texCoord = {{1.f, 0.f}};
    packet.vertices.push_back(vertex);
    vertex.position = {{0.f, 0.75f, 0.75f}};
    vertex.texCoord = {{0.5f, 1.f}};
    packet.vertices.push_back(vertex);
    packet.indices = {0, 1, 2};
    MaterialSceneDraw draw;
    draw.semanticId = 0x523562325f47454full; // "R5b2_GEO"
    draw.material = 0;
    draw.indexCount = 3;
    draw.transform[12] = 0.125f;
    draw.modelTransform[13] = -0.25f;
    packet.draws.push_back(draw);

    std::vector<std::byte> encoded;
    Status status = encodeMaterialScenePacket(packet, encoded);
    ensure(status.message(), status.ok());
    MaterialScenePacket decoded;
    status = decodeMaterialScenePacket(encoded, decoded);
    ensure(status.message(), status.ok());
    ensure("R5b2 geometry and transform round trip exactly", decoded == packet);
    ensure("R5b2 deterministic geometry packet hash",
           !materialScenePacketSha256(packet).empty());

#if defined(LL_GHI_R5A_SHADER_PACKAGE)
    ShaderPackageDesc package;
    status = loadShaderPackage(LL_GHI_R5A_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());
    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R5b2 validation device", created.status.ok() && device);
    {
        MaterialOffscreenProbe probe(*device, std::move(package));
        MaterialOffscreenProbeLimits limits;
        status = probe.submit(packet, limits);
        ensure(status.message(), status.ok());
        ensure("R5b2 asynchronous geometry sample is pending", probe.pending());
        MaterialOffscreenProbeResult result;
        status = probe.poll(result);
        ensure(status.message(), status.ok());
        ensure_equals("R5b2 executed draw count", result.draws,
                      std::uint32_t{1});
        ensure_equals("R5b2 retained vertex count", result.vertices,
                      std::uint32_t{3});
        ensure_equals("R5b2 retained index count", result.indices,
                      std::uint32_t{3});
        ensure("R5b2 material probe shuts down", probe.shutdown().ok());
    }
    ensure("R5b2 deferred resources drain", device->waitIdle().ok());
#endif

    packet.indices[2] = 3;
    ensure("R5b2 rejects an out-of-range vertex index",
           encodeMaterialScenePacket(packet, encoded).code() ==
               StatusCode::InvalidArgument);
    packet.indices[2] = 2;
    packet.draws[0].firstIndex = 2;
    packet.draws[0].indexCount = 2;
    ensure("R5b2 rejects an out-of-range draw span",
           encodeMaterialScenePacket(packet, encoded).code() ==
               StatusCode::InvalidArgument);
}

} // namespace tut
