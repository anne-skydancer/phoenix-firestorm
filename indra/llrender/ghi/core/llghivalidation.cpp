/**
 * @file llghivalidation.cpp
 * @brief Non-rendering GHI contract validator and trace device.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "llghivalidation.h"

#include <memory>
#include <string>
#include <utility>

namespace LL::GHI
{

namespace
{
Status invalidState(std::string message)
{
    return Status::failure(StatusCode::InvalidState, std::move(message));
}

Status invalidHandle(std::string message)
{
    return Status::failure(StatusCode::InvalidHandle, std::move(message));
}
} // namespace

ValidationCommandContext::ValidationCommandContext(ValidationDevice& device) :
    mDevice(device)
{
}

Status ValidationCommandContext::beginFrame()
{
    if (mFrameActive)
    {
        return invalidState("beginFrame called while a frame is active");
    }

    mTrace.reset();
    mTrace.beginFrame();
    mPipeline = {};
    mIndexBuffer = {};
    mFrameActive = true;
    return Status::success();
}

Status ValidationCommandContext::endFrame()
{
    if (!mFrameActive)
    {
        return invalidState("endFrame called without an active frame");
    }
    if (mRendering)
    {
        return invalidState("endFrame called inside a rendering pass");
    }

    mTrace.endFrame();
    mFrameActive = false;
    return Status::success();
}

Status ValidationCommandContext::beginRendering(const RenderingInfo& info)
{
    if (!mFrameActive)
    {
        return invalidState("beginRendering called outside a frame");
    }
    if (mRendering)
    {
        return invalidState("rendering passes may not be nested");
    }
    if (info.width == 0 || info.height == 0)
    {
        return Status::failure(
            StatusCode::InvalidArgument,
            "rendering extent must be nonzero");
    }
    if (info.colors.empty() && !info.depthStencil)
    {
        return Status::failure(
            StatusCode::InvalidArgument,
            "rendering pass must declare at least one attachment");
    }

    for (const AttachmentDesc& attachment : info.colors)
    {
        if (!mDevice.imageMatches(
                attachment.image,
                attachment.format,
                ResourceUsage::ColorAttachment))
        {
            return invalidHandle("invalid or incompatible color attachment");
        }
    }
    if (info.depthStencil &&
        !mDevice.imageMatches(
            info.depthStencil->image,
            info.depthStencil->format,
            ResourceUsage::DepthStencilAttachment))
    {
        return invalidHandle("invalid or incompatible depth/stencil attachment");
    }

    mTrace.beginRendering(info);
    mRenderingInfo = info;
    mPipeline = {};
    mIndexBuffer = {};
    mRendering = true;
    return Status::success();
}

Status ValidationCommandContext::endRendering()
{
    Status status = requireRendering("endRendering");
    if (!status)
    {
        return status;
    }

    mTrace.endRendering();
    mRenderingInfo = {};
    mRendering = false;
    return Status::success();
}

Status ValidationCommandContext::requireRendering(const char* operation) const
{
    if (!mFrameActive || !mRendering)
    {
        return invalidState(std::string(operation) + " called outside a rendering pass");
    }
    return Status::success();
}

Status ValidationCommandContext::bindPipeline(PipelineHandle pipeline)
{
    Status status = requireRendering("bindPipeline");
    if (!status)
    {
        return status;
    }
    if (!mDevice.isLive(pipeline))
    {
        return invalidHandle("bindPipeline received a stale or invalid pipeline handle");
    }
    if (!mDevice.pipelineMatches(pipeline, mRenderingInfo))
    {
        return Status::failure(
            StatusCode::InvalidArgument,
            "pipeline attachment formats or sample count do not match the rendering pass");
    }

    mPipeline = pipeline;
    mTrace.bindPipeline(pipeline);
    return Status::success();
}

Status ValidationCommandContext::bindVertexBuffer(
    std::uint32_t slot,
    BufferHandle buffer,
    std::uint64_t offset)
{
    Status status = requireRendering("bindVertexBuffer");
    if (!status)
    {
        return status;
    }
    if (!mDevice.bufferSupports(buffer, ResourceUsage::Vertex))
    {
        return invalidHandle("vertex buffer is stale, invalid, or lacks vertex usage");
    }

    mTrace.bindVertexBuffer(slot, buffer, offset);
    return Status::success();
}

Status ValidationCommandContext::bindIndexBuffer(
    BufferHandle buffer,
    std::uint64_t offset,
    IndexType type)
{
    Status status = requireRendering("bindIndexBuffer");
    if (!status)
    {
        return status;
    }
    if (!mDevice.bufferSupports(buffer, ResourceUsage::Index))
    {
        return invalidHandle("index buffer is stale, invalid, or lacks index usage");
    }

    mIndexBuffer = buffer;
    mTrace.bindIndexBuffer(buffer, offset, type);
    return Status::success();
}

Status ValidationCommandContext::draw(const DrawArguments& arguments)
{
    Status status = requireRendering("draw");
    if (!status)
    {
        return status;
    }
    if (!mPipeline)
    {
        return invalidState("draw requires a bound pipeline");
    }
    if (arguments.vertexCount == 0 || arguments.instanceCount == 0)
    {
        return Status::failure(
            StatusCode::InvalidArgument,
            "draw counts must be nonzero");
    }

    mTrace.draw(arguments);
    return Status::success();
}

Status ValidationCommandContext::drawIndexed(const DrawIndexedArguments& arguments)
{
    Status status = requireRendering("drawIndexed");
    if (!status)
    {
        return status;
    }
    if (!mPipeline)
    {
        return invalidState("drawIndexed requires a bound pipeline");
    }
    if (!mIndexBuffer)
    {
        return invalidState("drawIndexed requires a bound index buffer");
    }
    if (arguments.indexCount == 0 || arguments.instanceCount == 0)
    {
        return Status::failure(
            StatusCode::InvalidArgument,
            "indexed draw counts must be nonzero");
    }

    mTrace.drawIndexed(arguments);
    return Status::success();
}

ValidationDevice::ValidationDevice(const DeviceCreateInfo& info) :
    mCommands(*this)
{
    mCapabilities.maxFramesInFlight = info.framesInFlight;
    mCapabilities.maxColorAttachments = 8;
    mCapabilities.maxSampledImagesPerStage = 32;
    mCapabilities.maxStorageBuffersPerStage = 16;
    mCapabilities.maxTexture2DSize = 16384;
    mCapabilities.maxUniformBufferSize = 65536;
    mCapabilities.maxVaryingVectors = 32;
    mCapabilities.maxSamples = 8;
    mCapabilities.maxBufferSize = std::uint64_t{1} << 40;
    mCapabilities.timestampQueries = true;
    mCapabilities.occlusionQueries = true;
    mCapabilities.descriptorIndexing = true;
    mCapabilities.storageImageAtomics = true;
    mCapabilities.depthClamp = true;
}

Status ValidationDevice::canMutateResources() const
{
    if (mCommands.frameActive())
    {
        return invalidState("R0 validation resources may not change during an active frame");
    }
    return Status::success();
}

BufferHandle ValidationDevice::createBuffer(const BufferDesc& desc, Status& status)
{
    status = canMutateResources();
    if (!status)
    {
        return {};
    }
    if (desc.size == 0 || desc.usage == ResourceUsage::None)
    {
        status = Status::failure(
            StatusCode::InvalidArgument,
            "buffer size and usage must be nonzero");
        return {};
    }

    BufferHandle handle = mBuffers.allocate();
    mBufferDescs.emplace(key(handle), desc);
    status = Status::success();
    return handle;
}

ImageHandle ValidationDevice::createImage(const ImageDesc& desc, Status& status)
{
    status = canMutateResources();
    if (!status)
    {
        return {};
    }
    if (desc.extent.width == 0 || desc.extent.height == 0 ||
        desc.extent.depth == 0 || desc.format == Format::Undefined ||
        desc.usage == ResourceUsage::None || desc.mipLevels == 0 ||
        desc.arrayLayers == 0 || desc.samples == 0)
    {
        status = Status::failure(StatusCode::InvalidArgument, "invalid image descriptor");
        return {};
    }

    ImageHandle handle = mImages.allocate();
    mImageDescs.emplace(key(handle), desc);
    status = Status::success();
    return handle;
}

SamplerHandle ValidationDevice::createSampler(const SamplerDesc& desc, Status& status)
{
    status = canMutateResources();
    if (!status)
    {
        return {};
    }
    if (desc.maxAnisotropy < 1.f)
    {
        status = Status::failure(
            StatusCode::InvalidArgument,
            "sampler anisotropy must be at least one");
        return {};
    }

    status = Status::success();
    return mSamplers.allocate();
}

ShaderPackageHandle ValidationDevice::createShaderPackage(
    const ShaderPackageDesc&,
    Status& status)
{
    status = canMutateResources();
    if (!status)
    {
        return {};
    }

    status = Status::success();
    return mShaders.allocate();
}

PipelineHandle ValidationDevice::createPipeline(const PipelineDesc& desc, Status& status)
{
    status = canMutateResources();
    if (!status)
    {
        return {};
    }
    if (!mShaders.isLive(desc.shader))
    {
        status = invalidHandle("pipeline references a stale or invalid shader package");
        return {};
    }
    if (desc.colorFormats.size() != desc.blendStates.size() || desc.samples == 0)
    {
        status = Status::failure(
            StatusCode::InvalidArgument,
            "pipeline color formats and blend states must correspond");
        return {};
    }
    if (desc.colorFormats.empty() && !desc.depthStencilFormat)
    {
        status = Status::failure(
            StatusCode::InvalidArgument,
            "graphics pipeline must declare at least one attachment format");
        return {};
    }

    PipelineHandle handle = mPipelines.allocate();
    mPipelineDescs.emplace(key(handle), desc);
    status = Status::success();
    return handle;
}

Status ValidationDevice::destroy(BufferHandle handle)
{
    Status status = canMutateResources();
    if (!status)
    {
        return status;
    }
    if (!mBuffers.release(handle))
    {
        return invalidHandle("destroy received a stale or invalid buffer handle");
    }
    mBufferDescs.erase(key(handle));
    return Status::success();
}

Status ValidationDevice::destroy(ImageHandle handle)
{
    Status status = canMutateResources();
    if (!status)
    {
        return status;
    }
    if (!mImages.release(handle))
    {
        return invalidHandle("destroy received a stale or invalid image handle");
    }
    mImageDescs.erase(key(handle));
    return Status::success();
}

Status ValidationDevice::destroy(SamplerHandle handle)
{
    Status status = canMutateResources();
    if (!status)
    {
        return status;
    }
    return mSamplers.release(handle) ? Status::success() :
        invalidHandle("destroy received a stale or invalid sampler handle");
}

Status ValidationDevice::destroy(ShaderPackageHandle handle)
{
    Status status = canMutateResources();
    if (!status)
    {
        return status;
    }
    return mShaders.release(handle) ? Status::success() :
        invalidHandle("destroy received a stale or invalid shader package handle");
}

Status ValidationDevice::destroy(PipelineHandle handle)
{
    Status status = canMutateResources();
    if (!status)
    {
        return status;
    }
    if (!mPipelines.release(handle))
    {
        return invalidHandle("destroy received a stale or invalid pipeline handle");
    }
    mPipelineDescs.erase(key(handle));
    return Status::success();
}

bool ValidationDevice::bufferSupports(BufferHandle handle, ResourceUsage usage) const
{
    auto found = mBufferDescs.find(key(handle));
    return mBuffers.isLive(handle) && found != mBufferDescs.end() &&
           hasUsage(found->second.usage, usage);
}

bool ValidationDevice::imageMatches(
    ImageHandle handle,
    Format format,
    ResourceUsage usage) const
{
    auto found = mImageDescs.find(key(handle));
    return mImages.isLive(handle) && found != mImageDescs.end() &&
           found->second.format == format && hasUsage(found->second.usage, usage);
}

bool ValidationDevice::pipelineMatches(
    PipelineHandle handle,
    const RenderingInfo& rendering) const
{
    auto pipeline = mPipelineDescs.find(key(handle));
    if (!mPipelines.isLive(handle) || pipeline == mPipelineDescs.end() ||
        pipeline->second.colorFormats.size() != rendering.colors.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < rendering.colors.size(); ++i)
    {
        if (pipeline->second.colorFormats[i] != rendering.colors[i].format)
        {
            return false;
        }
        auto image = mImageDescs.find(key(rendering.colors[i].image));
        if (image == mImageDescs.end() || image->second.samples != pipeline->second.samples)
        {
            return false;
        }
    }

    const std::optional<Format> depth_format = rendering.depthStencil
        ? std::optional<Format>{rendering.depthStencil->format}
        : std::nullopt;
    if (pipeline->second.depthStencilFormat != depth_format)
    {
        return false;
    }
    if (rendering.depthStencil)
    {
        auto image = mImageDescs.find(key(rendering.depthStencil->image));
        if (image == mImageDescs.end() || image->second.samples != pipeline->second.samples)
        {
            return false;
        }
    }

    return true;
}

DeviceCreationResult createDevice(const DeviceCreateInfo& info)
{
    if (info.framesInFlight == 0 || info.framesInFlight > 16)
    {
        return {
            nullptr,
            Status::failure(
                StatusCode::InvalidArgument,
                "framesInFlight must be between one and sixteen")};
    }
    if (info.backend != Backend::Validation)
    {
        return {
            nullptr,
            Status::failure(
                StatusCode::Unsupported,
                "R0 provides only the non-rendering validation backend")};
    }

    return {
        std::make_unique<ValidationDevice>(info),
        Status::success()};
}

} // namespace LL::GHI
