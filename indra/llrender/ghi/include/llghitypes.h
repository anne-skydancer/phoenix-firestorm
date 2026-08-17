/**
 * @file llghitypes.h
 * @brief Backend-neutral Graphics Hardware Interface value types.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHITYPES_H
#define LL_LLGHITYPES_H

#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace LL::GHI
{

enum class Backend : std::uint8_t
{
    OpenGL,
    Vulkan,
    Validation,
};

enum class StatusCode : std::uint8_t
{
    Ok,
    NotReady,
    InvalidArgument,
    InvalidState,
    InvalidHandle,
    Unsupported,
    DeviceLost,
    BackendError,
};

class Status
{
public:
    static Status success()
    {
        return Status{};
    }

    static Status failure(StatusCode code, std::string message)
    {
        return Status(code, std::move(message));
    }

    bool ok() const { return mCode == StatusCode::Ok; }
    explicit operator bool() const { return ok(); }
    StatusCode code() const { return mCode; }
    const std::string& message() const { return mMessage; }

private:
    Status() = default;
    Status(StatusCode code, std::string message) :
        mCode(code),
        mMessage(std::move(message))
    {
    }

    StatusCode mCode = StatusCode::Ok;
    std::string mMessage;
};

template<typename Tag>
class Handle
{
public:
    static constexpr std::uint32_t INVALID_INDEX =
        std::numeric_limits<std::uint32_t>::max();

    constexpr Handle() = default;

    static constexpr Handle fromParts(std::uint32_t index, std::uint32_t generation)
    {
        return Handle(index, generation);
    }

    constexpr bool valid() const
    {
        return mIndex != INVALID_INDEX && mGeneration != 0;
    }

    constexpr explicit operator bool() const { return valid(); }
    constexpr std::uint32_t index() const { return mIndex; }
    constexpr std::uint32_t generation() const { return mGeneration; }

    friend constexpr bool operator==(Handle, Handle) = default;

private:
    constexpr Handle(std::uint32_t index, std::uint32_t generation) :
        mIndex(index),
        mGeneration(generation)
    {
    }

    std::uint32_t mIndex = INVALID_INDEX;
    std::uint32_t mGeneration = 0;
};

struct BufferTag;
struct ImageTag;
struct ImageViewTag;
struct SamplerTag;
struct ShaderPackageTag;
struct BindingSetTag;
struct PipelineTag;
struct QueryPoolTag;

using BufferHandle = Handle<BufferTag>;
using ImageHandle = Handle<ImageTag>;
using ImageViewHandle = Handle<ImageViewTag>;
using SamplerHandle = Handle<SamplerTag>;
using ShaderPackageHandle = Handle<ShaderPackageTag>;
using BindingSetHandle = Handle<BindingSetTag>;
using PipelineHandle = Handle<PipelineTag>;
using QueryPoolHandle = Handle<QueryPoolTag>;

enum class ResourceUsage : std::uint32_t
{
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Sampled = 1u << 4,
    ColorAttachment = 1u << 5,
    DepthStencilAttachment = 1u << 6,
    TransferSource = 1u << 7,
    TransferDestination = 1u << 8,
    Present = 1u << 9,
};

constexpr ResourceUsage operator|(ResourceUsage lhs, ResourceUsage rhs)
{
    return static_cast<ResourceUsage>(
        static_cast<std::underlying_type_t<ResourceUsage>>(lhs) |
        static_cast<std::underlying_type_t<ResourceUsage>>(rhs));
}

constexpr ResourceUsage operator&(ResourceUsage lhs, ResourceUsage rhs)
{
    return static_cast<ResourceUsage>(
        static_cast<std::underlying_type_t<ResourceUsage>>(lhs) &
        static_cast<std::underlying_type_t<ResourceUsage>>(rhs));
}

constexpr bool hasUsage(ResourceUsage value, ResourceUsage required)
{
    return (value & required) == required;
}

enum class MemoryClass : std::uint8_t
{
    DeviceLocal,
    Upload,
    Readback,
};

enum class ResourceBarrier : std::uint8_t
{
    // Shader storage buffer/image writes become visible to later shader
    // storage or sampled reads in the same frame.
    StorageWriteToRead,
    // A completed depth attachment becomes a sampled input to a later peel.
    DepthAttachmentWriteToSampledRead,
};

enum class Format : std::uint16_t
{
    Undefined,
    R8UNorm,
    RG8UNorm,
    RGBA8UNorm,
    RGBA8SRGB,
    BGRA8UNorm,
    BGRA8SRGB,
    R16Float,
    RG16Float,
    RGBA16Float,
    R32Float,
    RG32Float,
    RGBA32Float,
    R32UInt,
    Depth16UNorm,
    Depth24Stencil8,
    Depth32Float,
    Depth32FloatStencil8,
    // New serialized format values append here. Do not renumber existing
    // entries: Format participates in the semantic trace contract.
    RGB10A2UNorm,
    RGBA16UNorm,
    RGB16Float,
};

struct RendererCapabilities
{
    std::uint32_t maxFramesInFlight = 1;
    std::uint32_t maxColorAttachments = 1;
    std::uint32_t maxSampledImagesPerStage = 1;
    std::uint32_t maxStorageBuffersPerStage = 0;
    std::uint32_t maxTexture2DSize = 1;
    std::uint32_t maxUniformBufferSize = 0;
    std::uint32_t maxVaryingVectors = 0;
    std::uint32_t maxSamples = 1;
    std::uint64_t maxBufferSize = 0;
    std::uint64_t uniformBufferOffsetAlignment = 1;
    std::uint64_t storageBufferOffsetAlignment = 1;
    // Selected from queried capabilities. Exact-format resource creation
    // remains strict and never substitutes this format implicitly.
    Format preferredDepthStencilFormat = Format::Undefined;
    bool timestampQueries = false;
    double timestampPeriodNanoseconds = 0.0;
    bool occlusionQueries = false;
    bool descriptorIndexing = false;
    bool storageImageAtomics = false;
    bool depthClamp = false;
    bool independentBlend = false;

    // Semantic viewer feature levels. Backends derive these from their own
    // native requirements so policy consumers never compare unrelated API
    // version numbers (for example Vulkan 1.4 and OpenGL 4.6).
    bool baselineGraphicsPipeline = false;
    bool advancedGraphicsPipeline = false;

    friend bool operator==(const RendererCapabilities&, const RendererCapabilities&) = default;
};

static_assert(sizeof(BufferHandle) == 8, "GHI handles must remain compact values");
static_assert(std::is_trivially_copyable_v<BufferHandle>);

} // namespace LL::GHI

#endif // LL_LLGHITYPES_H
