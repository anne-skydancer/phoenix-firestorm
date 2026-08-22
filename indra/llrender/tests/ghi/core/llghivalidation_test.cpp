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
#include "ghi/core/llghihash.h"
#include "ghi/core/llghipipelinecache.h"
#include "ghi/core/llghishaderpackage.h"
#include "ghi/core/llghivalidation.h"
#include "ghi/include/llghienvironmentscenepacket.h"
#include "ghi/include/llghimaterialscenepacket.h"
#include "ghi/include/llghimaterialoffscreenprobe.h"
#include "ghi/include/llghinestedviewscenepacket.h"
#include "ghi/include/llghiproductionframeconsumer.h"
#include "ghi/include/llghiproductionframepacket.h"
#include "ghi/include/llghiproductionframetargets.h"
#include "ghi/include/llghiproductiongbufferexecutor.h"
#include "ghi/include/llghiproductionlightingexecutor.h"
#include "ghi/include/llghiproductiontextureresidency.h"
#include "ghi/include/llghiproductionwaterresources.h"
#include "ghi/include/llghilightingscenepacket.h"
#include "ghi/include/llghilightingpacketconsumer.h"
#include "ghi/include/llghiterrainscenepacket.h"
#include "ghi/include/llghiterrainoffscreenprobe.h"
#include "ghi/include/llghialphacontract.h"
#include "ghi/include/llghialphascenepacket.h"
#include "ghi/include/llghiproductionalphaexecutor.h"
#include "ghi/include/llghioffscreencontract.h"
#include "ghi/include/llghiopaqueoffscreenprobe.h"
#include "ghi/include/llghiopaquepacketconsumer.h"

#include <limits>
#include "ghi/include/llghiopaquescenepacket.h"
#include "ghi/include/llghirendererinfo.h"
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
    changedPipeline = pipeline;
    changedPipeline.polygonMode = PolygonMode::Line;
    ensure("polygon mode invalidates pipeline cache",
        identity != pipelineCacheIdentity(package, changedPipeline,
            ShaderPackageDesc::TargetProfile::OpenGL44,
            Backend::OpenGL, domain));
    changedPipeline = pipeline;
    changedPipeline.depthBias = true;
    changedPipeline.depthBiasConstantFactor = 2.f;
    changedPipeline.depthBiasSlopeFactor = 1.f;
    ensure("depth bias invalidates pipeline cache",
        identity != pipelineCacheIdentity(package, changedPipeline,
            ShaderPackageDesc::TargetProfile::OpenGL44,
            Backend::OpenGL, domain));
    changedPipeline = pipeline;
    changedPipeline.lineWidth = 2.f;
    ensure("line width invalidates pipeline cache",
        identity != pipelineCacheIdentity(package, changedPipeline,
            ShaderPackageDesc::TargetProfile::OpenGL44,
            Backend::OpenGL, domain));
    changedPipeline = pipeline;
    changedPipeline.depthBiasConstantFactor = -0.f;
    ensure_equals("signed zero has one semantic pipeline identity", identity,
        pipelineCacheIdentity(package, changedPipeline,
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
    ensure_equals("I5 reflected binding count", package.bindings.size(), std::size_t{8});
    ensure_equals("R5a reflected vertex input count",
                  package.vertexInputs.size(), std::size_t{7});
    ensure_equals("R5a reflected deferred output count",
                  package.fragmentOutputs.size(), std::size_t{4});

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("R5a validation device", created.status.ok() && device);
    ShaderPackageHandle shader = device->createShaderPackage(package, status);
    ensure("R5a shader package", status.ok() && shader);
    BufferHandle object = device->createBuffer(
        {128, ResourceUsage::Uniform, MemoryClass::DeviceLocal}, status);
    BufferHandle skin = device->createBuffer(
        {MATERIAL_SKIN_BYTES, ResourceUsage::Uniform, MemoryClass::DeviceLocal}, status);
    BufferHandle material = device->createBuffer(
        {176, ResourceUsage::Uniform, MemoryClass::DeviceLocal}, status);
    ImageHandle image = device->createImage(
        {{2, 2, 1}, Format::RGBA8UNorm, ResourceUsage::Sampled, 1, 1, 1}, status);
    ImageViewHandle view = device->createImageView(
        {image, Format::RGBA8UNorm, {ImageAspect::Color, 0, 1, 0, 1}}, status);
    SamplerHandle sampler = device->createSampler({}, status);
    ensure("I5 object, production skin and material resources",
           status.ok() && object && skin && material && image && view && sampler);

    BindingSetDesc objectSet;
    objectSet.shader = shader;
    objectSet.group = 1;
    objectSet.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         object, 0, 128, {}, {}});
    BindingSetHandle missingSkin = device->createBindingSet(objectSet, status);
    ensure("I5 object binding without the separate skin binding is rejected",
           !missingSkin && status.code() == StatusCode::InvalidArgument);
    objectSet.resources.push_back(
        {1, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         skin, 0, MATERIAL_SKIN_BYTES, {}, {}});
    BindingSetHandle completeObject = device->createBindingSet(objectSet, status);
    ensure("I5 separate object and production skin bindings are accepted",
           status.ok() && completeObject);

    BindingSetDesc materialSet;
    materialSet.shader = shader;
    materialSet.group = 2;
    materialSet.resources.push_back(
        {0, 0, ShaderPackageDesc::BindingType::UniformBuffer,
         material, 0, 176, {}, {}});
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

    source.skins[0].jointCount = MATERIAL_MAX_JOINTS + 1;
    source.skins[0].matrixPalette.resize(
        static_cast<std::size_t>(source.skins[0].jointCount) * 12);
    ensure("material packet rejects palettes above the production joint ceiling",
           encodeMaterialScenePacket(source, first).code() ==
               StatusCode::InvalidArgument);
    source.skins[0] = skin;
    source.skins[0].jointCount = 0;
    source.skins[0].matrixPalette.clear();
    ensure("material packet rejects an empty referenced skin palette",
           encodeMaterialScenePacket(source, first).code() ==
               StatusCode::InvalidArgument);
    source.skins[0] = skin;

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
             RenderViewClass::Preview, RenderViewClass::PreWaterAlpha,
             RenderViewClass::MediaSurface})
    {
        ensure("R7 offscreen alpha remains on the legacy path",
            routeAlphaSubmission({alphaPhaseForView(view),
                                  AlphaSubmissionClass::StandardBlend}, ppll).route ==
                AlphaRoute::LegacySorted);
    }

            NestedViewScenePacket nestedPacket;
            nestedPacket.frameId = 91;
            nestedPacket.sceneGeneration = 17;
            nestedPacket.resourceGeneration = 23;
            for (std::uint8_t faceIndex = 0; faceIndex < 6; ++faceIndex)
            {
             NestedViewPass nested;
             nested.resourceGeneration = nestedPacket.resourceGeneration;
             nested.pass.view = RenderViewClass::ReflectionProbe;
             nested.pass.recursionDepth = 1;
             nested.pass.face = static_cast<CubeFace>(faceIndex);
             nested.pass.probePhase = ProbePhase::DirectLighting;
             nested.pass.arrayLayer = static_cast<std::uint16_t>(6 + faceIndex);
             nested.pass.updateEpoch = nestedPacket.sceneGeneration;
             nested.semanticId = offscreenSemanticId(nested.pass);
             nestedPacket.passes.push_back(nested);
            }
            for (std::uint8_t faceIndex = 0; faceIndex < 6; ++faceIndex)
            {
             NestedViewPass nested;
             nested.resourceGeneration = 27;
             nested.pass.view = RenderViewClass::CubeSnapshot;
             nested.pass.recursionDepth = 1;
             nested.pass.face = static_cast<CubeFace>(faceIndex);
             nested.pass.arrayLayer = faceIndex;
             nested.pass.updateEpoch = nestedPacket.sceneGeneration;
             nested.semanticId = offscreenSemanticId(nested.pass);
             nestedPacket.passes.push_back(nested);
            }
            NestedViewPass impostor;
            impostor.resourceGeneration = 28;
            impostor.pass.view = RenderViewClass::Impostor;
            impostor.pass.recursionDepth = 1;
            impostor.pass.updateEpoch = nestedPacket.sceneGeneration;
            impostor.semanticId = offscreenSemanticId(impostor.pass);
            nestedPacket.passes.push_back(impostor);
            NestedViewPass dynamicTexture;
            dynamicTexture.resourceGeneration = 29;
            dynamicTexture.pass.view = RenderViewClass::DynamicTexture;
            dynamicTexture.pass.recursionDepth = 1;
            dynamicTexture.pass.updateEpoch = nestedPacket.sceneGeneration;
            dynamicTexture.semanticId = offscreenSemanticId(dynamicTexture.pass);
            nestedPacket.passes.push_back(dynamicTexture);
            NestedViewPass preview;
            preview.resourceGeneration = 31;
            preview.pass.view = RenderViewClass::Preview;
            preview.pass.recursionDepth = 1;
            preview.pass.updateEpoch = nestedPacket.sceneGeneration;
            preview.semanticId = offscreenSemanticId(preview.pass);
            nestedPacket.passes.push_back(preview);
            NestedViewPass preWaterAlpha;
            preWaterAlpha.resourceGeneration = 37;
            preWaterAlpha.pass.view = RenderViewClass::PreWaterAlpha;
            preWaterAlpha.pass.recursionDepth = 1;
            preWaterAlpha.pass.updateEpoch = nestedPacket.sceneGeneration;
            preWaterAlpha.semanticId = offscreenSemanticId(preWaterAlpha.pass);
            nestedPacket.passes.push_back(preWaterAlpha);
            NestedViewPass mediaSurface;
            mediaSurface.resourceGeneration = 41;
            mediaSurface.pass.view = RenderViewClass::MediaSurface;
            mediaSurface.pass.recursionDepth = 1;
            mediaSurface.pass.updateEpoch = nestedPacket.sceneGeneration;
            mediaSurface.semanticId = offscreenSemanticId(mediaSurface.pass);
            nestedPacket.passes.push_back(mediaSurface);
            ensure("P0e4 nested-view schedule accepted",
                validateNestedViewScenePacket(nestedPacket).ok());
            std::vector<std::byte> nestedFirst;
            std::vector<std::byte> nestedSecond;
            ensure("P0e4 nested-view schedule encodes",
                encodeNestedViewScenePacket(nestedPacket, nestedFirst).ok());
            ensure("P0e4 nested-view encoding is deterministic",
                encodeNestedViewScenePacket(nestedPacket, nestedSecond).ok() &&
                 nestedFirst == nestedSecond);
            NestedViewScenePacket nestedDecoded;
            ensure("P0e4 nested-view schedule round trips",
                decodeNestedViewScenePacket(nestedFirst, nestedDecoded).ok() &&
                 nestedDecoded == nestedPacket);
            ensure("P0e4 nested-view packet has a stable digest",
                !nestedViewScenePacketSha256(nestedPacket).empty());

            NestedViewScenePacket invalidNested = nestedPacket;
            invalidNested.passes[13].resourceGeneration = 0;
            ensure("P0e4 zero resource generation rejected",
                !validateNestedViewScenePacket(invalidNested));
            invalidNested = nestedPacket;
            ++invalidNested.passes[0].pass.updateEpoch;
            invalidNested.passes[0].semanticId =
             offscreenSemanticId(invalidNested.passes[0].pass);
            ensure("P0e4 stale scene generation rejected",
                !validateNestedViewScenePacket(invalidNested));
            invalidNested = nestedPacket;
            invalidNested.passes[13].pass.recursionDepth = 2;
            invalidNested.passes[13].semanticId =
             offscreenSemanticId(invalidNested.passes[13].pass);
            ensure("P0e4 recursive nested view rejected",
                !validateNestedViewScenePacket(invalidNested));
            invalidNested = nestedPacket;
            invalidNested.passes[1].semanticId = invalidNested.passes[0].semanticId;
            ensure("P0e4 duplicate semantic identity rejected",
                !validateNestedViewScenePacket(invalidNested));
            invalidNested = nestedPacket;
            ++invalidNested.passes[3].pass.arrayLayer;
            invalidNested.passes[3].semanticId =
             offscreenSemanticId(invalidNested.passes[3].pass);
            ensure("P0e4 cube face/layer mismatch rejected",
                !validateNestedViewScenePacket(invalidNested));
            invalidNested = nestedPacket;
            invalidNested.passes.erase(invalidNested.passes.begin() + 5);
            ensure("P0e4 incomplete cube face group rejected",
                !validateNestedViewScenePacket(invalidNested));
            invalidNested = nestedPacket;
            std::swap(invalidNested.passes[0], invalidNested.passes[1]);
            ensure("P0e4 noncanonical cube face order rejected",
                !validateNestedViewScenePacket(invalidNested));

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
    MaterialTextureResource texture;
    for (std::size_t index = 0; index < texture.sourceIdentity.size(); ++index)
    {
        texture.sourceIdentity[index] = static_cast<std::byte>(index + 3);
        texture.contentIdentity[index] = static_cast<std::byte>(index + 7);
    }
    texture.colorSpace = TextureColorSpace::SRGB;
    texture.width = texture.height = 1;
    texture.components = 4;
    texture.decodedPixels = {
        std::byte{192}, std::byte{224}, std::byte{255}, std::byte{255}};
    packet.textures.push_back(texture);
    MaterialResource material;
    for (std::size_t index = 0; index < material.identity.size(); ++index)
        material.identity[index] = static_cast<std::byte>(index + 1);
    material.model = MaterialModel::MetallicRoughness;
    material.alphaMode = MaterialAlphaMode::Opaque;
    material.baseColor = {{0.75f, 0.875f, 1.f, 1.f}};
    material.emissive = {{0.1f, 0.2f, 0.3f}};
    material.metallic = 0.625f;
    material.roughness = 0.8f;
    material.textures.push_back({
        TextureSemantic::BaseColor, 0, 0,
        {{0.125f, -0.25f, 0.75f, 1.25f, 0.5f}}});
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
    draw.modelTransform[0] = 0.75f;
    draw.modelTransform[5] = 1.25f;
    draw.modelTransform[10] = 0.5f;
    draw.modelTransform[13] = -0.25f;
    packet.draws.push_back(draw);

    SkinResource productionSkin;
    for (std::size_t index = 0; index < productionSkin.identity.size(); ++index)
        productionSkin.identity[index] = static_cast<std::byte>(index + 97);
    productionSkin.jointCount = MATERIAL_MAX_JOINTS;
    productionSkin.matrixPalette.resize(MATERIAL_MAX_JOINTS * 12);
    for (std::uint32_t joint = 0; joint < MATERIAL_MAX_JOINTS; ++joint)
    {
        productionSkin.matrixPalette[joint * 12] = 1.f;
        productionSkin.matrixPalette[joint * 12 + 5] = 1.f;
        productionSkin.matrixPalette[joint * 12 + 10] = 1.f;
    }
    productionSkin.matrixPalette[(MATERIAL_MAX_JOINTS - 1) * 12 + 3] = .125f;
    packet.skins.push_back(productionSkin);
    for (auto& capturedVertex : packet.vertices)
        capturedVertex.joints[0] = MATERIAL_MAX_JOINTS - 1;
    MaterialSceneDraw riggedDraw = draw;
    riggedDraw.semanticId = 0x49355f5249474744ull; // "I5_RIGGD"
    riggedDraw.skin = 0;
    packet.draws.push_back(riggedDraw);

    std::vector<std::byte> encoded;
    Status status = encodeMaterialScenePacket(packet, encoded);
    ensure(status.message(), status.ok());
    MaterialScenePacket decoded;
    status = decodeMaterialScenePacket(encoded, decoded);
    ensure(status.message(), status.ok());
    ensure("I4 geometry, model and texture transforms round trip exactly",
           decoded == packet);
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
        ensure_equals("I5 executed rigid plus rigged draw count", result.draws,
                      std::uint32_t{2});
        ensure_equals("I5 executed rigged draw count", result.riggedDraws,
                      std::uint32_t{1});
        ensure_equals("I5 retained production palette size", result.maxJointCount,
                      MATERIAL_MAX_JOINTS);
        ensure_equals("R5b2 retained vertex count", result.vertices,
                      std::uint32_t{3});
        ensure_equals("R5b2 retained index count", result.indices,
                      std::uint32_t{3});
        limits.maxDraws = 1;
        status = probe.submit(packet, limits);
        ensure(status.message(), status.ok());
        status = probe.poll(result);
        ensure(status.message(), status.ok());
        ensure_equals("I5 rigged draw has priority under a one-draw budget",
                      result.riggedDraws, std::uint32_t{1});
        limits.maxDraws = 32;
        packet.draws[0].modelTransform[0] = 0.f;
        status = probe.submit(packet, limits);
        ensure("I4 rejects a singular live model transform",
               status.code() == StatusCode::InvalidArgument);
        packet.draws[0].modelTransform[0] = 0.75f;
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

template<> template<>
void LLGHIValidationObject::test<33>()
{
    using namespace LL::GHI;

    auto digest = [](std::uint8_t seed)
    {
        ResourceDigest value{};
        for (std::size_t index = 0; index < value.size(); ++index)
            value[index] = static_cast<std::byte>(seed + index);
        return value;
    };

    TerrainScenePacket source;
    source.frameId = 81423;
    source.sceneEpoch = 4;
    source.resourceEpoch = 2;
    source.sourceWidth = 2560;
    source.sourceHeight = 1369;
    for (std::uint8_t textureIndex = 0; textureIndex < 6; ++textureIndex)
    {
        MaterialTextureResource texture;
        texture.sourceIdentity = digest(static_cast<std::uint8_t>(1 + textureIndex));
        texture.contentIdentity = digest(static_cast<std::uint8_t>(33 + textureIndex));
        texture.colorSpace = textureIndex == 0 || textureIndex == 5
            ? TextureColorSpace::Linear : TextureColorSpace::SRGB;
        texture.width = texture.height = 1;
        texture.components = 4;
        texture.decodedPixels = {
            static_cast<std::byte>(32 + textureIndex),
            static_cast<std::byte>(64 + textureIndex),
            static_cast<std::byte>(96 + textureIndex), std::byte{255}};
        source.textures.push_back(std::move(texture));
    }

    TerrainRegionResource region;
    region.identity = digest(65);
    region.model = MaterialModel::MetallicRoughness;
    region.paintMode = TerrainPaintMode::PBRPaintMap;
    region.projection = TerrainProjection::Triplanar;
    region.detailMode = TerrainDetailMode::Normal;
    region.regionScale = 256.f;
    region.detailScale = 0.125f;
    region.compositionTexture = 0;
    for (std::size_t layerIndex = 0; layerIndex < region.layers.size(); ++layerIndex)
    {
        auto& layer = region.layers[layerIndex];
        layer.identity = digest(static_cast<std::uint8_t>(97 + layerIndex));
        layer.model = MaterialModel::MetallicRoughness;
        layer.baseColor = {{0.25f * static_cast<float>(layerIndex + 1),
                            0.5f, 0.75f, 1.f}};
        layer.emissive = {{0.01f * static_cast<float>(layerIndex), 0.02f, 0.03f}};
        layer.metallic = 0.1f * static_cast<float>(layerIndex);
        layer.roughness = 0.9f - 0.1f * static_cast<float>(layerIndex);
        layer.alphaCutoff = 0.25f;
        layer.transform = {{0.1f * static_cast<float>(layerIndex), -0.25f,
                            2.f, 2.f, 0.125f}};
        layer.baseColorTexture = static_cast<std::uint32_t>(layerIndex + 1);
        layer.normalTexture = 5;
    }
    source.regions.push_back(region);

    TerrainSceneVertex vertex;
    vertex.position = {{0.f, 0.f, 2.f}};
    vertex.compositionCoord = {{0.5f, 0.25f}};
    source.vertices.push_back(vertex);
    vertex.position = {{16.f, 0.f, 3.f}};
    vertex.compositionCoord = {{1.5f, 0.25f}};
    source.vertices.push_back(vertex);
    vertex.position = {{0.f, 16.f, 4.f}};
    vertex.compositionCoord = {{0.5f, 1.25f}};
    source.vertices.push_back(vertex);
    source.indices = {0, 1, 2};
    TerrainSceneDraw draw;
    draw.semanticId = 0x49365f544552524eull; // "I6_TERRN"
    draw.region = 0;
    draw.indexCount = 3;
    draw.viewProjection[12] = 0.125f;
    draw.modelTransform[12] = 1024.f;
    draw.modelTransform[13] = 768.f;
    source.draws.push_back(draw);

    std::vector<std::byte> first;
    std::vector<std::byte> second;
    Status status = encodeTerrainScenePacket(source, first);
    ensure(status.message(), status.ok());
    ensure("I6 terrain encoding is deterministic",
           encodeTerrainScenePacket(source, second).ok() && first == second);
    TerrainScenePacket decoded;
    status = decodeTerrainScenePacket(first, decoded);
    ensure(status.message(), status.ok());
    ensure("I6 terrain packet round trips exactly", decoded == source);
    ensure_equals("I6 canonical terrain vertex size",
                  sizeof(TerrainSceneVertex), std::size_t{48});
    ensure("I6 terrain packet has a deterministic identity",
           !terrainScenePacketSha256(source).empty());

#if defined(LL_GHI_I6_TERRAIN_SHADER_PACKAGE)
    ShaderPackageDesc package;
    status = loadShaderPackage(LL_GHI_I6_TERRAIN_SHADER_PACKAGE, package);
    ensure(status.message(), status.ok());
    ensure_equals("I6 reflected terrain binding count",
                  package.bindings.size(), std::size_t{6});
    ensure_equals("I6 reflected canonical terrain input count",
                  package.vertexInputs.size(), std::size_t{3});
    ensure_equals("I6 reflected terrain output count",
                  package.fragmentOutputs.size(), std::size_t{4});
    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    ensure("I6 validation device", created.status.ok() && created.device);
    {
        TerrainOffscreenProbe probe(*created.device, std::move(package));
        TerrainOffscreenProbeLimits limits;
        status = probe.submit(source, limits);
        ensure(status.message(), status.ok());
        ensure("I6 asynchronous terrain sample is pending", probe.pending());
        TerrainOffscreenProbeResult result;
        status = probe.poll(result);
        ensure(status.message(), status.ok());
        ensure_equals("I6 executed terrain draw count", result.draws,
                      std::uint32_t{1});
        ensure_equals("I6 executed PBR terrain draw count", result.pbrDraws,
                      std::uint32_t{1});
        ensure_equals("I6 executed triplanar terrain draw count",
                      result.triplanarDraws, std::uint32_t{1});
        ensure("I6 terrain probe shuts down", probe.shutdown().ok());
    }
    ensure("I6 deferred resources drain", created.device->waitIdle().ok());
#endif

    source.indices[2] = 3;
    ensure("I6 rejects an out-of-range terrain index",
           encodeTerrainScenePacket(source, first).code() ==
               StatusCode::InvalidArgument);
    source.indices[2] = 2;
    source.regions[0].compositionTexture = NO_RESOURCE;
    ensure("I6 requires an explicit composition resource",
           encodeTerrainScenePacket(source, first).code() ==
               StatusCode::InvalidArgument);
    source.regions[0].compositionTexture = 0;
    source.regions[0].model = MaterialModel::Legacy;
    ensure("I6 rejects mixed legacy and PBR terrain state",
           encodeTerrainScenePacket(source, first).code() ==
               StatusCode::InvalidArgument);
    source.regions[0].model = MaterialModel::MetallicRoughness;
    first.pop_back();
    ensure("I6 rejects a truncated terrain packet",
           decodeTerrainScenePacket(first, decoded).code() ==
               StatusCode::InvalidArgument);
}

template<> template<>
void LLGHIValidationObject::test<34>()
{
    using namespace LL::GHI;

    LightingScenePacket source;
    source.frameId = 81426;
    source.sceneEpoch = 7;
    source.resourceEpoch = 3;
    source.sourceWidth = 2560;
    source.sourceHeight = 1369;
    for (std::size_t diagonal = 0; diagonal < 4; ++diagonal)
    {
        source.viewMatrix[diagonal * 5] = 1.f;
        source.projectionMatrix[diagonal * 5] = 1.f;
    }
    source.cameraOrigin = {{128.f, 64.f, 32.f}};
    source.ambientColor = {{0.08f, 0.10f, 0.14f}};
    source.sun.direction = {{0.25f, 0.5f, -0.75f}};
    source.sun.color = {{1.f, 0.9f, 0.75f}};
    source.sun.active = true;
    source.moon.direction = {{-0.25f, -0.5f, 0.75f}};
    source.moon.color = {{0.2f, 0.3f, 0.6f}};

    source.shadows.enabled = true;
    source.shadows.directionalCascadeCount = 4;
    source.shadows.projectorShadowCount = 1;
    source.shadows.comparability =
        LightingComparability::ShadowImagesDeferred;
    for (auto& matrix : source.shadows.matrices)
        for (std::size_t diagonal = 0; diagonal < 4; ++diagonal)
            matrix[diagonal * 5] = 1.f;
    source.shadows.clipPlanes = {{16.f, 64.f, 128.f, 256.f}};
    source.shadows.directionalBias = 0.002f;
    source.shadows.spotShadowOffset = 0.001f;
    source.shadows.spotShadowBias = 0.003f;
    source.shadows.projectorLightIds[0] = 0x4937415f50524f4aull;
    source.shadows.projectorFade[0] = 0.875f;

    LocalLightRecord point;
    point.semanticId = 0x4937415f504f494eull;
    point.position = {{124.f, 70.f, 35.f}};
    point.radius = 12.f;
    point.color = {{0.8f, 0.4f, 0.2f}};
    point.falloff = 0.5f;
    source.localLights.push_back(point);

    LocalLightRecord projector;
    projector.semanticId = source.shadows.projectorLightIds[0];
    projector.kind = LocalLightKind::Projector;
    projector.comparability =
        LightingComparability::Comparable;
    projector.position = {{136.f, 72.f, 40.f}};
    projector.radius = 24.f;
    projector.color = {{0.2f, 0.5f, 1.f}};
    projector.falloff = 0.75f;
    projector.projectorParams = {{1.1f, 0.4f, 0.125f}};
    projector.projectorTextureIdentity[0] = 17;
    projector.projectorTextureIdentity[15] = 91;
    projector.shadowSlot = 0;
    projector.shadowFade = 0.875f;
    source.localLights.push_back(projector);
    ProjectorTextureResource projectorTexture;
    projectorTexture.sourceIdentity = projector.projectorTextureIdentity;
    projectorTexture.width = projectorTexture.height = 2;
    projectorTexture.components = 4;
    projectorTexture.decodedPixels = {
        std::byte{255}, std::byte{64}, std::byte{32}, std::byte{255},
        std::byte{32}, std::byte{255}, std::byte{64}, std::byte{255},
        std::byte{64}, std::byte{32}, std::byte{255}, std::byte{255},
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    const std::string projectorDigest = sha256(
        projectorTexture.decodedPixels);
    const auto nibble = [](char value) -> std::uint8_t
    {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return value - 'A' + 10;
    };
    for (std::size_t index = 0;
         index < projectorTexture.contentIdentity.size(); ++index)
        projectorTexture.contentIdentity[index] = static_cast<std::byte>(
            (nibble(projectorDigest[index * 2]) << 4) |
             nibble(projectorDigest[index * 2 + 1]));
    source.projectorTextures.push_back(projectorTexture);

    std::vector<std::byte> first;
    std::vector<std::byte> second;
    Status status = encodeLightingScenePacket(source, first);
    ensure(status.message(), status.ok());
    ensure("I7a lighting encoding is deterministic",
           encodeLightingScenePacket(source, second).ok() && first == second);
    LightingScenePacket decoded;
    status = decodeLightingScenePacket(first, decoded);
    ensure(status.message(), status.ok());
    ensure("I7a lighting packet round trips exactly", decoded == source);
    ensure("I7a lighting packet has deterministic identity",
           !lightingScenePacketSha256(source).empty());
    LightingScenePacket missingProjectorImage = source;
    missingProjectorImage.projectorTextures.clear();
    ensure("I7c comparable projector requires its decoded image",
           encodeLightingScenePacket(missingProjectorImage, second).code() ==
               StatusCode::InvalidArgument);
    LightingScenePacket corruptProjectorImage = source;
    corruptProjectorImage.projectorTextures[0].decodedPixels[0] ^=
        std::byte{1};
    ensure("I7c rejects decoded projector bytes that do not match their hash",
           encodeLightingScenePacket(corruptProjectorImage, second).code() ==
               StatusCode::InvalidArgument);

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    ensure("I7a validation device", created.status.ok() && created.device);
    LightingPacketTransferResult result;
    LightingPacketTransferLimits limits;
    status = consumeLightingPacketTransfer(
        *created.device, source, limits, result);
    ensure(status.message(), status.ok());
    ensure_equals("I7a transfers both local lights", result.localLights,
                  std::uint32_t{2});
    ensure_equals("I7a identifies the projector", result.projectorLights,
                  std::uint32_t{1});
    ensure_equals("I7a retains the cascade count", result.shadowCascades,
                  std::uint32_t{4});
    ensure("I7a transfer retires cleanly", created.device->waitIdle().ok());

    limits.maxLocalLights = 1;
    ensure("I7a rejects transfer above the runtime light limit",
           consumeLightingPacketTransfer(*created.device, source, limits, result)
                   .code() == StatusCode::InvalidArgument);
    limits.maxLocalLights = 256;
    source.localLights[0].radius = 0.f;
    ensure("I7a rejects a nonpositive light radius",
           encodeLightingScenePacket(source, first).code() ==
               StatusCode::InvalidArgument);
    source.localLights[0].radius = 12.f;
    first.pop_back();
    ensure("I7a rejects a truncated lighting packet",
           decodeLightingScenePacket(first, decoded).code() ==
               StatusCode::InvalidArgument);
}

template<> template<>
void LLGHIValidationObject::test<35>()
{
    using namespace LL::GHI;

#if defined(LL_GHI_R5A_SHADER_PACKAGE) && \
    defined(LL_GHI_I7_LIGHTING_SHADER_PACKAGE)
    ShaderPackageDesc materialPackage;
    Status status = loadShaderPackage(
        LL_GHI_R5A_SHADER_PACKAGE, materialPackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc lightingPackage;
    status = loadShaderPackage(
        LL_GHI_I7_LIGHTING_SHADER_PACKAGE, lightingPackage);
    ensure(status.message(), status.ok());
    ensure_equals("I7b reflected lighting bindings",
                  lightingPackage.bindings.size(), std::size_t{10});
    ensure_equals("I7b fullscreen shader has no vertex inputs",
                  lightingPackage.vertexInputs.size(), std::size_t{0});
    ensure_equals("I7b reflected lighting output",
                  lightingPackage.fragmentOutputs.size(), std::size_t{1});

    MaterialScenePacket material;
    material.frameId = 81427;
    material.sceneEpoch = 11;
    material.resourceEpoch = 5;
    material.sourceWidth = 1280;
    material.sourceHeight = 720;
    MaterialResource pbr;
    pbr.model = MaterialModel::MetallicRoughness;
    pbr.alphaMode = MaterialAlphaMode::Opaque;
    pbr.baseColor = {{0.8f, 0.6f, 0.4f, 1.f}};
    pbr.emissive = {{0.01f, 0.02f, 0.03f}};
    pbr.metallic = 0.25f;
    pbr.roughness = 0.625f;
    material.materials.push_back(pbr);
    MaterialSceneVertex vertex;
    vertex.position = {{-0.75f, -0.75f, 0.5f}};
    material.vertices.push_back(vertex);
    vertex.position = {{0.75f, -0.75f, 0.5f}};
    material.vertices.push_back(vertex);
    vertex.position = {{0.f, 0.75f, 0.5f}};
    material.vertices.push_back(vertex);
    material.indices = {0, 1, 2};
    MaterialSceneDraw draw;
    draw.semanticId = 0x4937625f47425546ull; // "I7b_GBUF"
    draw.material = 0;
    draw.indexCount = 3;
    material.draws.push_back(draw);

    LightingScenePacket lighting;
    lighting.frameId = material.frameId;
    lighting.sceneEpoch = material.sceneEpoch;
    lighting.resourceEpoch = material.resourceEpoch;
    lighting.sourceWidth = material.sourceWidth;
    lighting.sourceHeight = material.sourceHeight;
    for (std::size_t diagonal = 0; diagonal < 4; ++diagonal)
    {
        lighting.viewMatrix[diagonal * 5] = 1.f;
        lighting.projectionMatrix[diagonal * 5] = 1.f;
    }
    lighting.cameraOrigin = {{0.f, 0.f, 2.f}};
    lighting.ambientColor = {{0.05f, 0.075f, 0.1f}};
    lighting.sun.active = true;
    lighting.sun.direction = {{0.f, 0.f, -1.f}};
    lighting.sun.color = {{1.f, 0.9f, 0.8f}};
    LocalLightRecord point;
    point.semanticId = 0x4937625f504f494eull; // "I7b_POIN"
    point.position = {{0.f, 0.f, 1.f}};
    point.radius = 4.f;
    point.color = {{0.2f, 0.5f, 1.f}};
    point.falloff = 0.5f;
    lighting.localLights.push_back(point);
    LocalLightRecord deferredProjector = point;
    deferredProjector.semanticId = 0x4937625f50524f4aull;
    deferredProjector.kind = LocalLightKind::Projector;
    deferredProjector.comparability =
        LightingComparability::ProjectorImageDeferred;
    deferredProjector.projectorTextureIdentity[0] = 1;
    lighting.localLights.push_back(deferredProjector);

    DeviceCreationResult created = createDevice(
        {Backend::Validation, 0, 2, true});
    ensure("I7b validation device", created.status.ok() && created.device);
    {
        MaterialOffscreenProbe probe(*created.device,
            std::move(materialPackage), std::move(lightingPackage));
        MaterialOffscreenProbeLimits limits;
        LightingScenePacket mismatched = lighting;
        ++mismatched.frameId;
        ensure("I7b rejects mismatched frame pairing",
            probe.submit(material, mismatched, limits).code() ==
                StatusCode::InvalidArgument);
        status = probe.submit(material, lighting, limits);
        ensure(status.message(), status.ok());
        ensure("I7b composite sample is asynchronous", probe.pending());
        MaterialOffscreenProbeResult result;
        status = probe.poll(result);
        ensure(status.message(), status.ok());
        ensure("I7b executed the deferred-lighting pass",
               result.lightingExecuted);
        ensure_equals("I7b executed one directional light",
                      result.directionalLights, std::uint32_t{1});
        ensure_equals("I7b executes point lights but defers projectors",
                      result.pointLights, std::uint32_t{1});
        ensure("I7b records the lighting packet identity",
               !result.lightingPacketSha256.empty());
        ensure("I7b records the lit target identity",
               !result.litColorSha256.empty());
        ensure("I7b composite probe shuts down", probe.shutdown().ok());
    }
#if defined(LL_GHI_I7_PROJECTOR_SHADER_PACKAGE)
    ShaderPackageDesc projectorMaterialPackage;
    status = loadShaderPackage(
        LL_GHI_R5A_SHADER_PACKAGE, projectorMaterialPackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc projectorBaseLightingPackage;
    status = loadShaderPackage(
        LL_GHI_I7_LIGHTING_SHADER_PACKAGE, projectorBaseLightingPackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc projectorPackage;
    status = loadShaderPackage(
        LL_GHI_I7_PROJECTOR_SHADER_PACKAGE, projectorPackage);
    ensure(status.message(), status.ok());
    ensure_equals("I7c reflected projector bindings",
                  projectorPackage.bindings.size(), std::size_t{7});
    ensure_equals("I7c fullscreen shader has no vertex inputs",
                  projectorPackage.vertexInputs.size(), std::size_t{0});

    LightingScenePacket projectorLighting = lighting;
    LocalLightRecord& executableProjector =
        projectorLighting.localLights.back();
    executableProjector.comparability =
        LightingComparability::Comparable;
    executableProjector.position = {{0.f, 0.f, 1.f}};
    executableProjector.radius = 4.f;
    executableProjector.scale = {{2.f, 2.f, 1.f}};
    executableProjector.projectorParams = {{1.f, 0.25f, 0.1f}};
    LocalLightRecord volumeProjector = executableProjector;
    ++volumeProjector.semanticId;
    volumeProjector.position = {{0.f, 0.f, 0.f}};
    volumeProjector.radius = 1.f;
    projectorLighting.localLights.push_back(volumeProjector);
    ProjectorTextureResource image;
    image.sourceIdentity = executableProjector.projectorTextureIdentity;
    image.width = image.height = 2;
    image.components = 4;
    image.decodedPixels = {
        std::byte{255}, std::byte{128}, std::byte{32}, std::byte{255},
        std::byte{32}, std::byte{255}, std::byte{128}, std::byte{255},
        std::byte{128}, std::byte{32}, std::byte{255}, std::byte{255},
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    const std::string imageDigest = sha256(image.decodedPixels);
    const auto hexNibble = [](char value) -> std::uint8_t
    {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return value - 'A' + 10;
    };
    for (std::size_t index = 0; index < image.contentIdentity.size(); ++index)
        image.contentIdentity[index] = static_cast<std::byte>(
            (hexNibble(imageDigest[index * 2]) << 4) |
             hexNibble(imageDigest[index * 2 + 1]));
    projectorLighting.projectorTextures.push_back(image);

    DeviceCreationResult projectorCreated = createDevice(
        {Backend::Validation, 0, 2, true});
    ensure("I7c validation device",
           projectorCreated.status.ok() && projectorCreated.device);
    {
        MaterialOffscreenProbe probe(*projectorCreated.device,
            std::move(projectorMaterialPackage),
            std::move(projectorBaseLightingPackage),
            std::move(projectorPackage));
        MaterialOffscreenProbeLimits limits;
        status = probe.submit(material, projectorLighting, limits);
        ensure(status.message(), status.ok());
        MaterialOffscreenProbeResult result;
        status = probe.poll(result);
        ensure(status.message(), status.ok());
        ensure_equals("I7c executes both projector draw paths",
                      result.projectorLights, std::uint32_t{2});
        ensure_equals("I7c deduplicates a shared projector texture",
                      result.projectorTextures, std::uint32_t{1});
        ensure_equals("I7c camera-inside projector uses fullscreen path",
                      result.projectorFullscreenLights, std::uint32_t{1});
        ensure_equals("I7c outside projector uses volume path",
                      result.projectorVolumeLights, std::uint32_t{1});
        ensure("I7c composite probe shuts down", probe.shutdown().ok());
    }
    ensure("I7c deferred resources drain",
           projectorCreated.device->waitIdle().ok());
#if defined(LL_GHI_I7_SHADOW_SHADER_PACKAGE)
    MaterialScenePacket shadowMaterial = material;
    MaterialTextureResource alphaTexture;
    alphaTexture.sourceIdentity[0] = std::byte{0x7d};
    alphaTexture.contentIdentity = image.contentIdentity;
    alphaTexture.colorSpace = TextureColorSpace::SRGB;
    alphaTexture.width = image.width;
    alphaTexture.height = image.height;
    alphaTexture.components = image.components;
    alphaTexture.decodedPixels = image.decodedPixels;
    shadowMaterial.textures.push_back(alphaTexture);
    MaterialResource masked = pbr;
    masked.alphaMode = MaterialAlphaMode::Mask;
    masked.alphaCutoff = 0.5f;
    masked.doubleSided = true;
    MaterialTextureBinding alphaBinding;
    alphaBinding.semantic = TextureSemantic::BaseColor;
    alphaBinding.texture = 0;
    masked.textures.push_back(alphaBinding);
    shadowMaterial.materials.push_back(masked);
    SkinResource skin;
    skin.identity[0] = std::byte{0x51};
    skin.jointCount = 1;
    skin.matrixPalette.resize(12);
    skin.matrixPalette[0] = skin.matrixPalette[5] =
        skin.matrixPalette[10] = 1.f;
    shadowMaterial.skins.push_back(skin);
    MaterialSceneDraw maskedDraw = draw;
    ++maskedDraw.semanticId;
    maskedDraw.material = 1;
    maskedDraw.skin = 0;
    shadowMaterial.draws.push_back(maskedDraw);

    LightingScenePacket shadowLighting = projectorLighting;
    shadowLighting.shadows.enabled = true;
    shadowLighting.shadows.directionalCascadeCount = 4;
    shadowLighting.shadows.projectorShadowCount = 1;
    shadowLighting.shadows.comparability =
        LightingComparability::ShadowImagesDeferred;
    shadowLighting.shadows.clipPlanes = {{1.f, 2.f, 4.f, 8.f}};
    shadowLighting.shadows.directionalBias = 0.001f;
    shadowLighting.shadows.spotShadowOffset = 0.001f;
    shadowLighting.shadows.spotShadowBias = 0.001f;
    for (auto& matrix : shadowLighting.shadows.matrices)
        for (std::size_t diagonal = 0; diagonal < 4; ++diagonal)
            matrix[diagonal * 5] = 1.f;
    shadowLighting.localLights[1].shadowSlot = 0;
    shadowLighting.localLights[1].shadowFade = 0.f;
    shadowLighting.shadows.projectorLightIds[0] =
        shadowLighting.localLights[1].semanticId;

    ShaderPackageDesc shadowMaterialPackage;
    status = loadShaderPackage(
        LL_GHI_R5A_SHADER_PACKAGE, shadowMaterialPackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc shadowLightingPackage;
    status = loadShaderPackage(
        LL_GHI_I7_LIGHTING_SHADER_PACKAGE, shadowLightingPackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc shadowProjectorPackage;
    status = loadShaderPackage(
        LL_GHI_I7_PROJECTOR_SHADER_PACKAGE, shadowProjectorPackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc shadowPackage;
    status = loadShaderPackage(
        LL_GHI_I7_SHADOW_SHADER_PACKAGE, shadowPackage);
    ensure(status.message(), status.ok());
    ensure_equals("I7d reflected shadow bindings",
                  shadowPackage.bindings.size(), std::size_t{5});
    ensure_equals("I7d reflected shadow vertex inputs",
                  shadowPackage.vertexInputs.size(), std::size_t{5});
    ensure_equals("I7d depth-only shader has no color outputs",
                  shadowPackage.fragmentOutputs.size(), std::size_t{0});

    DeviceCreationResult shadowCreated = createDevice(
        {Backend::Validation, 0, 2, true});
    ensure("I7d validation device",
           shadowCreated.status.ok() && shadowCreated.device);
    {
        MaterialOffscreenProbe probe(*shadowCreated.device,
            std::move(shadowMaterialPackage),
            std::move(shadowLightingPackage),
            std::move(shadowProjectorPackage),
            std::move(shadowPackage));
        MaterialOffscreenProbeLimits limits;
        status = probe.submit(shadowMaterial, shadowLighting, limits);
        ensure(status.message(), status.ok());
        MaterialOffscreenProbeResult result;
        status = probe.poll(result);
        ensure(status.message(), status.ok());
        ensure("I7d executes native shadow production", result.shadowsExecuted);
        ensure_equals("I7d produces four cascades and one projector map",
                      result.shadowMaps, std::uint32_t{5});
        ensure_equals("I7d produces four directional cascades",
                      result.directionalShadowMaps, std::uint32_t{4});
        ensure_equals("I7d produces one projector shadow map",
                      result.projectorShadowMaps, std::uint32_t{1});
        ensure_equals("I7d replays both bounded casters",
                      result.shadowCasterDraws, std::uint32_t{2});
        ensure_equals("I7d includes one rigged caster",
                      result.shadowRiggedDraws, std::uint32_t{1});
        ensure_equals("I7d includes one alpha-masked caster",
                      result.shadowMaskedDraws, std::uint32_t{1});
        ensure("I7d composite probe shuts down", probe.shutdown().ok());
    }
    ensure("I7d deferred resources drain",
           shadowCreated.device->waitIdle().ok());
#endif
#endif
#if defined(LL_GHI_I6_TERRAIN_SHADER_PACKAGE)
    ShaderPackageDesc terrainPackage;
    status = loadShaderPackage(
        LL_GHI_I6_TERRAIN_SHADER_PACKAGE, terrainPackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc terrainLightingPackage;
    status = loadShaderPackage(
        LL_GHI_I7_LIGHTING_SHADER_PACKAGE, terrainLightingPackage);
    ensure(status.message(), status.ok());
    TerrainScenePacket terrain;
    terrain.frameId = material.frameId + 1;
    terrain.sceneEpoch = material.sceneEpoch + 1;
    terrain.resourceEpoch = material.resourceEpoch;
    terrain.sourceWidth = material.sourceWidth;
    terrain.sourceHeight = material.sourceHeight;
    for (std::uint8_t textureIndex = 0; textureIndex < 5; ++textureIndex)
    {
        MaterialTextureResource texture;
        texture.width = texture.height = 1;
        texture.components = 4;
        texture.colorSpace = textureIndex ? TextureColorSpace::SRGB
                                          : TextureColorSpace::Linear;
        texture.decodedPixels = {
            static_cast<std::byte>(48 + textureIndex),
            static_cast<std::byte>(96 + textureIndex),
            static_cast<std::byte>(144 + textureIndex), std::byte{255}};
        terrain.textures.push_back(std::move(texture));
    }
    TerrainRegionResource terrainRegion;
    terrainRegion.model = MaterialModel::MetallicRoughness;
    terrainRegion.paintMode = TerrainPaintMode::PBRPaintMap;
    terrainRegion.projection = TerrainProjection::Triplanar;
    terrainRegion.compositionTexture = 0;
    for (std::size_t layer = 0; layer < terrainRegion.layers.size(); ++layer)
    {
        terrainRegion.layers[layer].model = MaterialModel::MetallicRoughness;
        terrainRegion.layers[layer].baseColorTexture =
            static_cast<std::uint32_t>(layer + 1);
        terrainRegion.layers[layer].roughness = 0.75f;
    }
    terrain.regions.push_back(terrainRegion);
    TerrainSceneVertex terrainVertex;
    terrainVertex.position = {{-0.75f, -0.75f, 0.5f}};
    terrain.vertices.push_back(terrainVertex);
    terrainVertex.position = {{0.75f, -0.75f, 0.5f}};
    terrain.vertices.push_back(terrainVertex);
    terrainVertex.position = {{0.f, 0.75f, 0.5f}};
    terrain.vertices.push_back(terrainVertex);
    terrain.indices = {0, 1, 2};
    TerrainSceneDraw terrainDraw;
    terrainDraw.semanticId = 0x4937625f54455252ull; // "I7b_TERR"
    terrainDraw.region = 0;
    terrainDraw.indexCount = 3;
    terrain.draws.push_back(terrainDraw);
    lighting.frameId = terrain.frameId;
    lighting.sceneEpoch = terrain.sceneEpoch;
    {
        TerrainOffscreenProbe probe(*created.device,
            std::move(terrainPackage), std::move(terrainLightingPackage));
        TerrainOffscreenProbeLimits limits;
        LightingScenePacket mismatched = lighting;
        ++mismatched.frameId;
        ensure("I7b terrain rejects mismatched frame pairing",
            probe.submit(terrain, mismatched, limits).code() ==
                StatusCode::InvalidArgument);
        status = probe.submit(terrain, lighting, limits);
        ensure(status.message(), status.ok());
        TerrainOffscreenProbeResult result;
        status = probe.poll(result);
        ensure(status.message(), status.ok());
        ensure("I7b terrain executed the deferred-lighting pass",
               result.lightingExecuted);
        ensure_equals("I7b terrain executed one directional light",
                      result.directionalLights, std::uint32_t{1});
        ensure_equals("I7b terrain executes points but defers projectors",
                      result.pointLights, std::uint32_t{1});
        ensure_equals("I7b terrain executed one PBR draw",
                      result.pbrDraws, std::uint32_t{1});
        ensure("I7b terrain probe shuts down", probe.shutdown().ok());
    }
#endif
    ensure("I7b deferred resources drain", created.device->waitIdle().ok());
#endif
}

template<> template<>
void LLGHIValidationObject::test<36>()
{
    using namespace LL::GHI;

    constexpr std::uint64_t frameId = 0x4938615f4652414dull; // "I8a_FRAM"
    ProductionFramePacket frame;
    frame.frameId = frameId;
    frame.assemblyEpoch = 7;
    frame.sourceWidth = 64;
    frame.sourceHeight = 64;
    frame.passes =
        productionFramePassBit(ProductionFramePass::OpaqueGBuffer) |
        productionFramePassBit(ProductionFramePass::MaterialGBuffer) |
        productionFramePassBit(ProductionFramePass::TerrainGBuffer) |
        productionFramePassBit(ProductionFramePass::DeferredLighting);

    frame.opaque.frameId = frameId;
    frame.opaque.sceneEpoch = 10;
    frame.opaque.sourceWidth = frame.sourceWidth;
    frame.opaque.sourceHeight = frame.sourceHeight;
    OpaqueSceneVertex opaqueVertex;
    opaqueVertex.position = {{-0.25f, -0.25f, 0.5f}};
    frame.opaque.vertices.push_back(opaqueVertex);
    opaqueVertex.position = {{0.25f, -0.25f, 0.5f}};
    frame.opaque.vertices.push_back(opaqueVertex);
    opaqueVertex.position = {{0.f, 0.25f, 0.5f}};
    frame.opaque.vertices.push_back(opaqueVertex);
    frame.opaque.indices = {0, 2, 1};
    OpaqueSceneDraw opaqueDraw;
    opaqueDraw.semanticId = 0x503065315f4f5041ull; // "P0e1_OPA"
    opaqueDraw.indexCount = 3;
    frame.opaque.draws.push_back(opaqueDraw);

    frame.materials.frameId = frameId;
    frame.materials.sceneEpoch = 11;
    frame.materials.resourceEpoch = 3;
    frame.materials.sourceWidth = frame.sourceWidth;
    frame.materials.sourceHeight = frame.sourceHeight;
    MaterialResource material;
    material.identity[0] = std::byte{0x11};
    material.model = MaterialModel::MetallicRoughness;
    frame.materials.materials.push_back(material);
    MaterialSceneVertex materialVertex;
    materialVertex.position = {{-0.5f, -0.5f, 0.5f}};
    frame.materials.vertices.push_back(materialVertex);
    materialVertex.position = {{0.5f, -0.5f, 0.5f}};
    frame.materials.vertices.push_back(materialVertex);
    materialVertex.position = {{0.f, 0.5f, 0.5f}};
    frame.materials.vertices.push_back(materialVertex);
    // Identity clip transforms need clockwise source order because the
    // Vulkan shader prelude flips clip-space Y before canonical CCW culling.
    frame.materials.indices = {0, 2, 1};
    MaterialSceneDraw materialDraw;
    materialDraw.semanticId = 0x4938615f4d41544cull; // "I8a_MATL"
    materialDraw.material = 0;
    materialDraw.indexCount = 3;
    frame.materials.draws.push_back(materialDraw);
    MaterialResource legacyMaterial;
    legacyMaterial.identity[0] = std::byte{0x13};
    legacyMaterial.model = MaterialModel::Legacy;
    legacyMaterial.legacySpecular = {{0.2f, 0.3f, 0.4f, 0.5f}};
    legacyMaterial.environmentIntensity = 0.25f;
    frame.materials.materials.push_back(legacyMaterial);
    MaterialSceneDraw legacyMaterialDraw = materialDraw;
    legacyMaterialDraw.semanticId = 0x50306531625f4c47ull; // "P0e1b_LG"
    legacyMaterialDraw.material = 1;
    frame.materials.draws.push_back(legacyMaterialDraw);
    SkinResource productionSkin;
    productionSkin.identity[0] = std::byte{0x12};
    productionSkin.jointCount = 1;
    productionSkin.matrixPalette = {
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f};
    frame.materials.skins.push_back(productionSkin);
    MaterialSceneDraw riggedMaterialDraw = materialDraw;
    riggedMaterialDraw.semanticId = 0x493863325f524947ull; // "I8c2_RIG"
    riggedMaterialDraw.skin = 0;
    frame.materials.draws.push_back(riggedMaterialDraw);

    frame.terrain.frameId = frameId;
    frame.terrain.sceneEpoch = 12;
    frame.terrain.resourceEpoch = 4;
    frame.terrain.sourceWidth = frame.sourceWidth;
    frame.terrain.sourceHeight = frame.sourceHeight;
    MaterialTextureResource terrainTexture;
    terrainTexture.sourceIdentity[0] = std::byte{0x21};
    terrainTexture.contentIdentity[0] = std::byte{0x22};
    terrainTexture.colorSpace = TextureColorSpace::SRGB;
    terrainTexture.width = terrainTexture.height = 1;
    terrainTexture.components = 4;
    terrainTexture.decodedPixels = {
        std::byte{64}, std::byte{96}, std::byte{128}, std::byte{255}};
    frame.terrain.textures.push_back(terrainTexture);
    TerrainRegionResource region;
    region.identity[0] = std::byte{0x31};
    region.model = MaterialModel::MetallicRoughness;
    region.compositionTexture = 0;
    for (std::size_t layer = 0; layer < region.layers.size(); ++layer)
    {
        region.layers[layer].identity[0] =
            static_cast<std::byte>(0x40 + layer);
        region.layers[layer].model = MaterialModel::MetallicRoughness;
        region.layers[layer].baseColorTexture = 0;
    }
    frame.terrain.regions.push_back(region);
    TerrainSceneVertex terrainVertex;
    terrainVertex.position = {{-0.75f, -0.75f, 0.5f}};
    frame.terrain.vertices.push_back(terrainVertex);
    terrainVertex.position = {{0.75f, -0.75f, 0.5f}};
    frame.terrain.vertices.push_back(terrainVertex);
    terrainVertex.position = {{0.f, 0.75f, 0.5f}};
    frame.terrain.vertices.push_back(terrainVertex);
    frame.terrain.indices = {0, 2, 1};
    TerrainSceneDraw terrainDraw;
    terrainDraw.semanticId = 0x4938615f54455252ull; // "I8a_TERR"
    terrainDraw.region = 0;
    terrainDraw.indexCount = 3;
    frame.terrain.draws.push_back(terrainDraw);

    frame.lighting.frameId = frameId;
    frame.lighting.sceneEpoch = 13;
    frame.lighting.resourceEpoch = 5;
    frame.lighting.sourceWidth = frame.sourceWidth;
    frame.lighting.sourceHeight = frame.sourceHeight;
    frame.lighting.ambientColor = {{0.1f, 0.2f, 0.3f}};
    frame.lighting.sun.active = true;

    ProductionFrameResourceSummary summary;
    Status status = validateProductionFramePacket(frame, &summary);
    ensure(status.message(), status.ok());
    ensure_equals("P0e1 summarizes all opaque draw streams",
                  summary.opaqueDraws + summary.materialDraws +
                      summary.terrainDraws,
                  std::uint32_t{5});
    ensure_equals("I8a summarizes combined geometry", summary.vertices,
                  std::uint32_t{9});
    ensure_equals("I8a builds a typed unique resource inventory",
                  summary.uniqueResources, std::uint32_t{9});
    ensure_equals("I8a accounts decoded texture bytes",
                  summary.decodedTextureBytes, std::uint64_t{4});

    std::vector<std::byte> first, second;
    ensure("I8a encodes an assembled frame",
           encodeProductionFramePacket(frame, first).ok());
    ensure("I8a encoding is deterministic",
           encodeProductionFramePacket(frame, second).ok() && first == second);
    ProductionFramePacket decoded;
    ensure("I8a decodes an assembled frame",
           decodeProductionFramePacket(first, decoded).ok());
    ensure("I8a packet round trips exactly", decoded == frame);
    const std::string hash = productionFramePacketSha256(frame);
    ensure_equals("I8a packet hash is SHA-256", hash.size(), std::size_t{64});

    ProductionFramePacket mismatched = frame;
    ++mismatched.terrain.frameId;
    ensure("I8a rejects cross-frame assembly",
           validateProductionFramePacket(mismatched).code() ==
               StatusCode::InvalidArgument);
    ProductionFramePacket missingOpaque = frame;
    missingOpaque.opaque.draws.clear();
    missingOpaque.opaque.vertices.clear();
    missingOpaque.opaque.indices.clear();
    ensure("P0e1 accepts an empty complementary opaque fallback",
           validateProductionFramePacket(missingOpaque).ok());
    ProductionFramePacket missingOpaquePass = frame;
    missingOpaquePass.passes &=
        ~productionFramePassBit(ProductionFramePass::OpaqueGBuffer);
    ensure("P0e1 requires the generic opaque pass declaration",
           validateProductionFramePacket(missingOpaquePass).code() ==
               StatusCode::InvalidArgument);
    ProductionFramePacket invalidPass = frame;
    invalidPass.passes |=
        productionFramePassBit(ProductionFramePass::ProjectorLighting);
    ensure("I8a rejects a projector pass without projector resources",
           validateProductionFramePacket(invalidPass).code() ==
               StatusCode::InvalidArgument);
    std::vector<std::byte> truncated(first.begin(), first.end() - 1);
    ensure("I8a rejects truncated assembled frames",
           decodeProductionFramePacket(truncated, decoded).code() ==
               StatusCode::InvalidArgument);

    DeviceCreationResult created = createDevice(
        {Backend::Validation, 0, 2, true});
    ensure("I8a validation device", created.status.ok() && created.device);
    ProductionFrameTransferResult result;
    ProductionFrameTransferLimits limits;
    status = consumeProductionFrameTransfer(
        *created.device, frame, limits, result);
    ensure(status.message(), status.ok());
    ensure_equals("I8a transfers the complete encoded frame",
                  result.uploadBytes,
                  static_cast<std::uint64_t>(first.size()));
    ensure_equals("I8a transfer preserves frame identity",
                  result.packetSha256, hash);
    ensure("I8a deferred transfer resources drain",
           created.device->waitIdle().ok());

    ProductionTextureResidency residency(*created.device);
    ProductionTextureResidencyLimits residencyLimits;
    residencyLimits.maxEntries = 1;
    ProductionTextureResidencyResult residencyResult;
    status = residency.update(frame, residencyLimits, residencyResult);
    ensure(status.message(), status.ok());
    ensure_equals("I8b uploads the first unique decoded texture",
                  residencyResult.uploads, std::uint32_t{1});
    ensure_equals("I8b records one resident allocation",
                  residencyResult.residentEntries, std::uint32_t{1});
    const ProductionTextureSourceIdentity terrainSource{
        ProductionTextureDomain::Terrain,
        frame.terrain.textures[0].sourceIdentity};
    const auto firstBinding = residency.find(terrainSource);
    ensure("I8b resolves a retained native binding", firstBinding.has_value());
    ensure_equals("I8b starts logical source generation at one",
                  firstBinding->generation, std::uint64_t{1});

    ++frame.assemblyEpoch;
    status = residency.update(frame, residencyLimits, residencyResult);
    ensure(status.message(), status.ok());
    ensure_equals("I8b reuses unchanged decoded content",
                  residencyResult.cacheHits, std::uint32_t{1});
    ensure_equals("I8b avoids redundant uploads",
                  residencyResult.uploads, std::uint32_t{0});

    ++frame.assemblyEpoch;
    frame.terrain.textures[0].decodedPixels[0] = std::byte{65};
    frame.terrain.textures[0].contentIdentity[0] = std::byte{0x23};
    status = residency.update(frame, residencyLimits, residencyResult);
    ensure(status.message(), status.ok());
    ensure_equals("I8b advances a changed logical source generation",
                  residencyResult.generationChanges, std::uint32_t{1});
    ensure_equals("I8b uploads changed decoded content",
                  residencyResult.uploads, std::uint32_t{1});
    ensure_equals("I8b evicts the displaced allocation under its cap",
                  residencyResult.evictions, std::uint32_t{1});
    const auto changedBinding = residency.find(terrainSource);
    ensure("I8b resolves the changed native binding", changedBinding.has_value());
    ensure_equals("I8b publishes the new source generation",
                  changedBinding->generation, std::uint64_t{2});
    ensure("I8b replaces the native image handle",
           changedBinding->image != firstBinding->image);

    ProductionFrameTargets targets(*created.device);
    ProductionFrameTargetLimits targetLimits;
    targetLimits.maxWidth = targetLimits.maxHeight = 32;
    targetLimits.maxPixels = 32 * 32;
    ProductionFrameTargetResult targetResult;
    status = targets.ensure(frame, targetLimits, targetResult);
    ensure(status.message(), status.ok());
    ensure("I8c1 allocates a new shared target topology",
           !targetResult.reused);
    ensure_equals("I8c1 owns four G-buffer, depth, and lighting images",
                  targetResult.imageCount, std::uint32_t{6});
    ensure_equals("I8c1 bounds the private target width",
                  targetResult.width, std::uint32_t{32});
    const std::uint64_t firstTargetGeneration =
        targetResult.targetGeneration;
    status = targets.ensure(frame, targetLimits, targetResult);
    ensure(status.message(), status.ok());
    ensure("I8c1 reuses an unchanged shared target topology",
           targetResult.reused);
    ensure_equals("I8c1 retains its target generation on reuse",
                  targetResult.targetGeneration, firstTargetGeneration);
    targetLimits.maxWidth = targetLimits.maxHeight = 16;
    targetLimits.maxPixels = 16 * 16;
    status = targets.ensure(frame, targetLimits, targetResult);
    ensure(status.message(), status.ok());
    ensure("I8c1 reallocates a changed bounded extent",
           !targetResult.reused);
    ensure("I8c1 advances target generation on replacement",
           targetResult.targetGeneration > firstTargetGeneration);
#if defined(LL_GHI_R4_SHADER_PACKAGE) && \
    defined(LL_GHI_R5A_SHADER_PACKAGE) && \
    defined(LL_GHI_I6_TERRAIN_SHADER_PACKAGE)
    ShaderPackageDesc productionOpaquePackage;
    status = loadShaderPackage(
        LL_GHI_R4_SHADER_PACKAGE, productionOpaquePackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc productionMaterialPackage;
    status = loadShaderPackage(
        LL_GHI_R5A_SHADER_PACKAGE, productionMaterialPackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc productionTerrainPackage;
    status = loadShaderPackage(
        LL_GHI_I6_TERRAIN_SHADER_PACKAGE, productionTerrainPackage);
    ensure(status.message(), status.ok());
    ProductionGBufferExecutor executor(
        *created.device, std::move(productionOpaquePackage),
        std::move(productionMaterialPackage),
        std::move(productionTerrainPackage));
    ProductionGBufferLimits executionLimits;
    status = executor.submit(
        frame, targets.targets(), residency, executionLimits);
    ensure(status.message(), status.ok());
    ensure("I8c2 shared G-buffer execution is asynchronous",
           executor.pending());
    ProductionGBufferResult executionResult;
    status = executor.poll(executionResult);
    ensure(status.message(), status.ok());
    ensure_equals("P0e1 executes the generic opaque stream",
                  executionResult.opaqueDraws, std::uint32_t{1});
    ensure_equals("I8c2 executes the material stream",
                  executionResult.materialDraws, std::uint32_t{3});
    ensure_equals("P0e1 executes a legacy opaque material draw",
                  executionResult.legacyMaterialDraws, std::uint32_t{1});
    ensure_equals("I8c2 executes a rigged material draw",
                  executionResult.riggedMaterialDraws, std::uint32_t{1});
    ensure_equals("I8c2 executes the terrain stream",
                  executionResult.terrainDraws, std::uint32_t{1});
    ensure_equals("I8c2 retains PBR terrain execution",
                  executionResult.pbrTerrainDraws, std::uint32_t{1});
    for (const std::string& targetHash : executionResult.colorSha256)
        ensure("I8c2 completes each shared-target verification readback",
               !targetHash.empty());
    ensure("I8c2 records a combined production-frame identity",
           !executionResult.frameSha256.empty());
    status = executor.submit(
        missingOpaque, targets.targets(), residency, executionLimits);
    ensure(status.message(), status.ok());
    status = executor.poll(executionResult);
    ensure(status.message(), status.ok());
    ensure_equals("P0e1 executes without a duplicate opaque fallback",
                  executionResult.opaqueDraws, std::uint32_t{0});
    ensure_equals("P0e1 retains legacy material ownership without fallback",
                  executionResult.legacyMaterialDraws, std::uint32_t{1});
#if defined(LL_GHI_I7_LIGHTING_SHADER_PACKAGE) && \
    defined(LL_GHI_I7_PROJECTOR_SHADER_PACKAGE) && \
    defined(LL_GHI_I7_SHADOW_SHADER_PACKAGE)
    MaterialTextureResource alphaTexture;
    alphaTexture.sourceIdentity[0] = std::byte{0x61};
    alphaTexture.contentIdentity[0] = std::byte{0x62};
    alphaTexture.colorSpace = TextureColorSpace::SRGB;
    alphaTexture.width = alphaTexture.height = 1;
    alphaTexture.components = 4;
    alphaTexture.decodedPixels = {
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    frame.materials.textures.push_back(alphaTexture);
    MaterialResource maskedMaterial = material;
    maskedMaterial.identity[0] = std::byte{0x63};
    maskedMaterial.alphaMode = MaterialAlphaMode::Mask;
    maskedMaterial.alphaCutoff = .5f;
    maskedMaterial.doubleSided = true;
    MaterialTextureBinding alphaBinding;
    alphaBinding.semantic = TextureSemantic::BaseColor;
    alphaBinding.texture = 0;
    maskedMaterial.textures.push_back(alphaBinding);
    frame.materials.materials.push_back(maskedMaterial);
    MaterialSceneDraw maskedDraw = riggedMaterialDraw;
    maskedDraw.semanticId = 0x493863335f4d4153ull; // "I8c3_MAS"
    maskedDraw.material = 2;
    frame.materials.draws.push_back(maskedDraw);

    ProjectorTextureResource projectorTexture;
    projectorTexture.sourceIdentity[0] = 0x71;
    projectorTexture.contentIdentity[0] = std::byte{0x72};
    projectorTexture.width = projectorTexture.height = 1;
    projectorTexture.components = 4;
    projectorTexture.decodedPixels = {
        std::byte{255}, std::byte{192}, std::byte{128}, std::byte{255}};
    const std::string projectorDigest = sha256(
        projectorTexture.decodedPixels);
    auto hexNibble = [](char value) -> std::uint8_t
    {
        return value >= 'a' ? static_cast<std::uint8_t>(value - 'a' + 10)
                            : static_cast<std::uint8_t>(value - '0');
    };
    for (std::size_t byte = 0;
         byte < projectorTexture.contentIdentity.size(); ++byte)
        projectorTexture.contentIdentity[byte] = static_cast<std::byte>(
            (hexNibble(projectorDigest[byte * 2]) << 4) |
             hexNibble(projectorDigest[byte * 2 + 1]));
    frame.lighting.projectorTextures.push_back(projectorTexture);
    LocalLightRecord projectorLight;
    projectorLight.semanticId = 0x493863335f50524aull; // "I8c3_PRJ"
    projectorLight.kind = LocalLightKind::Projector;
    projectorLight.position = {{0.f, 0.f, 2.f}};
    projectorLight.radius = 8.f;
    projectorLight.projectorTextureIdentity =
        projectorTexture.sourceIdentity;
    projectorLight.shadowSlot = 0;
    frame.lighting.localLights.push_back(projectorLight);
    frame.lighting.shadows.enabled = true;
    frame.lighting.shadows.directionalCascadeCount = 4;
    frame.lighting.shadows.projectorShadowCount = 1;
    frame.lighting.shadows.comparability =
        LightingComparability::ShadowImagesDeferred;
    frame.lighting.shadows.clipPlanes = {{1.f, 2.f, 4.f, 8.f}};
    frame.lighting.shadows.directionalBias = .001f;
    frame.lighting.shadows.spotShadowOffset = .001f;
    frame.lighting.shadows.spotShadowBias = .001f;
    frame.lighting.shadows.projectorLightIds[0] =
        projectorLight.semanticId;
    for (auto& matrix : frame.lighting.shadows.matrices)
        for (std::size_t diagonal = 0; diagonal < 4; ++diagonal)
            matrix[diagonal * 5] = 1.f;
    for (std::size_t diagonal = 0; diagonal < 4; ++diagonal)
    {
        frame.lighting.viewMatrix[diagonal * 5] = 1.f;
        frame.lighting.projectionMatrix[diagonal * 5] = 1.f;
    }
    frame.passes |=
        productionFramePassBit(ProductionFramePass::DirectionalShadow) |
        productionFramePassBit(ProductionFramePass::ProjectorShadow) |
        productionFramePassBit(ProductionFramePass::ProjectorLighting);
    ++frame.assemblyEpoch;
    ++frame.materials.resourceEpoch;
    ++frame.lighting.resourceEpoch;
    residencyLimits.maxEntries = 4;
    status = residency.update(frame, residencyLimits, residencyResult);
    ensure(status.message(), status.ok());
    ensure_equals("I8c3 retains the masked and projector images",
                  residencyResult.residentEntries, std::uint32_t{3});
    status = targets.ensure(frame, targetLimits, targetResult);
    ensure(status.message(), status.ok());
    ensure_equals("I8c3 owns four G-buffer, depth, lighting and six semantic shadow images",
                  targetResult.imageCount, std::uint32_t{12});

    status = executor.submit(
        frame, targets.targets(), residency, executionLimits);
    ensure(status.message(), status.ok());
    status = executor.poll(executionResult);
    ensure(status.message(), status.ok());
    ShaderPackageDesc productionLightingPackage;
    status = loadShaderPackage(
        LL_GHI_I7_LIGHTING_SHADER_PACKAGE, productionLightingPackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc productionProjectorPackage;
    status = loadShaderPackage(
        LL_GHI_I7_PROJECTOR_SHADER_PACKAGE, productionProjectorPackage);
    ensure(status.message(), status.ok());
    ShaderPackageDesc productionShadowPackage;
    status = loadShaderPackage(
        LL_GHI_I7_SHADOW_SHADER_PACKAGE, productionShadowPackage);
    ensure(status.message(), status.ok());
    ProductionLightingExecutor lightingExecutor(
        *created.device, std::move(productionLightingPackage),
        std::move(productionProjectorPackage),
        std::move(productionShadowPackage));
    ProductionLightingLimits lightingLimits;
    status = lightingExecutor.submit(
        frame, targets.targets(), residency, lightingLimits);
    ensure(status.message(), status.ok());
    ensure("I8c3 shared lighting execution is asynchronous",
           lightingExecutor.pending());
    ProductionLightingResult lightingResult;
    status = lightingExecutor.poll(lightingResult);
    ensure(status.message(), status.ok());
    ensure_equals("I8c3 executes four directional cascades",
                  lightingResult.directionalShadowMaps, std::uint32_t{4});
    ensure_equals("I8c3 executes one projector shadow",
                  lightingResult.projectorShadowMaps, std::uint32_t{1});
    ensure_equals("I8c3 replays opaque and alpha-masked casters",
                  lightingResult.shadowCasterDraws, std::uint32_t{4});
    ensure_equals("I8c3 includes two rigged casters",
                  lightingResult.shadowRiggedDraws, std::uint32_t{2});
    ensure_equals("I8c3 includes one alpha-masked caster",
                  lightingResult.shadowMaskedDraws, std::uint32_t{1});
    ensure_equals("I8c3 executes one projector light",
                  lightingResult.projectorLights, std::uint32_t{1});
    ensure("I8c3 completes the shared lighting readback",
           !lightingResult.lightingSha256.empty());
    ensure("I8c3 records the same production-frame identity",
           !lightingResult.frameSha256.empty());
    ensure("I8c3 destroys lighting resources explicitly",
           lightingExecutor.shutdown().ok());
#endif
    ensure("I8c2 destroys execution resources explicitly",
           executor.shutdown().ok());
#endif
    ensure("I8c1 destroys shared targets explicitly",
           targets.shutdown().ok());
    ensure("I8b destroys retained resources explicitly",
           residency.shutdown().ok());
    ensure("I8b deferred residency resources drain",
           created.device->waitIdle().ok());
}

template<> template<>
void LLGHIValidationObject::test<37>()
{
    using namespace LL::GHI;

    DeviceCreationResult created = createDevice({Backend::Validation, 0, 2, true});
    auto* device = dynamic_cast<ValidationDevice*>(created.device.get());
    ensure("P0c validation device", created.status.ok() && device);
    ensure("P0c exposes non-solid fill", device->capabilities().nonSolidFill);
    ensure("P0c exposes wide lines", device->capabilities().wideLines);

    Status status = Status::success();
    ShaderPackageHandle shader = device->createShaderPackage(
        makeUnboundShaderPackage(), status);
    ensure("P0c shader package", status.ok() && shader);

    PipelineDesc pipeline;
    pipeline.shader = shader;
    pipeline.polygonMode = PolygonMode::Line;
    pipeline.depthBias = true;
    pipeline.depthBiasConstantFactor = 2.f;
    pipeline.depthBiasSlopeFactor = 1.f;
    pipeline.lineWidth = 2.f;
    pipeline.depthTest = false;
    pipeline.depthWrite = false;
    pipeline.colorFormats = {Format::RGBA8UNorm};
    pipeline.blendStates = {BlendState{}};
    PipelineHandle accepted = device->createPipeline(pipeline, status);
    ensure("P0c explicit raster state accepted", status.ok() && accepted);

    pipeline.lineWidth = 0.f;
    ensure("P0c zero line width rejected",
        !device->createPipeline(pipeline, status) &&
        status.code() == StatusCode::InvalidArgument);
    pipeline.lineWidth = std::numeric_limits<float>::infinity();
    ensure("P0c non-finite line width rejected",
        !device->createPipeline(pipeline, status) &&
        status.code() == StatusCode::InvalidArgument);
    pipeline.lineWidth = 1.f;
    pipeline.depthBiasSlopeFactor = std::numeric_limits<float>::quiet_NaN();
    ensure("P0c non-finite depth bias rejected",
        !device->createPipeline(pipeline, status) &&
        status.code() == StatusCode::InvalidArgument);
}

template<> template<>
void LLGHIValidationObject::test<38>()
{
    using namespace LL::GHI;

    EnvironmentScenePacket source;
    source.frameId = 0x5030453241ull;
    source.sceneEpoch = 7;
    source.resourceEpoch = 11;
    source.sourceWidth = 1920;
    source.sourceHeight = 1080;
    source.passMask =
        environmentPassBit(EnvironmentPass::Atmosphere) |
        environmentPassBit(EnvironmentPass::Sun) |
        environmentPassBit(EnvironmentPass::Moon) |
        environmentPassBit(EnvironmentPass::Stars) |
        environmentPassBit(EnvironmentPass::Clouds) |
        environmentPassBit(EnvironmentPass::WaterSurface);
    source.dependencyMask =
        environmentDependencyBit(EnvironmentDependency::ProductionLighting) |
        environmentDependencyBit(EnvironmentDependency::ProductionDepth) |
        environmentDependencyBit(EnvironmentDependency::WaterExclusionMask) |
        environmentDependencyBit(EnvironmentDependency::ReflectionColor) |
        environmentDependencyBit(EnvironmentDependency::RefractionColor);
    source.atmosphere.planetRadius = 6360.f;
    source.atmosphere.skyBottomRadius = 6360.f;
    source.atmosphere.skyTopRadius = 6420.f;
    source.atmosphere.blueDensity = {.24f, .45f, .76f};
    source.atmosphere.blueHorizon = {.49f, .49f, .64f};
    source.atmosphere.gamma = 1.1f;
    source.sky.cloudScale = .42f;
    source.sky.blendFactor = .25f;
    source.sky.sunUp = true;
    source.water.fogDensity = .08f;
    source.water.exposure = 1.25f;
    source.water.normalBlendFactor = .25f;

    for (std::uint8_t index = 0; index < 5; ++index)
    {
        MaterialTextureResource texture;
        texture.sourceIdentity[0] = static_cast<std::byte>(index + 1);
        texture.contentIdentity[0] = static_cast<std::byte>(index + 17);
        texture.width = 1;
        texture.height = 1;
        texture.components = 4;
        texture.decodedPixels = {
            static_cast<std::byte>(index), std::byte{64},
            std::byte{128}, std::byte{255}};
        source.textures.push_back(std::move(texture));
    }
    MaterialTextureResource reflection = source.textures.front();
    reflection.sourceIdentity[0] = std::byte{6};
    reflection.contentIdentity[0] = std::byte{22};
    source.textures.push_back(std::move(reflection));
    MaterialTextureResource exclusion = source.textures.front();
    exclusion.sourceIdentity[0] = std::byte{7};
    exclusion.contentIdentity[0] = std::byte{23};
    exclusion.components = 1;
    exclusion.decodedPixels = {std::byte{255}};
    source.textures.push_back(std::move(exclusion));
    MaterialTextureResource depth = source.textures.back();
    depth.sourceIdentity[0] = std::byte{8};
    depth.contentIdentity[0] = std::byte{24};
    depth.components = 4;
    depth.decodedPixels = {
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    source.textures.push_back(std::move(depth));
    source.sky.textures = {
        {EnvironmentTextureSemantic::Sun, 0},
        {EnvironmentTextureSemantic::Moon, 1},
        {EnvironmentTextureSemantic::StarBloom, 2},
        {EnvironmentTextureSemantic::CloudNoise, 3}};
    source.water.textures = {
        {EnvironmentTextureSemantic::WaterNormal, 4},
        {EnvironmentTextureSemantic::ReflectionColor, 5},
        {EnvironmentTextureSemantic::WaterExclusionMask, 6},
        {EnvironmentTextureSemantic::WaterDepth, 7}};
    source.skyVertices = {
        {{{-1.f, -1.f, 0.f}}, {{0.f, 0.f}}, {{1.f, 1.f, 1.f, 1.f}}},
        {{{ 1.f, -1.f, 0.f}}, {{1.f, 0.f}}, {{1.f, 1.f, 1.f, 1.f}}},
        {{{ 0.f,  1.f, 0.f}}, {{.5f, 1.f}}, {{1.f, 1.f, 1.f, 1.f}}}};
    source.skyIndices = {0, 1, 2};
    source.skyDraws = {
        {SkyGeometryKind::Dome, EnvironmentPrimitive::TriangleStrip, 0, 3, {}},
        {SkyGeometryKind::Sun, EnvironmentPrimitive::Triangles, 0, 3, {}},
        {SkyGeometryKind::Moon, EnvironmentPrimitive::Triangles, 0, 3, {}},
        {SkyGeometryKind::Stars, EnvironmentPrimitive::Triangles, 0, 3, {}}};
    source.waterVertices = {
        {{{-1.f, -1.f, 0.f}}, {{0.f, 0.f, 1.f}}, {{0.f, 0.f}}},
        {{{ 1.f, -1.f, 0.f}}, {{0.f, 0.f, 1.f}}, {{1.f, 0.f}}},
        {{{ 0.f,  1.f, 0.f}}, {{0.f, 0.f, 1.f}}, {{.5f, 1.f}}}};
    source.waterIndices = {0, 1, 2};
    source.waterDraws = {{0x5741544552ull, 0, 3, {}, false}};

    ensure("P0e2 environment contract validates",
           validateEnvironmentScenePacket(source).ok());
    std::vector<std::byte> first, second;
    ensure("P0e2 environment packet encodes",
           encodeEnvironmentScenePacket(source, first).ok());
    ensure("P0e2 environment packet encoding is deterministic",
           encodeEnvironmentScenePacket(source, second).ok() && first == second);
    EnvironmentScenePacket decoded;
    ensure("P0e2 environment packet decodes",
           decodeEnvironmentScenePacket(first, decoded).ok());
    ensure("P0e2 environment packet round trips", decoded == source);
    ensure("P0e2 environment packet has a stable identity",
           !environmentScenePacketSha256(source).empty());

        EnvironmentScenePacket version2 = source;
        version2.version = 2;
        version2.water.textures.resize(1);
        version2.textures.resize(5);
        std::vector<std::byte> version2Encoded;
        EnvironmentScenePacket version2Decoded;
        ensure("P0e2 packet v2 remains encodable",
            encodeEnvironmentScenePacket(version2, version2Encoded).ok());
        ensure("P0e2 packet v2 remains decodable",
            decodeEnvironmentScenePacket(version2Encoded, version2Decoded).ok() &&
            version2Decoded == version2);

    DeviceCreationResult created = createDevice(
        {Backend::Validation, 0, 2, true});
    ensure("P0e2 water resource validation device creation",
           created.status.ok() && created.device);
    ProductionWaterResources resources(*created.device);
    ProductionWaterResourceResult resourceResult;
    Status status = resources.update(
        source, 9, ProductionWaterResourceLimits{}, resourceResult);
    ensure(status.message(), status.ok());
        ensure("P0e2 water resources publish all GHI views",
           resources.dependencies().reflectionColorView &&
            resources.dependencies().refractionColorView &&
            resources.dependencies().exclusionMaskView &&
            resources.dependencies().waterDepthView);
    ensure_equals("P0e2 water resources publish the target generation",
                  resources.dependencies().generation, std::uint64_t{9});
    ensure("P0e2 water resources report content identities",
           !resourceResult.reflectionSha256.empty() &&
           !resourceResult.exclusionSha256.empty() &&
           !resourceResult.depthSha256.empty());
    status = resources.update(
        source, 10, ProductionWaterResourceLimits{}, resourceResult);
    ensure("P0e2 water resources reject a stale resource epoch", !status);
    ensure("P0e2 water resources shut down", resources.shutdown().ok());

    EnvironmentScenePacket invalid = source;
    invalid.passMask |= environmentPassBit(EnvironmentPass::HdriSky);
    ensure("P0e2 atmosphere and HDRI are exclusive",
           !validateEnvironmentScenePacket(invalid));
    invalid = source;
    invalid.dependencyMask = 0;
    ensure("P0e2 water requires explicit shared-target dependencies",
           !validateEnvironmentScenePacket(invalid));
    invalid = source;
    invalid.water.textures.pop_back();
        ensure("P0e2 v4 water requires captured depth content",
            !validateEnvironmentScenePacket(invalid));
        invalid = source;
        invalid.water.textures.erase(invalid.water.textures.begin() + 2);
    ensure("P0e2 v3 water requires captured exclusion content",
           !validateEnvironmentScenePacket(invalid));
    invalid = source;
    invalid.water.textures.erase(invalid.water.textures.begin() + 1);
    ensure("P0e2 v3 water requires captured reflection content",
           !validateEnvironmentScenePacket(invalid));
    invalid = source;
    invalid.sky.textures.push_back(
        {EnvironmentTextureSemantic::Sun, 1});
    ensure("P0e2 texture semantics have one owner",
           !validateEnvironmentScenePacket(invalid));
    invalid = source;
    invalid.skyDraws.pop_back();
    ensure("P0e2 randomized stars require captured geometry",
           !validateEnvironmentScenePacket(invalid));
    invalid = source;
    invalid.waterIndices[2] = 3;
    ensure("P0e2 water geometry is bounds checked",
           !validateEnvironmentScenePacket(invalid));
    first.pop_back();
    ensure("P0e2 truncated environment packets fail closed",
           !decodeEnvironmentScenePacket(first, decoded));
}

template<> template<>
void LLGHIValidationObject::test<39>()
{
    using namespace LL::GHI;

    AlphaScenePacket source;
    source.frameId = 0x503065335f414c50ull; // "P0e3_ALP"
    source.sceneEpoch = 4;
    source.resourceEpoch = 7;
    source.sourceWidth = 2560;
    source.sourceHeight = 1350;
    source.requestedMethod = AlphaMethod::PPLL;
    source.materials.frameId = source.frameId;
    source.materials.sceneEpoch = source.sceneEpoch;
    source.materials.resourceEpoch = source.resourceEpoch;
    source.materials.sourceWidth = source.sourceWidth;
    source.materials.sourceHeight = source.sourceHeight;

    MaterialTextureResource texture;
    texture.sourceIdentity[0] = std::byte{1};
    texture.contentIdentity[0] = std::byte{2};
    texture.width = texture.height = 1;
    texture.components = 4;
    texture.colorSpace = TextureColorSpace::SRGB;
    texture.decodedPixels = {
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{128}};
    source.materials.textures.push_back(texture);
    MaterialResource material;
    material.identity[0] = std::byte{3};
    material.alphaMode = MaterialAlphaMode::Blend;
    material.comparability = ResourceComparability::AlphaDeferred;
    material.textures.push_back({TextureSemantic::BaseColor, 0});
    source.materials.materials.push_back(material);
    source.materials.vertices.resize(3);
    source.materials.indices = {0, 1, 2};
    MaterialSceneDraw materialDraw;
    materialDraw.semanticId = 0x414c504841445241ull;
    materialDraw.material = 0;
    materialDraw.indexCount = 3;
    source.materials.draws.push_back(materialDraw);
    AlphaSceneDraw alphaDraw;
    alphaDraw.classification = AlphaSubmissionClass::StandardBlend;
    alphaDraw.blend.sourceColor = AlphaBlendFactor::SourceAlpha;
    alphaDraw.blend.destinationColor =
        AlphaBlendFactor::OneMinusSourceAlpha;
    alphaDraw.blend.sourceAlpha = AlphaBlendFactor::Zero;
    alphaDraw.blend.destinationAlpha =
        AlphaBlendFactor::OneMinusSourceAlpha;
    source.draws.push_back(alphaDraw);

    ensure("P0e3 alpha scene validates",
           validateAlphaScenePacket(source).ok());
    std::vector<std::byte> first, second;
    ensure("P0e3 alpha scene encodes",
           encodeAlphaScenePacket(source, first).ok());
    ensure("P0e3 alpha scene encoding is deterministic",
           encodeAlphaScenePacket(source, second).ok() && first == second);
    AlphaScenePacket decoded;
    ensure("P0e3 alpha scene decodes",
           decodeAlphaScenePacket(first, decoded).ok());
    ensure("P0e3 alpha scene round trips", decoded == source);
    ensure("P0e3 preserves production separate alpha blend factors",
           decoded.draws[0].blend.sourceAlpha == AlphaBlendFactor::Zero &&
           decoded.draws[0].blend.destinationAlpha ==
               AlphaBlendFactor::OneMinusSourceAlpha);
    ensure("P0e3 alpha scene has a stable identity",
           !alphaScenePacketSha256(source).empty());

    AlphaScenePacket invalid = source;
    invalid.materials.frameId++;
    ensure("P0e3 rejects cross-frame material work",
           !validateAlphaScenePacket(invalid));
    invalid = source;
    invalid.draws[0].classification = AlphaSubmissionClass::Mask;
    ensure("P0e3 rejects route/material disagreement",
           !validateAlphaScenePacket(invalid));
    invalid = source;
    invalid.draws[0].classification = AlphaSubmissionClass::Particle;
    ensure("P0e3 particles remain serializable on the residual route",
           validateAlphaScenePacket(invalid).ok() &&
           routeAlphaSubmission(
               {invalid.phase, invalid.draws[0].classification},
               {invalid.requestedMethod, true, true,
                invalid.transientLoad}).route == AlphaRoute::LegacyResidual);
    invalid = source;
    invalid.depthPeelPolicy.maximumLayers = 0;
    ensure("P0e3 rejects unbounded OIT policy",
           !validateAlphaScenePacket(invalid));
        invalid = source;
        invalid.draws[0].blend.sourceColor =
         static_cast<AlphaBlendFactor>(0xff);
        ensure("P0e3 rejects malformed blend state",
            !validateAlphaScenePacket(invalid));
        first.push_back(std::byte{0});
        ensure("P0e3 rejects trailing alpha scene data",
            !decodeAlphaScenePacket(first, decoded));
        first.pop_back();
    #if defined(LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE)
        AlphaScenePacket execution = source;
        execution.materials.vertices[0].position = {{-.5f, -.5f, .5f}};
        execution.materials.vertices[1].position = {{.5f, -.5f, .5f}};
        execution.materials.vertices[2].position = {{0.f, .5f, .5f}};
        for (auto& vertex : execution.materials.vertices)
            vertex.normal = {{0.f, 0.f, 1.f}};
        execution.draws[0].emissive = true;
        execution.draws.push_back(execution.draws[0]);
        execution.draws.back().classification = AlphaSubmissionClass::Particle;
        execution.materials.draws.push_back(execution.materials.draws[0]);
        MaterialResource maskMaterial = execution.materials.materials[0];
        maskMaterial.identity[0] = std::byte{4};
        maskMaterial.alphaMode = MaterialAlphaMode::Mask;
        maskMaterial.comparability = ResourceComparability::Comparable;
        execution.materials.materials.push_back(maskMaterial);
        MaterialSceneDraw maskGeometry = execution.materials.draws[0];
        maskGeometry.material = 1;
        execution.materials.draws.push_back(maskGeometry);
        AlphaSceneDraw maskPolicy;
        maskPolicy.classification = AlphaSubmissionClass::Mask;
        maskPolicy.minimumAlpha = .5f;
        execution.draws.push_back(maskPolicy);
        ensure("P0e3c synthetic production alpha packet validates",
               validateAlphaScenePacket(execution).ok());

        DeviceCreationResult alphaDevice = createDevice(
            {Backend::Validation, 0, 2, true});
        ensure("P0e3c validation device",
               alphaDevice.status.ok() && alphaDevice.device);
        ShaderPackageDesc alphaPackage;
        Status alphaStatus = loadShaderPackage(
            LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE, alphaPackage);
        ensure(alphaStatus.message(), alphaStatus.ok());
        ProductionFrameTargetSet alphaTargets;
        alphaTargets.width = alphaTargets.height = 32;
        alphaTargets.generation = 7;
        alphaTargets.lightingImage = alphaDevice.device->createImage(
            {{32, 32, 1}, Format::RGBA16Float,
             ResourceUsage::ColorAttachment | ResourceUsage::TransferSource,
             1, 1, 1}, alphaStatus);
        alphaTargets.lightingView = alphaDevice.device->createImageView(
            {alphaTargets.lightingImage, Format::RGBA16Float,
             {ImageAspect::Color, 0, 1, 0, 1}}, alphaStatus);
        alphaTargets.depthImage = alphaDevice.device->createImage(
            {{32, 32, 1}, Format::Depth32Float,
             ResourceUsage::DepthStencilAttachment | ResourceUsage::Sampled,
             1, 1, 1}, alphaStatus);
        alphaTargets.depthView = alphaDevice.device->createImageView(
            {alphaTargets.depthImage, Format::Depth32Float,
             {ImageAspect::Depth, 0, 1, 0, 1}}, alphaStatus);
        ensure(alphaStatus.message(), alphaStatus.ok());
        ProductionAlphaExecutor alphaExecutor(
            *alphaDevice.device, std::move(alphaPackage));
        ProductionAlphaLighting alphaLighting;
        alphaLighting.generation = alphaTargets.generation;
        alphaStatus = alphaExecutor.submit(
            execution, alphaTargets, alphaLighting, ProductionAlphaLimits{});
        ensure(alphaStatus.message(), alphaStatus.ok());
        ensure("P0e3c execution is asynchronous", alphaExecutor.pending());
        ProductionAlphaResult alphaResult;
        alphaStatus = alphaExecutor.poll(alphaResult);
        ensure(alphaStatus.message(), alphaStatus.ok());
        ensure_equals("P0e3c leaves masks with the G-buffer owner",
                      alphaResult.maskDraws, std::uint32_t{1});
        ensure_equals("P0e3c executes standard alpha in captured order",
                      alphaResult.sortedDraws, std::uint32_t{1});
        ensure_equals("P0e3c executes particles on the residual route",
                      alphaResult.residualDraws, std::uint32_t{1});
        ensure_equals("P0e3c replays each emissive intent once",
                      alphaResult.emissiveReplays, std::uint32_t{2});
        ensure_equals("P0e3c has no deferred executable work",
                      alphaResult.deferredDraws, std::uint32_t{0});
        ensure_equals("P0e3c has no material-route deferrals",
                      alphaResult.deferredRouteOrMaterialDraws, std::uint32_t{0});
        ensure_equals("P0e3c has no skin deferrals",
                      alphaResult.deferredSkinDraws, std::uint32_t{0});
        ensure_equals("P0e3c has no texture deferrals",
                      alphaResult.deferredTextureDraws, std::uint32_t{0});
        ensure_equals("P0e3c preserves packet identity",
                      alphaResult.packetSha256,
                      alphaScenePacketSha256(execution));
        ProductionAlphaLighting staleLighting = alphaLighting;
        ++staleLighting.generation;
        ensure("P0e3c rejects stale lighting dependencies",
               !alphaExecutor.submit(execution, alphaTargets, staleLighting,
                                     ProductionAlphaLimits{}));
            ProductionAlphaLighting invalidLighting = alphaLighting;
            invalidLighting.ambient[0] = 17.f;
            ensure("P0e3c rejects out-of-range lighting",
                !alphaExecutor.submit(execution, alphaTargets, invalidLighting,
                             ProductionAlphaLimits{}));
        ProductionAlphaLimits alphaLimits;
        alphaLimits.maxDraws = 1;
        ensure("P0e3c rejects over-budget alpha work",
               alphaExecutor.submit(execution, alphaTargets, alphaLighting,
                                    alphaLimits).code() == StatusCode::Unsupported);
        ensure("P0e3c destroys executor resources explicitly",
               alphaExecutor.shutdown().ok());
    #if defined(LL_GHI_P0_ALPHA_SHADER_PACKAGE)
        ShaderPackageDesc ppllPackage;
        alphaStatus = loadShaderPackage(
            LL_GHI_P0_ALPHA_SHADER_PACKAGE, ppllPackage);
        ensure(alphaStatus.message(), alphaStatus.ok());
        ProductionAlphaExecutor ppllExecutor(
            *alphaDevice.device, std::move(ppllPackage));
        alphaStatus = ppllExecutor.submit(
            execution, alphaTargets, alphaLighting, ProductionAlphaLimits{});
        ensure(alphaStatus.message(), alphaStatus.ok());
        ProductionAlphaResult ppllResult;
        alphaStatus = ppllExecutor.poll(ppllResult);
        ensure(alphaStatus.message(), alphaStatus.ok());
        ensure("P0e3d publishes exact-method availability",
               ppllResult.ppllAvailable);
        ensure_equals("P0e3d captures only standard alpha",
                      ppllResult.ppllDraws, std::uint32_t{1});
        ensure_equals("P0e3d removes exact work from sorted fallback",
                      ppllResult.sortedDraws, std::uint32_t{0});
        ensure_equals("P0e3d preserves particle residual work",
                      ppllResult.residualDraws, std::uint32_t{1});
        ensure_equals("P0e3d retains mask ownership upstream",
                      ppllResult.maskDraws, std::uint32_t{1});
        ensure_equals("P0e3d uses the bounded packet exact-layer policy",
                      ppllResult.ppllExactLayers,
                      execution.ppllPolicy.exactLayersPerPixel);
        ensure_equals("P0e3d plans the requested node capacity",
                      ppllResult.ppllNodeCapacity,
                      std::uint64_t{32 * 32 * 8});
        ensure_equals("P0e3d defers no synthetic production work",
                      ppllResult.deferredDraws, std::uint32_t{0});
        ensure("P0e3d destroys exact-method resources explicitly",
               ppllExecutor.shutdown().ok());
    #endif
    #if defined(LL_GHI_P0_ALPHA_PEEL_SHADER_PACKAGE)
        AlphaScenePacket peelExecution = execution;
        peelExecution.requestedMethod = AlphaMethod::DepthPeeling;
        ShaderPackageDesc peelPackage;
        alphaStatus = loadShaderPackage(
            LL_GHI_P0_ALPHA_PEEL_SHADER_PACKAGE, peelPackage);
        ensure(alphaStatus.message(), alphaStatus.ok());
        ProductionAlphaExecutor peelExecutor(
            *alphaDevice.device, std::move(peelPackage));
        alphaStatus = peelExecutor.submit(
            peelExecution, alphaTargets, alphaLighting,
            ProductionAlphaLimits{});
        ensure(alphaStatus.message(), alphaStatus.ok());
        ProductionAlphaResult peelResult;
        alphaStatus = peelExecutor.poll(peelResult);
        ensure(alphaStatus.message(), alphaStatus.ok());
        ensure("P0e3e publishes depth-peel availability",
               peelResult.depthPeelingAvailable);
        ensure_equals("P0e3e peels only standard alpha",
                      peelResult.depthPeelDraws, std::uint32_t{1});
        ensure_equals("P0e3e removes exact work from sorted fallback",
                      peelResult.sortedDraws, std::uint32_t{0});
        ensure_equals("P0e3e preserves particle residual work",
                      peelResult.residualDraws, std::uint32_t{1});
        ensure_equals("P0e3e uses the accepted maximum layer count",
                      peelResult.depthPeelLayers,
                      peelExecution.depthPeelPolicy.maximumLayers);
        ensure("P0e3e renders the filtered legacy tail",
               peelResult.depthPeelTailRendered);
         ensure("P0e3e synthetic submission stays within budget",
             !peelResult.depthPeelBudgetExhausted);
        ensure_equals("P0e3e defers no synthetic production work",
                      peelResult.deferredDraws, std::uint32_t{0});
        ensure("P0e3e destroys peel resources explicitly",
               peelExecutor.shutdown().ok());
    #endif
        ensure("P0e3c destroys shared alpha views",
               alphaDevice.device->destroy(alphaTargets.depthView).ok() &&
               alphaDevice.device->destroy(alphaTargets.lightingView).ok());
        ensure("P0e3c destroys shared alpha images",
               alphaDevice.device->destroy(alphaTargets.depthImage).ok() &&
               alphaDevice.device->destroy(alphaTargets.lightingImage).ok());
        ensure("P0e3c drains deferred resources",
               alphaDevice.device->waitIdle().ok());
    #endif
    first.pop_back();
    ensure("P0e3 rejects truncated alpha scenes",
           !decodeAlphaScenePacket(first, decoded));
}

} // namespace tut
