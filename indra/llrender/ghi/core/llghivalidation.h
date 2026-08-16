/**
 * @file llghivalidation.h
 * @brief Non-rendering GHI contract validator and trace device.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIVALIDATION_H
#define LL_LLGHIVALIDATION_H

#include "ghi/core/llghihandlepool.h"
#include "ghi/core/llghitrace.h"
#include "ghi/include/llghidevice.h"

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace LL::GHI
{

class ValidationDevice;

class ValidationCommandContext final : public CommandContext
{
public:
    explicit ValidationCommandContext(ValidationDevice& device);

    Status beginFrame() override;
    Status endFrame() override;
    Status copyBuffer(
        BufferHandle source,
        BufferHandle destination,
        std::span<const BufferCopyRegion> regions) override;
    Status copyBufferToImage(
        BufferHandle source,
        ImageHandle destination,
        std::span<const BufferImageCopyRegion> regions) override;
    Status copyImageToBuffer(
        ImageHandle source,
        BufferHandle destination,
        std::span<const BufferImageCopyRegion> regions) override;
    Status generateMipmaps(
        ImageHandle image,
        const ImageSubresourceRange& subresources) override;
    Status resetQueryPool(
        QueryPoolHandle pool,
        std::uint32_t firstQuery,
        std::uint32_t queryCount) override;
    Status writeTimestamp(QueryPoolHandle pool, std::uint32_t query) override;
    Status beginQuery(QueryPoolHandle pool, std::uint32_t query) override;
    Status endQuery(QueryPoolHandle pool, std::uint32_t query) override;
    Status beginRendering(const RenderingInfo& info) override;
    Status endRendering() override;
    Status bindPipeline(PipelineHandle pipeline) override;
    Status bindBindingSet(
        std::uint8_t group,
        BindingSetHandle bindings,
        std::span<const std::uint32_t> dynamicOffsets) override;
    Status setViewport(const Viewport& viewport) override;
    Status setScissor(const ScissorRect& scissor) override;
    Status bindVertexBuffer(
        std::uint32_t slot,
        BufferHandle buffer,
        std::uint64_t offset) override;
    Status bindIndexBuffer(
        BufferHandle buffer,
        std::uint64_t offset,
        IndexType type) override;
    Status draw(const DrawArguments& arguments) override;
    Status drawIndexed(const DrawIndexedArguments& arguments) override;

    bool frameActive() const { return mFrameActive; }
    const SemanticTrace& trace() const { return mTrace; }

private:
    Status requireRendering(const char* operation) const;
    Status requireTransfer(const char* operation) const;

    ValidationDevice& mDevice;
    SemanticTrace mTrace;
    RenderingInfo mRenderingInfo;
    PipelineHandle mPipeline;
    BufferHandle mIndexBuffer;
    std::unordered_map<std::uint8_t, BindingSetHandle> mBindingSets;
    std::unordered_set<std::uint32_t> mVertexBufferSlots;
    std::uint64_t mIndexOffset = 0;
    IndexType mIndexType = IndexType::UInt16;
    bool mFrameActive = false;
    bool mRendering = false;
    bool mViewportSet = false;
    bool mScissorSet = false;
    QueryPoolHandle mActiveQueryPool;
    std::uint32_t mActiveQuery = 0;
    std::uint64_t mOcclusionSamples = 0;
};

class ValidationDevice final : public Device
{
public:
    explicit ValidationDevice(const DeviceCreateInfo& info);

    Backend backend() const override { return Backend::Validation; }
    const RendererCapabilities& capabilities() const override { return mCapabilities; }
    PipelineCacheDomain pipelineCacheDomain() const override
    {
        return {"validation-device", "validation-driver-v1"};
    }
    CommandContext& commandContext() override { return mCommands; }

    BufferHandle createBuffer(const BufferDesc& desc, Status& status) override;
    ImageHandle createImage(const ImageDesc& desc, Status& status) override;
    ImageViewHandle createImageView(const ImageViewDesc& desc, Status& status) override;
    SamplerHandle createSampler(const SamplerDesc& desc, Status& status) override;
    QueryPoolHandle createQueryPool(const QueryPoolDesc& desc, Status& status) override;
    ShaderPackageHandle createShaderPackage(
        const ShaderPackageDesc& desc,
        Status& status) override;
    BindingSetHandle createBindingSet(
        const BindingSetDesc& desc,
        Status& status) override;
    PipelineHandle createPipeline(const PipelineDesc& desc, Status& status) override;

    Status destroy(BufferHandle handle) override;
    Status destroy(ImageHandle handle) override;
    Status destroy(ImageViewHandle handle) override;
    Status destroy(SamplerHandle handle) override;
    Status destroy(QueryPoolHandle handle) override;
    Status destroy(ShaderPackageHandle handle) override;
    Status destroy(BindingSetHandle handle) override;
    Status destroy(PipelineHandle handle) override;
    Status writeBuffer(
        BufferHandle handle,
        std::uint64_t offset,
        std::span<const std::byte> data) override;
    Status readBuffer(
        BufferHandle handle,
        std::uint64_t offset,
        std::span<std::byte> data) override;
    Status getQueryResults(
        QueryPoolHandle pool,
        std::uint32_t firstQuery,
        std::span<std::uint64_t> results,
        QueryReadMode mode = QueryReadMode::AvailableOnly) override;
    Status waitIdle() override;

    bool isLive(BufferHandle handle) const { return mBuffers.isLive(handle); }
    bool isLive(ImageHandle handle) const { return mImages.isLive(handle); }
    bool isLive(ImageViewHandle handle) const { return mImageViews.isLive(handle); }
    bool isLive(SamplerHandle handle) const { return mSamplers.isLive(handle); }
    bool isLive(BindingSetHandle handle) const { return mBindingSets.isLive(handle); }
    bool isLive(QueryPoolHandle handle) const { return mQueryPools.isLive(handle); }
    bool isLive(PipelineHandle handle) const { return mPipelines.isLive(handle); }
    bool bufferSupports(BufferHandle handle, ResourceUsage usage) const;
    bool imageViewMatches(ImageViewHandle handle, Format format, ResourceUsage usage) const;
    bool imageViewCovers(ImageViewHandle handle, std::uint32_t width, std::uint32_t height) const;
    bool pipelineMatches(PipelineHandle handle, const RenderingInfo& rendering) const;
    Status validateBindingSetForPipeline(
        PipelineHandle pipeline,
        std::uint8_t group,
        BindingSetHandle bindings,
        std::span<const std::uint32_t> dynamicOffsets) const;
    Status validateDrawState(
        PipelineHandle pipeline,
        const std::unordered_map<std::uint8_t, BindingSetHandle>& bindingSets,
        const std::unordered_set<std::uint32_t>& vertexBufferSlots,
        bool viewportSet,
        bool scissorSet) const;
    std::uint64_t bufferSize(BufferHandle handle) const;

    Status executeCopyBuffer(
        BufferHandle source,
        BufferHandle destination,
        std::span<const BufferCopyRegion> regions);
    Status executeCopyBufferToImage(
        BufferHandle source,
        ImageHandle destination,
        std::span<const BufferImageCopyRegion> regions);
    Status executeCopyImageToBuffer(
        ImageHandle source,
        BufferHandle destination,
        std::span<const BufferImageCopyRegion> regions);
    Status executeGenerateMipmaps(
        ImageHandle image,
        const ImageSubresourceRange& subresources);
    Status resetQueries(QueryPoolHandle pool, std::uint32_t first, std::uint32_t count);
    Status recordTimestamp(QueryPoolHandle pool, std::uint32_t query);
    Status beginOcclusionQuery(QueryPoolHandle pool, std::uint32_t query);
    Status endOcclusionQuery(
        QueryPoolHandle pool,
        std::uint32_t query,
        std::uint64_t samples);
    void completeFrame();
    std::size_t pendingRetirementCount() const { return mRetirements.size(); }

    const SemanticTrace& semanticTrace() const { return mCommands.trace(); }

private:
    template<typename Tag>
    static std::uint64_t key(Handle<Tag> handle)
    {
        return (static_cast<std::uint64_t>(handle.generation()) << 32) |
               handle.index();
    }

    Status canMutateResources() const;
    void retire();

    struct QueryRecord
    {
        QueryPoolDesc desc;
        std::vector<std::uint64_t> values;
        std::vector<bool> available;
        std::vector<bool> pending;
        std::vector<bool> active;
    };

    struct Retirement
    {
        std::uint64_t releaseAfterFrame = 0;
    };

    RendererCapabilities mCapabilities;
    HandlePool<BufferTag> mBuffers;
    HandlePool<ImageTag> mImages;
    HandlePool<ImageViewTag> mImageViews;
    HandlePool<SamplerTag> mSamplers;
    HandlePool<QueryPoolTag> mQueryPools;
    HandlePool<ShaderPackageTag> mShaders;
    HandlePool<BindingSetTag> mBindingSets;
    HandlePool<PipelineTag> mPipelines;
    std::unordered_map<std::uint64_t, BufferDesc> mBufferDescs;
    std::unordered_map<std::uint64_t, ImageDesc> mImageDescs;
    std::unordered_map<std::uint64_t, ImageViewDesc> mImageViewDescs;
    std::unordered_map<std::uint64_t, QueryRecord> mQueryRecords;
    std::unordered_map<std::uint64_t, ShaderPackageDesc> mShaderDescs;
    std::unordered_map<std::uint64_t, BindingSetDesc> mBindingSetDescs;
    std::unordered_map<std::uint64_t, PipelineDesc> mPipelineDescs;
    std::unordered_map<std::uint64_t, std::vector<std::byte>> mBufferData;
    std::unordered_map<std::uint64_t, std::uint64_t> mBufferReadyFrame;
    std::unordered_map<std::uint64_t,
        std::unordered_map<std::uint32_t, std::vector<std::byte>>> mImageData;
    std::vector<Retirement> mRetirements;
    std::uint64_t mSubmittedFrames = 0;
    std::uint64_t mTimestampCounter = 0;
    std::uint32_t mFramesInFlight = 1;
    ValidationCommandContext mCommands;
};

} // namespace LL::GHI

#endif // LL_LLGHIVALIDATION_H
