/**
 * @file llghicommand.h
 * @brief Backend-neutral GHI command recording contract.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHICOMMAND_H
#define LL_LLGHICOMMAND_H

#include "llghidescriptors.h"

#include <cstdint>
#include <span>

namespace LL::GHI
{

class CommandContext
{
public:
    virtual ~CommandContext() = default;

    virtual Status beginFrame() = 0;
    virtual Status endFrame() = 0;

    virtual Status copyBuffer(
        BufferHandle source,
        BufferHandle destination,
        std::span<const BufferCopyRegion> regions) = 0;
    virtual Status copyBufferToImage(
        BufferHandle source,
        ImageHandle destination,
        std::span<const BufferImageCopyRegion> regions) = 0;
    virtual Status copyImageToBuffer(
        ImageHandle source,
        BufferHandle destination,
        std::span<const BufferImageCopyRegion> regions) = 0;
    virtual Status generateMipmaps(
        ImageHandle image,
        const ImageSubresourceRange& subresources) = 0;
    virtual Status resetQueryPool(
        QueryPoolHandle pool,
        std::uint32_t firstQuery,
        std::uint32_t queryCount) = 0;
    virtual Status writeTimestamp(QueryPoolHandle pool, std::uint32_t query) = 0;
    virtual Status beginQuery(QueryPoolHandle pool, std::uint32_t query) = 0;
    virtual Status endQuery(QueryPoolHandle pool, std::uint32_t query) = 0;

    virtual Status beginRendering(const RenderingInfo& info) = 0;
    virtual Status endRendering() = 0;

    virtual Status bindPipeline(PipelineHandle pipeline) = 0;
    virtual Status bindBindingSet(
        std::uint8_t group,
        BindingSetHandle bindings,
        std::span<const std::uint32_t> dynamicOffsets = {}) = 0;
    virtual Status setViewport(const Viewport& viewport) = 0;
    virtual Status setScissor(const ScissorRect& scissor) = 0;
    virtual Status bindVertexBuffer(
        std::uint32_t slot,
        BufferHandle buffer,
        std::uint64_t offset) = 0;
    virtual Status bindIndexBuffer(
        BufferHandle buffer,
        std::uint64_t offset,
        IndexType type) = 0;

    virtual Status draw(const DrawArguments& arguments) = 0;
    virtual Status drawIndexed(const DrawIndexedArguments& arguments) = 0;
};

} // namespace LL::GHI

#endif // LL_LLGHICOMMAND_H
