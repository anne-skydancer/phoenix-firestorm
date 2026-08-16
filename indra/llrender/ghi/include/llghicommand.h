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

namespace LL::GHI
{

class CommandContext
{
public:
    virtual ~CommandContext() = default;

    virtual Status beginFrame() = 0;
    virtual Status endFrame() = 0;

    virtual Status beginRendering(const RenderingInfo& info) = 0;
    virtual Status endRendering() = 0;

    virtual Status bindPipeline(PipelineHandle pipeline) = 0;
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
