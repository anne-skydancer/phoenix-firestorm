/**
 * @file llghitrace.cpp
 * @brief Canonical semantic command stream for GHI validation.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "llghitrace.h"

#include <openssl/evp.h>

#include <array>
#include <bit>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace LL::GHI
{

void SemanticTrace::reset()
{
    mBytes.clear();
}

void SemanticTrace::appendUnsigned(std::uint64_t value, std::size_t bytes)
{
    // Canonical traces are always little endian, independent of the host.
    for (std::size_t i = 0; i < bytes; ++i)
    {
        mBytes.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

void SemanticTrace::appendOpcode(TraceOpcode opcode)
{
    appendEnum(opcode);
}

void SemanticTrace::appendU8(std::uint8_t value)
{
    appendUnsigned(value, sizeof(value));
}

void SemanticTrace::appendU16(std::uint16_t value)
{
    appendUnsigned(value, sizeof(value));
}

void SemanticTrace::appendU32(std::uint32_t value)
{
    appendUnsigned(value, sizeof(value));
}

void SemanticTrace::appendI32(std::int32_t value)
{
    appendU32(std::bit_cast<std::uint32_t>(value));
}

void SemanticTrace::appendU64(std::uint64_t value)
{
    appendUnsigned(value, sizeof(value));
}

void SemanticTrace::appendFloat(float value)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    appendU32(std::bit_cast<std::uint32_t>(value));
}

void SemanticTrace::appendClearValue(const ClearValue& value)
{
    for (float component : value.color)
    {
        appendFloat(component);
    }
    appendFloat(value.depth);
    appendU32(value.stencil);
}

void SemanticTrace::appendAttachment(const AttachmentDesc& attachment)
{
    appendHandle(attachment.image);
    appendEnum(attachment.format);
    appendEnum(attachment.load);
    appendEnum(attachment.store);
    appendClearValue(attachment.clear);
}

void SemanticTrace::beginFrame()
{
    appendOpcode(TraceOpcode::BeginFrame);
}

void SemanticTrace::endFrame()
{
    appendOpcode(TraceOpcode::EndFrame);
}

void SemanticTrace::beginRendering(const RenderingInfo& info)
{
    appendOpcode(TraceOpcode::BeginRendering);
    appendU64(info.semanticId);
    appendU32(info.width);
    appendU32(info.height);
    appendU32(static_cast<std::uint32_t>(info.colors.size()));
    for (const AttachmentDesc& attachment : info.colors)
    {
        appendAttachment(attachment);
    }
    appendU8(info.depthStencil.has_value() ? 1 : 0);
    if (info.depthStencil)
    {
        appendAttachment(*info.depthStencil);
    }
}

void SemanticTrace::endRendering()
{
    appendOpcode(TraceOpcode::EndRendering);
}

void SemanticTrace::bindPipeline(PipelineHandle pipeline)
{
    appendOpcode(TraceOpcode::BindPipeline);
    appendHandle(pipeline);
}

void SemanticTrace::bindVertexBuffer(
    std::uint32_t slot,
    BufferHandle buffer,
    std::uint64_t offset)
{
    appendOpcode(TraceOpcode::BindVertexBuffer);
    appendU32(slot);
    appendHandle(buffer);
    appendU64(offset);
}

void SemanticTrace::bindIndexBuffer(
    BufferHandle buffer,
    std::uint64_t offset,
    IndexType type)
{
    appendOpcode(TraceOpcode::BindIndexBuffer);
    appendHandle(buffer);
    appendU64(offset);
    appendEnum(type);
}

void SemanticTrace::draw(const DrawArguments& arguments)
{
    appendOpcode(TraceOpcode::Draw);
    appendU32(arguments.vertexCount);
    appendU32(arguments.instanceCount);
    appendU32(arguments.firstVertex);
    appendU32(arguments.firstInstance);
}

void SemanticTrace::drawIndexed(const DrawIndexedArguments& arguments)
{
    appendOpcode(TraceOpcode::DrawIndexed);
    appendU32(arguments.indexCount);
    appendU32(arguments.instanceCount);
    appendU32(arguments.firstIndex);
    appendI32(arguments.vertexOffset);
    appendU32(arguments.firstInstance);
}

std::string SemanticTrace::sha256() const
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    const void* data = mBytes.empty() ? nullptr : mBytes.data();

    if (EVP_Digest(
            data,
            mBytes.size(),
            digest.data(),
            &digest_size,
            EVP_sha256(),
            nullptr) != 1)
    {
        throw std::runtime_error("Unable to calculate the GHI semantic trace SHA-256");
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_size; ++i)
    {
        output << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return output.str();
}

} // namespace LL::GHI
