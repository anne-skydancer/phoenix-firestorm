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
#include "ghi/core/llghivalidation.h"

#include <memory>
#include <string>

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
            {color, Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store, {}});

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
        {color, Format::RGBA8UNorm, LoadOp::Clear, StoreOp::Store, {}});

    CommandContext& commands = device->commandContext();
    ensure("begin frame", commands.beginFrame().ok());
    ensure("begin rendering", commands.beginRendering(pass).ok());
    ensure(
        "incompatible pipeline is rejected",
        commands.bindPipeline(pipeline).code() == StatusCode::InvalidArgument);
    ensure("end rendering", commands.endRendering().ok());
    ensure("end frame", commands.endFrame().ok());
}

} // namespace tut
