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

#include <algorithm>
#include <cstring>
#include <limits>
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

Status invalidArgument(std::string message)
{
    return Status::failure(StatusCode::InvalidArgument, std::move(message));
}

Status unsupported(std::string message)
{
    return Status::failure(StatusCode::Unsupported, std::move(message));
}

bool rangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t total)
{
    return offset <= total && size <= total - offset;
}

std::uint32_t formatBytesPerTexel(Format format)
{
    switch (format)
    {
        case Format::R8UNorm: return 1;
        case Format::RG8UNorm:
        case Format::R16Float:
        case Format::Depth16UNorm: return 2;
        case Format::RGBA8UNorm:
        case Format::RGBA8SRGB:
        case Format::BGRA8UNorm:
        case Format::BGRA8SRGB:
        case Format::RG16Float:
        case Format::R32Float:
        case Format::R32UInt:
        case Format::Depth24Stencil8:
        case Format::Depth32Float: return 4;
        case Format::RGBA16Float:
        case Format::RG32Float:
        case Format::Depth32FloatStencil8: return 8;
        case Format::RGBA32Float: return 16;
        case Format::Undefined: break;
    }
    return 0;
}

bool isColorFormat(Format format)
{
    return format != Format::Undefined &&
           format != Format::Depth16UNorm &&
           format != Format::Depth24Stencil8 &&
           format != Format::Depth32Float &&
           format != Format::Depth32FloatStencil8;
}

bool aspectMatchesFormat(ImageAspect aspect, Format format)
{
    if (isColorFormat(format))
    {
        return aspect == ImageAspect::Color;
    }
    if (format == Format::Depth24Stencil8 || format == Format::Depth32FloatStencil8)
    {
        return aspect == ImageAspect::Depth || aspect == ImageAspect::Stencil ||
               aspect == ImageAspect::DepthStencil;
    }
    return aspect == ImageAspect::Depth;
}

Extent3D mipExtent(const ImageDesc& desc, std::uint16_t level)
{
    return {
        std::max(1u, desc.extent.width >> level),
        std::max(1u, desc.extent.height >> level),
        std::max(1u, desc.extent.depth >> level)
    };
}

std::uint32_t subresourceKey(std::uint16_t mip, std::uint16_t layer)
{
    return (static_cast<std::uint32_t>(layer) << 16) | mip;
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
    mIndexOffset = 0;
    mIndexType = IndexType::UInt16;
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
    mDevice.completeFrame();
    return Status::success();
}

Status ValidationCommandContext::requireTransfer(const char* operation) const
{
    if (!mFrameActive)
    {
        return invalidState(std::string(operation) + " called outside a frame");
    }
    if (mRendering)
    {
        return invalidState(std::string(operation) + " called inside a rendering pass");
    }
    return Status::success();
}

Status ValidationCommandContext::copyBuffer(
    BufferHandle source,
    BufferHandle destination,
    std::span<const BufferCopyRegion> regions)
{
    Status status = requireTransfer("copyBuffer");
    if (!status) return status;
    status = mDevice.executeCopyBuffer(source, destination, regions);
    if (status) mTrace.copyBuffer(source, destination, regions);
    return status;
}

Status ValidationCommandContext::copyBufferToImage(
    BufferHandle source,
    ImageHandle destination,
    std::span<const BufferImageCopyRegion> regions)
{
    Status status = requireTransfer("copyBufferToImage");
    if (!status) return status;
    status = mDevice.executeCopyBufferToImage(source, destination, regions);
    if (status) mTrace.copyBufferToImage(source, destination, regions);
    return status;
}

Status ValidationCommandContext::copyImageToBuffer(
    ImageHandle source,
    BufferHandle destination,
    std::span<const BufferImageCopyRegion> regions)
{
    Status status = requireTransfer("copyImageToBuffer");
    if (!status) return status;
    status = mDevice.executeCopyImageToBuffer(source, destination, regions);
    if (status) mTrace.copyImageToBuffer(source, destination, regions);
    return status;
}

Status ValidationCommandContext::generateMipmaps(
    ImageHandle image,
    const ImageSubresourceRange& subresources)
{
    Status status = requireTransfer("generateMipmaps");
    if (!status) return status;
    status = mDevice.executeGenerateMipmaps(image, subresources);
    if (status) mTrace.generateMipmaps(image, subresources);
    return status;
}

Status ValidationCommandContext::resetQueryPool(
    QueryPoolHandle pool,
    std::uint32_t firstQuery,
    std::uint32_t queryCount)
{
    Status status = requireTransfer("resetQueryPool");
    if (!status) return status;
    status = mDevice.resetQueries(pool, firstQuery, queryCount);
    if (status) mTrace.resetQueryPool(pool, firstQuery, queryCount);
    return status;
}

Status ValidationCommandContext::writeTimestamp(
    QueryPoolHandle pool,
    std::uint32_t query)
{
    if (!mFrameActive)
    {
        return invalidState("writeTimestamp called outside a frame");
    }
    Status status = mDevice.recordTimestamp(pool, query);
    if (status) mTrace.writeTimestamp(pool, query);
    return status;
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
        if (!mDevice.imageViewMatches(
                attachment.view,
                attachment.format,
                ResourceUsage::ColorAttachment))
        {
            return invalidHandle("invalid or incompatible color attachment");
        }
    }
    if (info.depthStencil &&
        !mDevice.imageViewMatches(
            info.depthStencil->view,
            info.depthStencil->format,
            ResourceUsage::DepthStencilAttachment))
    {
        return invalidHandle("invalid or incompatible depth/stencil attachment");
    }

    mTrace.beginRendering(info);
    mRenderingInfo = info;
    mPipeline = {};
    mIndexBuffer = {};
    mIndexOffset = 0;
    mIndexType = IndexType::UInt16;
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

Status ValidationCommandContext::bindBindingSet(
    std::uint8_t,
    BindingSetHandle,
    std::span<const std::uint32_t>)
{
    Status status = requireRendering("bindBindingSet");
    if (!status) return status;
    return unsupported("validation binding sets begin in R3c");
}

Status ValidationCommandContext::setViewport(const Viewport& viewport)
{
    Status status = requireRendering("setViewport");
    if (!status) return status;
    if (viewport.width <= 0.f || viewport.height <= 0.f ||
        viewport.minDepth < 0.f || viewport.maxDepth > 1.f ||
        viewport.minDepth > viewport.maxDepth)
    {
        return invalidArgument("invalid viewport");
    }
    mTrace.setViewport(viewport);
    return Status::success();
}

Status ValidationCommandContext::setScissor(const ScissorRect& scissor)
{
    Status status = requireRendering("setScissor");
    if (!status) return status;
    if (!scissor.width || !scissor.height)
    {
        return invalidArgument("invalid scissor rectangle");
    }
    mTrace.setScissor(scissor);
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
    if (offset >= mDevice.bufferSize(buffer))
    {
        return invalidArgument("vertex buffer offset is outside the buffer");
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
    const std::uint64_t index_size = type == IndexType::UInt16 ? 2 : 4;
    if (offset >= mDevice.bufferSize(buffer) || offset % index_size != 0)
    {
        return invalidArgument("index buffer offset is outside the buffer or misaligned");
    }

    mIndexBuffer = buffer;
    mIndexOffset = offset;
    mIndexType = type;
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
    const std::uint64_t index_size = mIndexType == IndexType::UInt16 ? 2 : 4;
    const std::uint64_t first_byte =
        mIndexOffset + static_cast<std::uint64_t>(arguments.firstIndex) * index_size;
    const std::uint64_t byte_count =
        static_cast<std::uint64_t>(arguments.indexCount) * index_size;
    if (!rangeFits(first_byte, byte_count, mDevice.bufferSize(mIndexBuffer)))
    {
        return invalidArgument("indexed draw exceeds the bound index buffer");
    }

    mTrace.drawIndexed(arguments);
    return Status::success();
}

ValidationDevice::ValidationDevice(const DeviceCreateInfo& info) :
    mFramesInFlight(info.framesInFlight),
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
    mCapabilities.timestampPeriodNanoseconds = 1.0;
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
    if (desc.size == 0 || desc.usage == ResourceUsage::None ||
        desc.size > mCapabilities.maxBufferSize)
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
    std::uint32_t max_dimension = std::max(
        {desc.extent.width, desc.extent.height, desc.extent.depth});
    std::uint16_t max_mips = 1;
    while (max_dimension > 1)
    {
        max_dimension >>= 1;
        ++max_mips;
    }
    if (desc.extent.width == 0 || desc.extent.height == 0 ||
        desc.extent.depth == 0 || desc.format == Format::Undefined ||
        desc.usage == ResourceUsage::None || desc.mipLevels == 0 ||
        desc.arrayLayers == 0 || desc.samples == 0 ||
        desc.extent.width > mCapabilities.maxTexture2DSize ||
        desc.extent.height > mCapabilities.maxTexture2DSize ||
        desc.mipLevels > max_mips || desc.samples > mCapabilities.maxSamples ||
        (desc.samples & (desc.samples - 1)) != 0 ||
        (desc.samples > 1 && desc.mipLevels != 1))
    {
        status = Status::failure(StatusCode::InvalidArgument, "invalid image descriptor");
        return {};
    }

    ImageHandle handle = mImages.allocate();
    mImageDescs.emplace(key(handle), desc);
    status = Status::success();
    return handle;
}

ImageViewHandle ValidationDevice::createImageView(
    const ImageViewDesc& desc,
    Status& status)
{
    status = canMutateResources();
    if (!status) return {};

    auto image = mImageDescs.find(key(desc.image));
    const ImageSubresourceRange& range = desc.subresources;
    if (!mImages.isLive(desc.image) || image == mImageDescs.end())
    {
        status = invalidHandle("image view references a stale or invalid image");
        return {};
    }
    if (desc.format != image->second.format ||
        !aspectMatchesFormat(range.aspect, desc.format) ||
        range.mipLevelCount == 0 || range.arrayLayerCount == 0 ||
        range.baseMipLevel >= image->second.mipLevels ||
        range.mipLevelCount > image->second.mipLevels - range.baseMipLevel ||
        range.baseArrayLayer >= image->second.arrayLayers ||
        range.arrayLayerCount > image->second.arrayLayers - range.baseArrayLayer)
    {
        status = invalidArgument("invalid or incompatible image view descriptor");
        return {};
    }

    ImageViewHandle handle = mImageViews.allocate();
    mImageViewDescs.emplace(key(handle), desc);
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

QueryPoolHandle ValidationDevice::createQueryPool(
    const QueryPoolDesc& desc,
    Status& status)
{
    status = canMutateResources();
    if (!status) return {};
    if (desc.count == 0)
    {
        status = invalidArgument("query pool count must be nonzero");
        return {};
    }
    if (!mCapabilities.timestampQueries)
    {
        status = Status::failure(StatusCode::Unsupported, "query type is unsupported");
        return {};
    }

    QueryPoolHandle handle = mQueryPools.allocate();
    QueryRecord record;
    record.desc = desc;
    record.values.resize(desc.count);
    record.available.resize(desc.count, false);
    record.pending.resize(desc.count, false);
    mQueryRecords.emplace(key(handle), std::move(record));
    status = Status::success();
    return handle;
}

ShaderPackageHandle ValidationDevice::createShaderPackage(
    const ShaderPackageDesc& desc,
    Status& status)
{
    status = canMutateResources();
    if (!status)
    {
        return {};
    }

    ShaderPackageHandle handle = mShaders.allocate();
    mShaderDescs.emplace(key(handle), desc);
    status = Status::success();
    return handle;
}

BindingSetHandle ValidationDevice::createBindingSet(
    const BindingSetDesc&,
    Status& status)
{
    status = canMutateResources();
    if (status) status = unsupported("validation binding sets begin in R3c");
    return {};
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
    mBufferData.erase(key(handle));
    mBufferReadyFrame.erase(key(handle));
    retire();
    return Status::success();
}

Status ValidationDevice::destroy(ImageHandle handle)
{
    Status status = canMutateResources();
    if (!status)
    {
        return status;
    }
    for (const auto& [unused, view] : mImageViewDescs)
    {
        if (view.image == handle)
        {
            return invalidState("image must outlive its image views");
        }
    }
    if (!mImages.release(handle))
    {
        return invalidHandle("destroy received a stale or invalid image handle");
    }
    mImageDescs.erase(key(handle));
    mImageData.erase(key(handle));
    retire();
    return Status::success();
}

Status ValidationDevice::destroy(ImageViewHandle handle)
{
    Status status = canMutateResources();
    if (!status) return status;
    if (!mImageViews.release(handle))
    {
        return invalidHandle("destroy received a stale or invalid image view handle");
    }
    mImageViewDescs.erase(key(handle));
    retire();
    return Status::success();
}

Status ValidationDevice::destroy(SamplerHandle handle)
{
    Status status = canMutateResources();
    if (!status)
    {
        return status;
    }
    if (!mSamplers.release(handle))
    {
        return invalidHandle("destroy received a stale or invalid sampler handle");
    }
    retire();
    return Status::success();
}

Status ValidationDevice::destroy(QueryPoolHandle handle)
{
    Status status = canMutateResources();
    if (!status) return status;
    if (!mQueryPools.release(handle))
    {
        return invalidHandle("destroy received a stale or invalid query pool handle");
    }
    mQueryRecords.erase(key(handle));
    retire();
    return Status::success();
}

Status ValidationDevice::destroy(ShaderPackageHandle handle)
{
    Status status = canMutateResources();
    if (!status)
    {
        return status;
    }
    for (const auto& [unused, pipeline] : mPipelineDescs)
    {
        if (pipeline.shader == handle)
        {
            return invalidState("shader package must outlive its pipelines");
        }
    }
    if (!mShaders.release(handle))
    {
        return invalidHandle("destroy received a stale or invalid shader package handle");
    }
    mShaderDescs.erase(key(handle));
    retire();
    return Status::success();
}

Status ValidationDevice::destroy(BindingSetHandle)
{
    Status status = canMutateResources();
    if (!status) return status;
    return unsupported("validation binding sets begin in R3c");
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
    retire();
    return Status::success();
}

Status ValidationDevice::writeBuffer(
    BufferHandle handle,
    std::uint64_t offset,
    std::span<const std::byte> data)
{
    if (mCommands.frameActive())
    {
        return invalidState("host buffer writes are not allowed during an active frame");
    }
    auto found = mBufferDescs.find(key(handle));
    if (!mBuffers.isLive(handle) || found == mBufferDescs.end())
    {
        return invalidHandle("writeBuffer received a stale or invalid buffer handle");
    }
    if (found->second.memory != MemoryClass::Upload)
    {
        return invalidArgument("writeBuffer requires an upload buffer");
    }
    if (!rangeFits(offset, data.size(), found->second.size))
    {
        return invalidArgument("writeBuffer range exceeds the buffer");
    }
    std::vector<std::byte>& backing = mBufferData[key(handle)];
    const std::size_t end = static_cast<std::size_t>(offset + data.size());
    if (backing.size() < end) backing.resize(end);
    std::copy(data.begin(), data.end(), backing.begin() + static_cast<std::size_t>(offset));
    return Status::success();
}

Status ValidationDevice::readBuffer(
    BufferHandle handle,
    std::uint64_t offset,
    std::span<std::byte> data)
{
    if (mCommands.frameActive())
    {
        return invalidState("host buffer reads are not allowed during an active frame");
    }
    auto found = mBufferDescs.find(key(handle));
    if (!mBuffers.isLive(handle) || found == mBufferDescs.end())
    {
        return invalidHandle("readBuffer received a stale or invalid buffer handle");
    }
    if (found->second.memory != MemoryClass::Readback)
    {
        return invalidArgument("readBuffer requires a readback buffer");
    }
    if (!rangeFits(offset, data.size(), found->second.size))
    {
        return invalidArgument("readBuffer range exceeds the buffer");
    }
    const auto ready = mBufferReadyFrame.find(key(handle));
    if (ready != mBufferReadyFrame.end() && ready->second > mSubmittedFrames)
    {
        return Status::failure(StatusCode::NotReady, "readback buffer is not ready");
    }
    const auto backing = mBufferData.find(key(handle));
    std::fill(data.begin(), data.end(), std::byte{});
    if (backing != mBufferData.end() && offset < backing->second.size())
    {
        const std::size_t available = std::min<std::size_t>(
            data.size(), backing->second.size() - static_cast<std::size_t>(offset));
        std::copy_n(
            backing->second.begin() + static_cast<std::size_t>(offset),
            available,
            data.begin());
    }
    return Status::success();
}

Status ValidationDevice::getQueryResults(
    QueryPoolHandle pool,
    std::uint32_t firstQuery,
    std::span<std::uint64_t> results,
    QueryReadMode mode)
{
    if (mCommands.frameActive())
    {
        return invalidState("query results may not be read during an active frame");
    }
    auto found = mQueryRecords.find(key(pool));
    if (!mQueryPools.isLive(pool) || found == mQueryRecords.end())
    {
        return invalidHandle("getQueryResults received a stale or invalid query pool");
    }
    if (results.empty() || firstQuery >= found->second.desc.count ||
        results.size() > found->second.desc.count - firstQuery)
    {
        return invalidArgument("query result range is empty or out of bounds");
    }
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        const std::size_t index = firstQuery + i;
        if (!found->second.available[index])
        {
            if (mode == QueryReadMode::AvailableOnly)
            {
                return Status::failure(StatusCode::NotReady, "query results are not ready");
            }
            if (!found->second.pending[index])
            {
                return Status::failure(StatusCode::NotReady, "query has not been written");
            }
            found->second.available[index] = true;
            found->second.pending[index] = false;
        }
        results[i] = found->second.values[index];
    }
    return Status::success();
}

Status ValidationDevice::waitIdle()
{
    if (mCommands.frameActive())
    {
        return invalidState("waitIdle may not be called during an active frame");
    }
    for (auto& [unused, query] : mQueryRecords)
    {
        for (std::size_t i = 0; i < query.pending.size(); ++i)
        {
            if (query.pending[i])
            {
                query.pending[i] = false;
                query.available[i] = true;
            }
        }
    }
    mRetirements.clear();
    return Status::success();
}

void ValidationDevice::retire()
{
    mRetirements.push_back({mSubmittedFrames + mFramesInFlight});
}

void ValidationDevice::completeFrame()
{
    ++mSubmittedFrames;
    for (auto& [unused, query] : mQueryRecords)
    {
        for (std::size_t i = 0; i < query.pending.size(); ++i)
        {
            if (query.pending[i])
            {
                query.pending[i] = false;
                query.available[i] = true;
            }
        }
    }
    std::erase_if(mRetirements, [this](const Retirement& retirement)
    {
        return retirement.releaseAfterFrame <= mSubmittedFrames;
    });
}

std::uint64_t ValidationDevice::bufferSize(BufferHandle handle) const
{
    auto found = mBufferDescs.find(key(handle));
    return mBuffers.isLive(handle) && found != mBufferDescs.end()
        ? found->second.size
        : 0;
}

Status ValidationDevice::executeCopyBuffer(
    BufferHandle source,
    BufferHandle destination,
    std::span<const BufferCopyRegion> regions)
{
    if (!bufferSupports(source, ResourceUsage::TransferSource) ||
        !bufferSupports(destination, ResourceUsage::TransferDestination))
    {
        return invalidHandle("buffer copy requires live transfer source/destination buffers");
    }
    if (regions.empty())
    {
        return invalidArgument("buffer copy requires at least one region");
    }
    const std::uint64_t source_size = bufferSize(source);
    const std::uint64_t destination_size = bufferSize(destination);
    for (const BufferCopyRegion& region : regions)
    {
        if (region.size == 0 ||
            ((region.sourceOffset | region.destinationOffset | region.size) & 3u) != 0 ||
            !rangeFits(region.sourceOffset, region.size, source_size) ||
            !rangeFits(region.destinationOffset, region.size, destination_size))
        {
            return invalidArgument("buffer copy region is unaligned, empty, or out of bounds");
        }
        if (source == destination)
        {
            const std::uint64_t source_end = region.sourceOffset + region.size;
            const std::uint64_t destination_end = region.destinationOffset + region.size;
            if (region.sourceOffset < destination_end &&
                region.destinationOffset < source_end)
            {
                return invalidArgument("overlapping in-place buffer copies are not supported");
            }
        }
    }

    std::vector<std::byte>& source_data = mBufferData[key(source)];
    std::vector<std::byte>& destination_data = mBufferData[key(destination)];
    for (const BufferCopyRegion& region : regions)
    {
        const std::size_t source_offset = static_cast<std::size_t>(region.sourceOffset);
        const std::size_t destination_offset = static_cast<std::size_t>(region.destinationOffset);
        const std::size_t size = static_cast<std::size_t>(region.size);
        std::vector<std::byte> copy(size, std::byte{});
        if (source_offset < source_data.size())
        {
            const std::size_t available = std::min(size, source_data.size() - source_offset);
            std::copy_n(source_data.begin() + source_offset, available, copy.begin());
        }
        if (destination_data.size() < destination_offset + size)
        {
            destination_data.resize(destination_offset + size);
        }
        std::copy(copy.begin(), copy.end(), destination_data.begin() + destination_offset);
    }
    const auto destination_desc = mBufferDescs.find(key(destination));
    if (destination_desc->second.memory == MemoryClass::Readback)
    {
        mBufferReadyFrame[key(destination)] = mSubmittedFrames + 1;
    }
    return Status::success();
}

Status ValidationDevice::executeCopyBufferToImage(
    BufferHandle source,
    ImageHandle destination,
    std::span<const BufferImageCopyRegion> regions)
{
    if (!bufferSupports(source, ResourceUsage::TransferSource))
    {
        return invalidHandle("buffer-to-image copy requires a live transfer-source buffer");
    }
    auto image = mImageDescs.find(key(destination));
    if (!mImages.isLive(destination) || image == mImageDescs.end() ||
        !hasUsage(image->second.usage, ResourceUsage::TransferDestination))
    {
        return invalidHandle("buffer-to-image copy requires a live transfer-destination image");
    }
    if (regions.empty() || image->second.samples != 1)
    {
        return invalidArgument("buffer-to-image copy requires regions and a single-sample image");
    }
    const std::uint64_t buffer_size = bufferSize(source);
    const std::uint32_t texel_size = formatBytesPerTexel(image->second.format);
    for (const BufferImageCopyRegion& region : regions)
    {
        const ImageSubresourceLayers& layers = region.imageSubresource;
        if ((region.bufferOffset & 3u) != 0 ||
            !aspectMatchesFormat(layers.aspect, image->second.format) ||
            layers.mipLevel >= image->second.mipLevels ||
            layers.arrayLayerCount == 0 ||
            layers.baseArrayLayer >= image->second.arrayLayers ||
            layers.arrayLayerCount > image->second.arrayLayers - layers.baseArrayLayer ||
            region.imageOffset.x < 0 || region.imageOffset.y < 0 || region.imageOffset.z < 0 ||
            region.imageExtent.width == 0 || region.imageExtent.height == 0 ||
            region.imageExtent.depth == 0)
        {
            return invalidArgument("invalid buffer-to-image subresource region");
        }
        const Extent3D level_extent = mipExtent(image->second, layers.mipLevel);
        if (static_cast<std::uint32_t>(region.imageOffset.x) > level_extent.width ||
            region.imageExtent.width > level_extent.width - region.imageOffset.x ||
            static_cast<std::uint32_t>(region.imageOffset.y) > level_extent.height ||
            region.imageExtent.height > level_extent.height - region.imageOffset.y ||
            static_cast<std::uint32_t>(region.imageOffset.z) > level_extent.depth ||
            region.imageExtent.depth > level_extent.depth - region.imageOffset.z)
        {
            return invalidArgument("buffer-to-image region exceeds the image subresource");
        }
        const std::uint64_t row_texels = region.bufferRowLength
            ? region.bufferRowLength : region.imageExtent.width;
        const std::uint64_t image_rows = region.bufferImageHeight
            ? region.bufferImageHeight : region.imageExtent.height;
        if (row_texels < region.imageExtent.width || image_rows < region.imageExtent.height)
        {
            return invalidArgument("buffer image row/slice pitch is too small");
        }
        const std::uint64_t row_pitch = row_texels * texel_size;
        const std::uint64_t layer_pitch = row_pitch * image_rows * region.imageExtent.depth;
        const std::uint64_t required =
            (layers.arrayLayerCount - 1) * layer_pitch +
            (region.imageExtent.depth - 1) * row_pitch * image_rows +
            (region.imageExtent.height - 1) * row_pitch +
            static_cast<std::uint64_t>(region.imageExtent.width) * texel_size;
        if (!rangeFits(region.bufferOffset, required, buffer_size))
        {
            return invalidArgument("buffer-to-image region exceeds the source buffer");
        }
    }

    const std::vector<std::byte>& source_data = mBufferData[key(source)];
    auto& image_data = mImageData[key(destination)];
    for (const BufferImageCopyRegion& region : regions)
    {
        const ImageSubresourceLayers& layers = region.imageSubresource;
        const Extent3D level_extent = mipExtent(image->second, layers.mipLevel);
        const std::size_t image_row_pitch = level_extent.width * texel_size;
        const std::size_t image_slice_pitch = image_row_pitch * level_extent.height;
        const std::size_t subresource_size = image_slice_pitch * level_extent.depth;
        const std::size_t buffer_row_pitch =
            (region.bufferRowLength ? region.bufferRowLength : region.imageExtent.width) * texel_size;
        const std::size_t buffer_slice_pitch = buffer_row_pitch *
            (region.bufferImageHeight ? region.bufferImageHeight : region.imageExtent.height);
        const std::size_t buffer_layer_pitch = buffer_slice_pitch * region.imageExtent.depth;
        const std::size_t copy_bytes = region.imageExtent.width * texel_size;
        for (std::uint16_t layer = 0; layer < layers.arrayLayerCount; ++layer)
        {
            std::vector<std::byte>& target = image_data[
                subresourceKey(layers.mipLevel, layers.baseArrayLayer + layer)];
            target.resize(subresource_size);
            for (std::uint32_t z = 0; z < region.imageExtent.depth; ++z)
            {
                for (std::uint32_t y = 0; y < region.imageExtent.height; ++y)
                {
                    const std::size_t source_offset = static_cast<std::size_t>(region.bufferOffset) +
                        layer * buffer_layer_pitch + z * buffer_slice_pitch + y * buffer_row_pitch;
                    const std::size_t target_offset =
                        (region.imageOffset.z + z) * image_slice_pitch +
                        (region.imageOffset.y + y) * image_row_pitch +
                        region.imageOffset.x * texel_size;
                    std::fill_n(target.begin() + target_offset, copy_bytes, std::byte{});
                    if (source_offset < source_data.size())
                    {
                        const std::size_t available = std::min(
                            copy_bytes, source_data.size() - source_offset);
                        std::copy_n(source_data.begin() + source_offset, available,
                                    target.begin() + target_offset);
                    }
                }
            }
        }
    }
    return Status::success();
}

Status ValidationDevice::executeCopyImageToBuffer(
    ImageHandle source,
    BufferHandle destination,
    std::span<const BufferImageCopyRegion> regions)
{
    auto image = mImageDescs.find(key(source));
    if (!mImages.isLive(source) || image == mImageDescs.end() ||
        !hasUsage(image->second.usage, ResourceUsage::TransferSource))
    {
        return invalidHandle("image-to-buffer copy requires a live transfer-source image");
    }
    if (!bufferSupports(destination, ResourceUsage::TransferDestination))
    {
        return invalidHandle("image-to-buffer copy requires a live transfer-destination buffer");
    }
    // Validate the exact same region geometry by temporarily checking against
    // the destination buffer size, then execute the reverse row copies.
    if (regions.empty() || image->second.samples != 1)
    {
        return invalidArgument("image-to-buffer copy requires regions and a single-sample image");
    }
    const std::uint64_t buffer_size = bufferSize(destination);
    const std::uint32_t texel_size = formatBytesPerTexel(image->second.format);
    for (const BufferImageCopyRegion& region : regions)
    {
        const ImageSubresourceLayers& layers = region.imageSubresource;
        if ((region.bufferOffset & 3u) != 0 ||
            !aspectMatchesFormat(layers.aspect, image->second.format) ||
            layers.mipLevel >= image->second.mipLevels || layers.arrayLayerCount == 0 ||
            layers.baseArrayLayer >= image->second.arrayLayers ||
            layers.arrayLayerCount > image->second.arrayLayers - layers.baseArrayLayer ||
            region.imageOffset.x < 0 || region.imageOffset.y < 0 || region.imageOffset.z < 0 ||
            region.imageExtent.width == 0 || region.imageExtent.height == 0 ||
            region.imageExtent.depth == 0)
        {
            return invalidArgument("invalid image-to-buffer subresource region");
        }
        const Extent3D level_extent = mipExtent(image->second, layers.mipLevel);
        if (static_cast<std::uint32_t>(region.imageOffset.x) > level_extent.width ||
            region.imageExtent.width > level_extent.width - region.imageOffset.x ||
            static_cast<std::uint32_t>(region.imageOffset.y) > level_extent.height ||
            region.imageExtent.height > level_extent.height - region.imageOffset.y ||
            static_cast<std::uint32_t>(region.imageOffset.z) > level_extent.depth ||
            region.imageExtent.depth > level_extent.depth - region.imageOffset.z)
        {
            return invalidArgument("image-to-buffer region exceeds the image subresource");
        }
        const std::uint64_t row_texels = region.bufferRowLength
            ? region.bufferRowLength : region.imageExtent.width;
        const std::uint64_t image_rows = region.bufferImageHeight
            ? region.bufferImageHeight : region.imageExtent.height;
        if (row_texels < region.imageExtent.width || image_rows < region.imageExtent.height)
        {
            return invalidArgument("buffer image row/slice pitch is too small");
        }
        const std::uint64_t row_pitch = row_texels * texel_size;
        const std::uint64_t layer_pitch = row_pitch * image_rows * region.imageExtent.depth;
        const std::uint64_t required = (layers.arrayLayerCount - 1) * layer_pitch +
            (region.imageExtent.depth - 1) * row_pitch * image_rows +
            (region.imageExtent.height - 1) * row_pitch +
            static_cast<std::uint64_t>(region.imageExtent.width) * texel_size;
        if (!rangeFits(region.bufferOffset, required, buffer_size))
        {
            return invalidArgument("image-to-buffer region exceeds the destination buffer");
        }
    }

    const auto image_data = mImageData.find(key(source));
    std::vector<std::byte>& destination_data = mBufferData[key(destination)];
    for (const BufferImageCopyRegion& region : regions)
    {
        const ImageSubresourceLayers& layers = region.imageSubresource;
        const Extent3D level_extent = mipExtent(image->second, layers.mipLevel);
        const std::size_t image_row_pitch = level_extent.width * texel_size;
        const std::size_t image_slice_pitch = image_row_pitch * level_extent.height;
        const std::size_t buffer_row_pitch =
            (region.bufferRowLength ? region.bufferRowLength : region.imageExtent.width) * texel_size;
        const std::size_t buffer_slice_pitch = buffer_row_pitch *
            (region.bufferImageHeight ? region.bufferImageHeight : region.imageExtent.height);
        const std::size_t buffer_layer_pitch = buffer_slice_pitch * region.imageExtent.depth;
        const std::size_t copy_bytes = region.imageExtent.width * texel_size;
        const std::size_t required_end = static_cast<std::size_t>(region.bufferOffset) +
            (layers.arrayLayerCount - 1) * buffer_layer_pitch +
            (region.imageExtent.depth - 1) * buffer_slice_pitch +
            (region.imageExtent.height - 1) * buffer_row_pitch + copy_bytes;
        if (destination_data.size() < required_end) destination_data.resize(required_end);
        for (std::uint16_t layer = 0; layer < layers.arrayLayerCount; ++layer)
        {
            const std::vector<std::byte>* source_subresource = nullptr;
            if (image_data != mImageData.end())
            {
                auto found = image_data->second.find(
                    subresourceKey(layers.mipLevel, layers.baseArrayLayer + layer));
                if (found != image_data->second.end()) source_subresource = &found->second;
            }
            for (std::uint32_t z = 0; z < region.imageExtent.depth; ++z)
            {
                for (std::uint32_t y = 0; y < region.imageExtent.height; ++y)
                {
                    const std::size_t source_offset =
                        (region.imageOffset.z + z) * image_slice_pitch +
                        (region.imageOffset.y + y) * image_row_pitch +
                        region.imageOffset.x * texel_size;
                    const std::size_t destination_offset = static_cast<std::size_t>(region.bufferOffset) +
                        layer * buffer_layer_pitch + z * buffer_slice_pitch + y * buffer_row_pitch;
                    std::fill_n(destination_data.begin() + destination_offset, copy_bytes, std::byte{});
                    if (source_subresource && source_offset < source_subresource->size())
                    {
                        const std::size_t available = std::min(
                            copy_bytes, source_subresource->size() - source_offset);
                        std::copy_n(source_subresource->begin() + source_offset, available,
                                    destination_data.begin() + destination_offset);
                    }
                }
            }
        }
    }
    const auto destination_desc = mBufferDescs.find(key(destination));
    if (destination_desc->second.memory == MemoryClass::Readback)
    {
        mBufferReadyFrame[key(destination)] = mSubmittedFrames + 1;
    }
    return Status::success();
}

Status ValidationDevice::executeGenerateMipmaps(
    ImageHandle image_handle,
    const ImageSubresourceRange& subresources)
{
    auto image = mImageDescs.find(key(image_handle));
    if (!mImages.isLive(image_handle) || image == mImageDescs.end() ||
        !hasUsage(image->second.usage, ResourceUsage::TransferSource | ResourceUsage::TransferDestination))
    {
        return invalidHandle("mip generation requires a live transfer source/destination image");
    }
    const Format format = image->second.format;
    const bool byte_unorm = format == Format::R8UNorm || format == Format::RG8UNorm ||
        format == Format::RGBA8UNorm || format == Format::BGRA8UNorm;
    if (!byte_unorm)
    {
        return Status::failure(
            StatusCode::Unsupported,
            "validation mip generation currently supports byte UNorm formats");
    }
    if (subresources.aspect != ImageAspect::Color || subresources.mipLevelCount < 2 ||
        subresources.arrayLayerCount == 0 ||
        subresources.baseMipLevel >= image->second.mipLevels ||
        subresources.mipLevelCount > image->second.mipLevels - subresources.baseMipLevel ||
        subresources.baseArrayLayer >= image->second.arrayLayers ||
        subresources.arrayLayerCount > image->second.arrayLayers - subresources.baseArrayLayer)
    {
        return invalidArgument("invalid mip generation subresource range");
    }

    const std::uint32_t channels = formatBytesPerTexel(format);
    auto& image_data = mImageData[key(image_handle)];
    for (std::uint16_t layer = 0; layer < subresources.arrayLayerCount; ++layer)
    {
        const std::uint16_t array_layer = subresources.baseArrayLayer + layer;
        for (std::uint16_t relative = 1; relative < subresources.mipLevelCount; ++relative)
        {
            const std::uint16_t source_mip = subresources.baseMipLevel + relative - 1;
            const std::uint16_t destination_mip = source_mip + 1;
            const Extent3D source_extent = mipExtent(image->second, source_mip);
            const Extent3D destination_extent = mipExtent(image->second, destination_mip);
            std::vector<std::byte>& source = image_data[subresourceKey(source_mip, array_layer)];
            source.resize(static_cast<std::size_t>(source_extent.width) * source_extent.height *
                          source_extent.depth * channels);
            std::vector<std::byte>& destination = image_data[
                subresourceKey(destination_mip, array_layer)];
            destination.resize(static_cast<std::size_t>(destination_extent.width) *
                               destination_extent.height * destination_extent.depth * channels);
            for (std::uint32_t z = 0; z < destination_extent.depth; ++z)
            for (std::uint32_t y = 0; y < destination_extent.height; ++y)
            for (std::uint32_t x = 0; x < destination_extent.width; ++x)
            for (std::uint32_t channel = 0; channel < channels; ++channel)
            {
                std::uint32_t sum = 0;
                std::uint32_t samples = 0;
                for (std::uint32_t dz = 0; dz < 2; ++dz)
                for (std::uint32_t dy = 0; dy < 2; ++dy)
                for (std::uint32_t dx = 0; dx < 2; ++dx)
                {
                    const std::uint32_t sx = x * 2 + dx;
                    const std::uint32_t sy = y * 2 + dy;
                    const std::uint32_t sz = z * 2 + dz;
                    if (sx < source_extent.width && sy < source_extent.height &&
                        sz < source_extent.depth)
                    {
                        const std::size_t source_index =
                            (((static_cast<std::size_t>(sz) * source_extent.height + sy) *
                               source_extent.width + sx) * channels) + channel;
                        sum += std::to_integer<std::uint8_t>(source[source_index]);
                        ++samples;
                    }
                }
                const std::size_t destination_index =
                    (((static_cast<std::size_t>(z) * destination_extent.height + y) *
                       destination_extent.width + x) * channels) + channel;
                destination[destination_index] = std::byte(sum / samples);
            }
        }
    }
    return Status::success();
}

Status ValidationDevice::resetQueries(
    QueryPoolHandle pool,
    std::uint32_t first,
    std::uint32_t count)
{
    auto found = mQueryRecords.find(key(pool));
    if (!mQueryPools.isLive(pool) || found == mQueryRecords.end())
    {
        return invalidHandle("resetQueryPool received a stale or invalid query pool");
    }
    if (count == 0 || first >= found->second.desc.count ||
        count > found->second.desc.count - first)
    {
        return invalidArgument("query reset range is empty or out of bounds");
    }
    for (std::uint32_t query = first; query < first + count; ++query)
    {
        found->second.available[query] = false;
        found->second.pending[query] = false;
        found->second.values[query] = 0;
    }
    return Status::success();
}

Status ValidationDevice::recordTimestamp(QueryPoolHandle pool, std::uint32_t query)
{
    auto found = mQueryRecords.find(key(pool));
    if (!mQueryPools.isLive(pool) || found == mQueryRecords.end())
    {
        return invalidHandle("writeTimestamp received a stale or invalid query pool");
    }
    if (found->second.desc.type != QueryType::Timestamp || query >= found->second.desc.count)
    {
        return invalidArgument("timestamp query index or pool type is invalid");
    }
    if (found->second.pending[query] || found->second.available[query])
    {
        return invalidState("timestamp query must be reset before reuse");
    }
    found->second.values[query] = ++mTimestampCounter;
    found->second.pending[query] = true;
    return Status::success();
}

bool ValidationDevice::bufferSupports(BufferHandle handle, ResourceUsage usage) const
{
    auto found = mBufferDescs.find(key(handle));
    return mBuffers.isLive(handle) && found != mBufferDescs.end() &&
           hasUsage(found->second.usage, usage);
}

bool ValidationDevice::imageViewMatches(
    ImageViewHandle handle,
    Format format,
    ResourceUsage usage) const
{
    auto view = mImageViewDescs.find(key(handle));
    if (!mImageViews.isLive(handle) || view == mImageViewDescs.end() ||
        view->second.format != format)
    {
        return false;
    }
    auto image = mImageDescs.find(key(view->second.image));
    return image != mImageDescs.end() && mImages.isLive(view->second.image) &&
           image->second.format == format && hasUsage(image->second.usage, usage);
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
        auto view = mImageViewDescs.find(key(rendering.colors[i].view));
        if (view == mImageViewDescs.end()) return false;
        auto image = mImageDescs.find(key(view->second.image));
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
        auto view = mImageViewDescs.find(key(rendering.depthStencil->view));
        if (view == mImageViewDescs.end()) return false;
        auto image = mImageDescs.find(key(view->second.image));
        if (image == mImageDescs.end() || image->second.samples != pipeline->second.samples)
        {
            return false;
        }
    }

    return true;
}

DeviceCreationResult createValidationDevice(const DeviceCreateInfo& info)
{
    if (info.backend != Backend::Validation)
    {
        return {
            nullptr,
            Status::failure(
                StatusCode::InvalidArgument,
                "validation factory received a different backend")};
    }

    return {
        std::make_unique<ValidationDevice>(info),
        Status::success()};
}

} // namespace LL::GHI
