/**
 * @file llghidescriptors.h
 * @brief Backend-neutral GHI resource, pass, and pipeline descriptors.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIDESCRIPTORS_H
#define LL_LLGHIDESCRIPTORS_H

#include "llghitypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace LL::GHI
{

struct Extent3D
{
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depth = 1;

    friend bool operator==(const Extent3D&, const Extent3D&) = default;
};

struct BufferDesc
{
    std::uint64_t size = 0;
    ResourceUsage usage = ResourceUsage::None;
    MemoryClass memory = MemoryClass::DeviceLocal;

    friend bool operator==(const BufferDesc&, const BufferDesc&) = default;
};

struct ImageDesc
{
    Extent3D extent;
    Format format = Format::Undefined;
    ResourceUsage usage = ResourceUsage::None;
    std::uint16_t mipLevels = 1;
    std::uint16_t arrayLayers = 1;
    std::uint8_t samples = 1;

    friend bool operator==(const ImageDesc&, const ImageDesc&) = default;
};

enum class ImageAspect : std::uint8_t
{
    Color,
    Depth,
    Stencil,
    DepthStencil,
};

struct ImageSubresourceRange
{
    ImageAspect aspect = ImageAspect::Color;
    std::uint16_t baseMipLevel = 0;
    std::uint16_t mipLevelCount = 1;
    std::uint16_t baseArrayLayer = 0;
    std::uint16_t arrayLayerCount = 1;

    friend bool operator==(const ImageSubresourceRange&, const ImageSubresourceRange&) = default;
};

struct ImageViewDesc
{
    ImageHandle image;
    Format format = Format::Undefined;
    ImageSubresourceRange subresources;

    friend bool operator==(const ImageViewDesc&, const ImageViewDesc&) = default;
};

enum class Filter : std::uint8_t
{
    Nearest,
    Linear,
};

enum class AddressMode : std::uint8_t
{
    Repeat,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder,
};

struct SamplerDesc
{
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    Filter mipFilter = Filter::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    float maxAnisotropy = 1.f;

    friend bool operator==(const SamplerDesc&, const SamplerDesc&) = default;
};

struct BufferCopyRegion
{
    std::uint64_t sourceOffset = 0;
    std::uint64_t destinationOffset = 0;
    std::uint64_t size = 0;

    friend bool operator==(const BufferCopyRegion&, const BufferCopyRegion&) = default;
};

struct Offset3D
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    friend bool operator==(const Offset3D&, const Offset3D&) = default;
};

struct ImageSubresourceLayers
{
    ImageAspect aspect = ImageAspect::Color;
    std::uint16_t mipLevel = 0;
    std::uint16_t baseArrayLayer = 0;
    std::uint16_t arrayLayerCount = 1;

    friend bool operator==(const ImageSubresourceLayers&, const ImageSubresourceLayers&) = default;
};

struct BufferImageCopyRegion
{
    std::uint64_t bufferOffset = 0;
    // Zero selects tightly packed rows/slices.
    std::uint32_t bufferRowLength = 0;
    std::uint32_t bufferImageHeight = 0;
    ImageSubresourceLayers imageSubresource;
    Offset3D imageOffset;
    Extent3D imageExtent;

    friend bool operator==(const BufferImageCopyRegion&, const BufferImageCopyRegion&) = default;
};

enum class QueryType : std::uint8_t
{
    Timestamp,
};

struct QueryPoolDesc
{
    QueryType type = QueryType::Timestamp;
    std::uint32_t count = 0;

    friend bool operator==(const QueryPoolDesc&, const QueryPoolDesc&) = default;
};

enum class QueryReadMode : std::uint8_t
{
    AvailableOnly,
    Wait,
};

enum class VertexFormat : std::uint8_t
{
    Float32,
    Float32x2,
    Float32x3,
    Float32x4,
    UNorm8x4,
    SNorm8x4,
    UInt16x2,
    UInt16x4,
    UInt32,
};

struct ShaderPackageDesc
{
    static constexpr std::uint32_t CURRENT_SCHEMA_VERSION = 1;

    std::uint32_t schemaVersion = CURRENT_SCHEMA_VERSION;
    // Stable content/permutation identity produced by the shader packager.
    std::array<std::uint8_t, 32> semanticHash{};
    // Includes the pinned frontend and optimization recipe so cached native
    // programs/pipelines cannot survive a toolchain change accidentally.
    std::array<std::uint8_t, 32> toolchainHash{};

    enum class Stage : std::uint8_t
    {
        Vertex,
        Fragment,
        Compute,
    };

    struct StageArtifact
    {
        Stage stage = Stage::Vertex;
        std::string entryPoint = "main";
        // The OpenGL peer consumes packaged OpenGL-dialect GLSL. The Vulkan
        // peer consumes offline-compiled SPIR-V and never compiles GLSL in the
        // normal runtime path.
        std::string openGLSource;
        std::vector<std::uint32_t> vulkanSpirv;
        std::array<std::uint8_t, 32> openGLArtifactHash{};
        std::array<std::uint8_t, 32> vulkanArtifactHash{};

        friend bool operator==(const StageArtifact&, const StageArtifact&) = default;
    };

    enum class BindingType : std::uint8_t
    {
        UniformBuffer,
        StorageBuffer,
        Sampler,
        SampledImage,
        CombinedImageSampler,
        StorageImage,
    };

    enum class StageVisibility : std::uint8_t
    {
        None = 0,
        Vertex = 1u << 0,
        Fragment = 1u << 1,
        Compute = 1u << 2,
    };

    struct Binding
    {
        std::uint8_t group = 0;
        std::uint16_t binding = 0;
        BindingType type = BindingType::UniformBuffer;
        StageVisibility visibility = StageVisibility::None;
        std::uint16_t arrayCount = 1;
        bool dynamicOffset = false;

        friend bool operator==(const Binding&, const Binding&) = default;
    };

    struct VertexInput
    {
        std::uint16_t location = 0;
        VertexFormat format = VertexFormat::Float32;

        friend bool operator==(const VertexInput&, const VertexInput&) = default;
    };

    std::vector<StageArtifact> stages;
    std::vector<Binding> bindings;
    std::vector<VertexInput> vertexInputs;
    std::uint16_t pushConstantBytes = 0;

    friend bool operator==(const ShaderPackageDesc&, const ShaderPackageDesc&) = default;
};

constexpr ShaderPackageDesc::StageVisibility operator|(
    ShaderPackageDesc::StageVisibility lhs,
    ShaderPackageDesc::StageVisibility rhs)
{
    return static_cast<ShaderPackageDesc::StageVisibility>(
        static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

enum class VertexInputRate : std::uint8_t
{
    PerVertex,
    PerInstance,
};

struct VertexBufferLayoutDesc
{
    std::uint8_t slot = 0;
    std::uint16_t stride = 0;
    VertexInputRate inputRate = VertexInputRate::PerVertex;

    friend bool operator==(const VertexBufferLayoutDesc&, const VertexBufferLayoutDesc&) = default;
};

struct VertexAttributeDesc
{
    std::uint16_t location = 0;
    std::uint8_t bufferSlot = 0;
    VertexFormat format = VertexFormat::Float32;
    std::uint16_t offset = 0;

    friend bool operator==(const VertexAttributeDesc&, const VertexAttributeDesc&) = default;
};

struct SpecializationConstantDesc
{
    std::uint32_t id = 0;
    std::array<std::byte, 8> value{};
    std::uint8_t size = 0;

    friend bool operator==(const SpecializationConstantDesc&, const SpecializationConstantDesc&) = default;
};

struct BindingResourceDesc
{
    std::uint16_t binding = 0;
    std::uint16_t arrayElement = 0;
    ShaderPackageDesc::BindingType type = ShaderPackageDesc::BindingType::UniformBuffer;
    BufferHandle buffer;
    std::uint64_t bufferOffset = 0;
    // Zero means the remainder of the buffer.
    std::uint64_t bufferRange = 0;
    ImageViewHandle imageView;
    SamplerHandle sampler;

    friend bool operator==(const BindingResourceDesc&, const BindingResourceDesc&) = default;
};

struct BindingSetDesc
{
    ShaderPackageHandle shader;
    std::uint8_t group = 0;
    std::vector<BindingResourceDesc> resources;

    friend bool operator==(const BindingSetDesc&, const BindingSetDesc&) = default;
};

enum class PrimitiveTopology : std::uint8_t
{
    Points,
    Lines,
    LineStrip,
    Triangles,
    TriangleStrip,
};

enum class CullMode : std::uint8_t
{
    None,
    Front,
    Back,
};

enum class CompareOp : std::uint8_t
{
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

enum class StencilOp : std::uint8_t
{
    Keep,
    Zero,
    Replace,
    IncrementClamp,
    DecrementClamp,
    Invert,
    IncrementWrap,
    DecrementWrap,
};

struct StencilFaceState
{
    StencilOp fail = StencilOp::Keep;
    StencilOp depthFail = StencilOp::Keep;
    StencilOp pass = StencilOp::Keep;
    CompareOp compare = CompareOp::Always;
    std::uint32_t compareMask = ~std::uint32_t{0};
    std::uint32_t writeMask = ~std::uint32_t{0};
    std::uint32_t reference = 0;

    friend bool operator==(const StencilFaceState&, const StencilFaceState&) = default;
};

enum class BlendFactor : std::uint8_t
{
    Zero,
    One,
    SourceColor,
    OneMinusSourceColor,
    DestinationColor,
    OneMinusDestinationColor,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationAlpha,
    OneMinusDestinationAlpha,
};

enum class BlendOp : std::uint8_t
{
    Add,
    Subtract,
    ReverseSubtract,
    Minimum,
    Maximum,
};

struct BlendState
{
    bool enabled = false;
    BlendFactor sourceColor = BlendFactor::One;
    BlendFactor destinationColor = BlendFactor::Zero;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor sourceAlpha = BlendFactor::One;
    BlendFactor destinationAlpha = BlendFactor::Zero;
    BlendOp alphaOp = BlendOp::Add;
    std::uint8_t colorWriteMask = 0x0f;

    friend bool operator==(const BlendState&, const BlendState&) = default;
};

struct PipelineDesc
{
    ShaderPackageHandle shader;
    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    CullMode cullMode = CullMode::Back;
    bool frontFaceCounterClockwise = true;
    bool depthTest = true;
    bool depthWrite = true;
    CompareOp depthCompare = CompareOp::GreaterEqual;
    bool depthClamp = false;
    bool stencilTest = false;
    StencilFaceState frontStencil;
    StencilFaceState backStencil;
    std::vector<Format> colorFormats;
    std::optional<Format> depthStencilFormat;
    std::vector<BlendState> blendStates;
    std::vector<VertexBufferLayoutDesc> vertexBuffers;
    std::vector<VertexAttributeDesc> vertexAttributes;
    std::vector<SpecializationConstantDesc> specializationConstants;
    std::uint8_t samples = 1;

    friend bool operator==(const PipelineDesc&, const PipelineDesc&) = default;
};

enum class LoadOp : std::uint8_t
{
    Load,
    Clear,
    Discard,
};

enum class StoreOp : std::uint8_t
{
    Store,
    Discard,
};

struct ClearValue
{
    std::array<float, 4> color{0.f, 0.f, 0.f, 0.f};
    float depth = 0.f;
    std::uint32_t stencil = 0;

    friend bool operator==(const ClearValue&, const ClearValue&) = default;
};

struct AttachmentDesc
{
    // Rendering binds a particular view, not an ambient whole-image target.
    ImageViewHandle view;
    Format format = Format::Undefined;
    LoadOp load = LoadOp::Load;
    StoreOp store = StoreOp::Store;
    ClearValue clear;

    friend bool operator==(const AttachmentDesc&, const AttachmentDesc&) = default;
};

struct Viewport
{
    float x = 0.f;
    float y = 0.f;
    float width = 0.f;
    float height = 0.f;
    float minDepth = 0.f;
    float maxDepth = 1.f;

    friend bool operator==(const Viewport&, const Viewport&) = default;
};

struct ScissorRect
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const ScissorRect&, const ScissorRect&) = default;
};

struct RenderingInfo
{
    // Stable renderer-defined identity; debug labels are deliberately separate.
    std::uint64_t semanticId = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<AttachmentDesc> colors;
    std::optional<AttachmentDesc> depthStencil;

    friend bool operator==(const RenderingInfo&, const RenderingInfo&) = default;
};

enum class IndexType : std::uint8_t
{
    UInt16,
    UInt32,
};

struct DrawArguments
{
    std::uint32_t vertexCount = 0;
    std::uint32_t instanceCount = 1;
    std::uint32_t firstVertex = 0;
    std::uint32_t firstInstance = 0;

    friend bool operator==(const DrawArguments&, const DrawArguments&) = default;
};

struct DrawIndexedArguments
{
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 1;
    std::uint32_t firstIndex = 0;
    std::int32_t vertexOffset = 0;
    std::uint32_t firstInstance = 0;

    friend bool operator==(const DrawIndexedArguments&, const DrawIndexedArguments&) = default;
};

} // namespace LL::GHI

#endif // LL_LLGHIDESCRIPTORS_H
