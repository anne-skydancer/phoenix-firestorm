/**
 * @file llghitrace.h
 * @brief Canonical semantic command stream for GHI validation.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHITRACE_H
#define LL_LLGHITRACE_H

#include "ghi/include/llghidescriptors.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace LL::GHI
{

enum class TraceOpcode : std::uint8_t
{
    BeginFrame = 1,
    EndFrame,
    BeginRendering,
    EndRendering,
    BindPipeline,
    BindVertexBuffer,
    BindIndexBuffer,
    Draw,
    DrawIndexed,
    CopyBuffer,
    CopyBufferToImage,
    CopyImageToBuffer,
    GenerateMipmaps,
    ResetQueryPool,
    WriteTimestamp,
    BindBindingSet,
    SetViewport,
    SetScissor,
};

class SemanticTrace
{
public:
    void reset();

    void beginFrame();
    void endFrame();
    void copyBuffer(
        BufferHandle source,
        BufferHandle destination,
        std::span<const BufferCopyRegion> regions);
    void copyBufferToImage(
        BufferHandle source,
        ImageHandle destination,
        std::span<const BufferImageCopyRegion> regions);
    void copyImageToBuffer(
        ImageHandle source,
        BufferHandle destination,
        std::span<const BufferImageCopyRegion> regions);
    void generateMipmaps(ImageHandle image, const ImageSubresourceRange& subresources);
    void resetQueryPool(QueryPoolHandle pool, std::uint32_t first, std::uint32_t count);
    void writeTimestamp(QueryPoolHandle pool, std::uint32_t query);
    void beginRendering(const RenderingInfo& info);
    void endRendering();
    void bindPipeline(PipelineHandle pipeline);
    void bindBindingSet(
        std::uint8_t group,
        BindingSetHandle bindings,
        std::span<const std::uint32_t> dynamicOffsets);
    void setViewport(const Viewport& viewport);
    void setScissor(const ScissorRect& scissor);
    void bindVertexBuffer(
        std::uint32_t slot,
        BufferHandle buffer,
        std::uint64_t offset);
    void bindIndexBuffer(BufferHandle buffer, std::uint64_t offset, IndexType type);
    void draw(const DrawArguments& arguments);
    void drawIndexed(const DrawIndexedArguments& arguments);

    const std::vector<std::uint8_t>& bytes() const { return mBytes; }
    std::string sha256() const;

private:
    template<typename Enum>
    void appendEnum(Enum value)
    {
        appendUnsigned(static_cast<std::uint64_t>(value), sizeof(Enum));
    }

    template<typename Tag>
    void appendHandle(Handle<Tag> handle)
    {
        appendU32(handle.index());
        appendU32(handle.generation());
    }

    void appendOpcode(TraceOpcode opcode);
    void appendUnsigned(std::uint64_t value, std::size_t bytes);
    void appendU8(std::uint8_t value);
    void appendU16(std::uint16_t value);
    void appendU32(std::uint32_t value);
    void appendI32(std::int32_t value);
    void appendU64(std::uint64_t value);
    void appendFloat(float value);
    void appendClearValue(const ClearValue& value);
    void appendAttachment(const AttachmentDesc& attachment);
    void appendImageSubresourceRange(const ImageSubresourceRange& subresources);
    void appendBufferImageCopyRegion(const BufferImageCopyRegion& region);

    std::vector<std::uint8_t> mBytes;
};

} // namespace LL::GHI

#endif // LL_LLGHITRACE_H
