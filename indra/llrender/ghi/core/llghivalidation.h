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
#include <unordered_map>

namespace LL::GHI
{

class ValidationDevice;

class ValidationCommandContext final : public CommandContext
{
public:
    explicit ValidationCommandContext(ValidationDevice& device);

    Status beginFrame() override;
    Status endFrame() override;
    Status beginRendering(const RenderingInfo& info) override;
    Status endRendering() override;
    Status bindPipeline(PipelineHandle pipeline) override;
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

    ValidationDevice& mDevice;
    SemanticTrace mTrace;
    RenderingInfo mRenderingInfo;
    PipelineHandle mPipeline;
    BufferHandle mIndexBuffer;
    bool mFrameActive = false;
    bool mRendering = false;
};

class ValidationDevice final : public Device
{
public:
    explicit ValidationDevice(const DeviceCreateInfo& info);

    Backend backend() const override { return Backend::Validation; }
    const DeviceCapabilities& capabilities() const override { return mCapabilities; }
    CommandContext& commandContext() override { return mCommands; }

    BufferHandle createBuffer(const BufferDesc& desc, Status& status) override;
    ImageHandle createImage(const ImageDesc& desc, Status& status) override;
    SamplerHandle createSampler(const SamplerDesc& desc, Status& status) override;
    ShaderPackageHandle createShaderPackage(
        const ShaderPackageDesc& desc,
        Status& status) override;
    PipelineHandle createPipeline(const PipelineDesc& desc, Status& status) override;

    Status destroy(BufferHandle handle) override;
    Status destroy(ImageHandle handle) override;
    Status destroy(SamplerHandle handle) override;
    Status destroy(ShaderPackageHandle handle) override;
    Status destroy(PipelineHandle handle) override;

    bool isLive(BufferHandle handle) const { return mBuffers.isLive(handle); }
    bool isLive(ImageHandle handle) const { return mImages.isLive(handle); }
    bool isLive(PipelineHandle handle) const { return mPipelines.isLive(handle); }
    bool bufferSupports(BufferHandle handle, ResourceUsage usage) const;
    bool imageMatches(ImageHandle handle, Format format, ResourceUsage usage) const;
    bool pipelineMatches(PipelineHandle handle, const RenderingInfo& rendering) const;

    const SemanticTrace& semanticTrace() const { return mCommands.trace(); }

private:
    template<typename Tag>
    static std::uint64_t key(Handle<Tag> handle)
    {
        return (static_cast<std::uint64_t>(handle.generation()) << 32) |
               handle.index();
    }

    Status canMutateResources() const;

    DeviceCapabilities mCapabilities;
    HandlePool<BufferTag> mBuffers;
    HandlePool<ImageTag> mImages;
    HandlePool<SamplerTag> mSamplers;
    HandlePool<ShaderPackageTag> mShaders;
    HandlePool<PipelineTag> mPipelines;
    std::unordered_map<std::uint64_t, BufferDesc> mBufferDescs;
    std::unordered_map<std::uint64_t, ImageDesc> mImageDescs;
    std::unordered_map<std::uint64_t, PipelineDesc> mPipelineDescs;
    ValidationCommandContext mCommands;
};

} // namespace LL::GHI

#endif // LL_LLGHIVALIDATION_H
