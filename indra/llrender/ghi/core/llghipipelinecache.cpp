/**
 * @file llghipipelinecache.cpp
 * @brief Backend-neutral native pipeline-cache identity contract.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "llghipipelinecache.h"

#include "llghihash.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

namespace LL::GHI
{
namespace
{

class CanonicalBytes
{
public:
    template<typename T>
        requires std::is_integral_v<T>
    void integer(T value)
    {
        using Value = std::remove_cv_t<T>;
        using Unsigned = std::make_unsigned_t<Value>;
        Unsigned encoded = static_cast<Unsigned>(value);
        for (std::size_t i = 0; i < sizeof(Unsigned); ++i)
        {
            mBytes.push_back(static_cast<std::byte>((encoded >> (i * 8)) & 0xffu));
        }
    }

    template<typename T>
        requires std::is_enum_v<T>
    void integer(T value)
    {
        integer(static_cast<std::underlying_type_t<T>>(value));
    }

    void boolean(bool value) { integer<std::uint8_t>(value ? 1 : 0); }

    void bytes(std::span<const std::uint8_t> value)
    {
        for (std::uint8_t byte : value) mBytes.push_back(static_cast<std::byte>(byte));
    }

    void bytes(std::span<const std::byte> value)
    {
        mBytes.insert(mBytes.end(), value.begin(), value.end());
    }

    void string(std::string_view value)
    {
        integer<std::uint32_t>(static_cast<std::uint32_t>(value.size()));
        for (char byte : value)
            mBytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    }

    std::span<const std::byte> view() const { return mBytes; }

private:
    std::vector<std::byte> mBytes;
};

void appendStencil(CanonicalBytes& bytes, const StencilFaceState& state)
{
    bytes.integer(state.compare);
    bytes.integer(state.fail);
    bytes.integer(state.depthFail);
    bytes.integer(state.pass);
    bytes.integer(state.compareMask);
    bytes.integer(state.writeMask);
    bytes.integer(state.reference);
}

void appendBlend(CanonicalBytes& bytes, const BlendState& state)
{
    bytes.boolean(state.enabled);
    bytes.integer(state.sourceColor);
    bytes.integer(state.destinationColor);
    bytes.integer(state.colorOp);
    bytes.integer(state.sourceAlpha);
    bytes.integer(state.destinationAlpha);
    bytes.integer(state.alphaOp);
    bytes.integer(state.colorWriteMask);
}

} // namespace

std::string pipelineCacheIdentity(
    const ShaderPackageDesc& shaderPackage,
    const PipelineDesc& pipeline,
    ShaderPackageDesc::TargetProfile target,
    Backend backend,
    const PipelineCacheDomain& domain)
{
    CanonicalBytes bytes;
    bytes.string("LLGHI_PIPELINE_CACHE_V1");
    bytes.integer(backend);
    bytes.integer(target);
    bytes.bytes(shaderPackage.semanticHash);
    bytes.bytes(shaderPackage.toolchainHash);
    bytes.string(domain.deviceIdentity);
    bytes.string(domain.driverIdentity);

    bytes.integer(pipeline.topology);
    bytes.integer(pipeline.cullMode);
    bytes.boolean(pipeline.frontFaceCounterClockwise);
    bytes.boolean(pipeline.depthClamp);
    bytes.boolean(pipeline.depthTest);
    bytes.boolean(pipeline.depthWrite);
    bytes.integer(pipeline.depthCompare);
    bytes.boolean(pipeline.stencilTest);
    appendStencil(bytes, pipeline.frontStencil);
    appendStencil(bytes, pipeline.backStencil);
    bytes.integer(pipeline.samples);

    bytes.integer<std::uint32_t>(static_cast<std::uint32_t>(pipeline.colorFormats.size()));
    for (Format format : pipeline.colorFormats) bytes.integer(format);
    bytes.boolean(pipeline.depthStencilFormat.has_value());
    if (pipeline.depthStencilFormat) bytes.integer(*pipeline.depthStencilFormat);

    bytes.integer<std::uint32_t>(static_cast<std::uint32_t>(pipeline.blendStates.size()));
    for (const BlendState& blend : pipeline.blendStates) appendBlend(bytes, blend);

    bytes.integer<std::uint32_t>(static_cast<std::uint32_t>(pipeline.vertexBuffers.size()));
    for (const VertexBufferLayoutDesc& layout : pipeline.vertexBuffers)
    {
        bytes.integer(layout.slot);
        bytes.integer(layout.stride);
        bytes.integer(layout.inputRate);
    }
    bytes.integer<std::uint32_t>(static_cast<std::uint32_t>(pipeline.vertexAttributes.size()));
    for (const VertexAttributeDesc& attribute : pipeline.vertexAttributes)
    {
        bytes.integer(attribute.location);
        bytes.integer(attribute.bufferSlot);
        bytes.integer(attribute.format);
        bytes.integer(attribute.offset);
    }
    bytes.integer<std::uint32_t>(
        static_cast<std::uint32_t>(pipeline.specializationConstants.size()));
    for (const SpecializationConstantDesc& value : pipeline.specializationConstants)
    {
        bytes.integer(value.id);
        bytes.integer(value.size);
        bytes.bytes(std::span<const std::byte>(value.value.data(), value.size));
    }
    return sha256(bytes.view());
}

} // namespace LL::GHI
