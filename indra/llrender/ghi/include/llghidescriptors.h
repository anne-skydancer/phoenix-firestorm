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
#include <cstdint>
#include <optional>
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

struct ShaderPackageDesc
{
    // Stable content/permutation identity produced by the shader packager.
    std::array<std::uint8_t, 32> semanticHash{};

    friend bool operator==(const ShaderPackageDesc&, const ShaderPackageDesc&) = default;
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
    std::vector<Format> colorFormats;
    std::optional<Format> depthStencilFormat;
    std::vector<BlendState> blendStates;
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
    ImageHandle image;
    Format format = Format::Undefined;
    LoadOp load = LoadOp::Load;
    StoreOp store = StoreOp::Store;
    ClearValue clear;

    friend bool operator==(const AttachmentDesc&, const AttachmentDesc&) = default;
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
