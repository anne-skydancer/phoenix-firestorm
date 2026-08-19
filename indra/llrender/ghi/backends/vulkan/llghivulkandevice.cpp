/**
 * @file llghivulkandevice.cpp
 * @brief Vulkan 1.3 implementation of the R3 GHI draw contract.
 *
 * Native Vulkan handles remain private to this translation unit.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/core/llghidevicebackend.h"
#include "ghi/core/llghihandlepool.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LL::GHI
{
namespace
{

Status invalidArgument(const char* message) { return Status::failure(StatusCode::InvalidArgument, message); }
Status invalidState(const char* message) { return Status::failure(StatusCode::InvalidState, message); }
Status invalidHandle(const char* message) { return Status::failure(StatusCode::InvalidHandle, message); }
Status unsupported(const char* message) { return Status::failure(StatusCode::Unsupported, message); }

Status vkFailure(const char* operation, VkResult result)
{
    std::ostringstream message;
    message << operation << " failed with VkResult " << static_cast<int>(result);
    return Status::failure(result == VK_ERROR_DEVICE_LOST ? StatusCode::DeviceLost : StatusCode::BackendError,
                           message.str());
}

template<typename Tag>
std::uint64_t handleKey(Handle<Tag> handle)
{
    return (static_cast<std::uint64_t>(handle.generation()) << 32) | handle.index();
}

bool rangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t total)
{
    return offset <= total && size <= total - offset;
}

bool hasLayer(const char* requested)
{
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
    return std::any_of(layers.begin(), layers.end(), [requested](const VkLayerProperties& layer)
    {
        return std::strcmp(layer.layerName, requested) == 0;
    });
}

bool hasInstanceExtension(const char* requested)
{
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
    return std::any_of(extensions.begin(), extensions.end(), [requested](const auto& extension)
    { return std::strcmp(extension.extensionName, requested) == 0; });
}

struct VulkanFormat
{
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    std::uint32_t bytes = 0;
    bool filterable = false;
};

VulkanFormat translateFormat(Format format)
{
    switch (format)
    {
    case Format::R8UNorm: return {VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, true};
    case Format::RG8UNorm: return {VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 2, true};
    case Format::RGBA8UNorm: return {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 4, true};
    case Format::RGBA8SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, 4, true};
    case Format::BGRA8UNorm: return {VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 4, true};
    case Format::BGRA8SRGB: return {VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, 4, true};
    case Format::RGB10A2UNorm: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_ASPECT_COLOR_BIT, 4, true};
    case Format::RGBA16UNorm: return {VK_FORMAT_R16G16B16A16_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 8, true};
    case Format::RGB16Float: return {VK_FORMAT_R16G16B16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 6, true};
    case Format::R16Float: return {VK_FORMAT_R16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 2, true};
    case Format::RG16Float: return {VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 4, true};
    case Format::RGBA16Float: return {VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 8, true};
    case Format::R32Float: return {VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 4, true};
    case Format::RG32Float: return {VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 8, true};
    case Format::RGBA32Float: return {VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 16, true};
    case Format::R32UInt: return {VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, 4, false};
    case Format::Depth16UNorm: return {VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT, 2, false};
    case Format::Depth24Stencil8: return {VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 4, false};
    case Format::Depth32Float: return {VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 4, false};
    case Format::Depth32FloatStencil8: return {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 8, false};
    case Format::Undefined: break;
    }
    return {};
}

VkImageAspectFlags translateAspect(ImageAspect aspect)
{
    switch (aspect)
    {
    case ImageAspect::Color: return VK_IMAGE_ASPECT_COLOR_BIT;
    case ImageAspect::Depth: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case ImageAspect::Stencil: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case ImageAspect::DepthStencil: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return 0;
}

VkBufferUsageFlags translateBufferUsage(ResourceUsage usage)
{
    VkBufferUsageFlags result = 0;
    if (hasUsage(usage, ResourceUsage::Vertex)) result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (hasUsage(usage, ResourceUsage::Index)) result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (hasUsage(usage, ResourceUsage::Uniform)) result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (hasUsage(usage, ResourceUsage::Storage)) result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (hasUsage(usage, ResourceUsage::TransferSource)) result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (hasUsage(usage, ResourceUsage::TransferDestination)) result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return result;
}

VkImageUsageFlags translateImageUsage(ResourceUsage usage)
{
    VkImageUsageFlags result = 0;
    if (hasUsage(usage, ResourceUsage::Sampled)) result |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (hasUsage(usage, ResourceUsage::Storage)) result |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (hasUsage(usage, ResourceUsage::ColorAttachment)) result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (hasUsage(usage, ResourceUsage::DepthStencilAttachment)) result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (hasUsage(usage, ResourceUsage::TransferSource)) result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (hasUsage(usage, ResourceUsage::TransferDestination)) result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return result;
}

VkImageViewType translateViewType(ImageViewType type)
{
    switch (type)
    {
    case ImageViewType::Texture2D: return VK_IMAGE_VIEW_TYPE_2D;
    case ImageViewType::Texture2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case ImageViewType::TextureCube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case ImageViewType::TextureCubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case ImageViewType::Texture3D: return VK_IMAGE_VIEW_TYPE_3D;
    case ImageViewType::Automatic: break;
    }
    return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
}

VkSampleCountFlagBits translateSamples(std::uint8_t samples)
{
    switch (samples)
    {
    case 1: return VK_SAMPLE_COUNT_1_BIT;
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    default: return static_cast<VkSampleCountFlagBits>(0);
    }
}

VkShaderStageFlags translateVisibility(ShaderPackageDesc::StageVisibility visibility)
{
    VkShaderStageFlags flags = 0;
    const auto bits = static_cast<std::uint8_t>(visibility);
    if (bits & static_cast<std::uint8_t>(ShaderPackageDesc::StageVisibility::Vertex))
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if (bits & static_cast<std::uint8_t>(ShaderPackageDesc::StageVisibility::Fragment))
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (bits & static_cast<std::uint8_t>(ShaderPackageDesc::StageVisibility::Compute))
        flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    return flags;
}

VkDescriptorType translateDescriptorType(ShaderPackageDesc::BindingType type, bool dynamic)
{
    using Type = ShaderPackageDesc::BindingType;
    switch (type)
    {
    case Type::UniformBuffer: return dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                             : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case Type::StorageBuffer: return dynamic ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                                             : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case Type::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case Type::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case Type::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case Type::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

VkPrimitiveTopology translateTopology(PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PrimitiveTopology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PrimitiveTopology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
}

VkPolygonMode translatePolygonMode(PolygonMode mode)
{
    switch (mode)
    {
    case PolygonMode::Fill: return VK_POLYGON_MODE_FILL;
    case PolygonMode::Line: return VK_POLYGON_MODE_LINE;
    case PolygonMode::Point: return VK_POLYGON_MODE_POINT;
    }
    return VK_POLYGON_MODE_MAX_ENUM;
}

VkCompareOp translateCompare(CompareOp compare)
{
    switch (compare)
    {
    case CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case CompareOp::Less: return VK_COMPARE_OP_LESS;
    case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkStencilOp translateStencilOp(StencilOp op)
{
    switch (op)
    {
    case StencilOp::Keep: return VK_STENCIL_OP_KEEP;
    case StencilOp::Zero: return VK_STENCIL_OP_ZERO;
    case StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
    case StencilOp::IncrementClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case StencilOp::DecrementClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case StencilOp::Invert: return VK_STENCIL_OP_INVERT;
    case StencilOp::IncrementWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case StencilOp::DecrementWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

VkBlendFactor translateBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SourceColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::OneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::DestinationColor: return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::OneMinusDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::SourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::OneMinusDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp translateBlendOp(BlendOp op)
{
    switch (op)
    {
    case BlendOp::Add: return VK_BLEND_OP_ADD;
    case BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Minimum: return VK_BLEND_OP_MIN;
    case BlendOp::Maximum: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkFormat translateVertexFormat(VertexFormat format)
{
    switch (format)
    {
    case VertexFormat::Float32: return VK_FORMAT_R32_SFLOAT;
    case VertexFormat::Float32x2: return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float32x3: return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::Float32x4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexFormat::UNorm8x4: return VK_FORMAT_R8G8B8A8_UNORM;
    case VertexFormat::SNorm8x4: return VK_FORMAT_R8G8B8A8_SNORM;
    case VertexFormat::UInt16x2: return VK_FORMAT_R16G16_UINT;
    case VertexFormat::UInt16x4: return VK_FORMAT_R16G16B16A16_UINT;
    case VertexFormat::UInt32: return VK_FORMAT_R32_UINT;
    case VertexFormat::UInt32x4: return VK_FORMAT_R32G32B32A32_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkAttachmentLoadOp translateLoadOp(LoadOp op)
{
    switch (op)
    {
    case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::Discard: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp translateStoreOp(StoreOp op)
{
    return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE
                                : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VkStencilOpState translateStencilState(const StencilFaceState& state)
{
    return {translateStencilOp(state.fail), translateStencilOp(state.pass),
            translateStencilOp(state.depthFail), translateCompare(state.compare),
            state.compareMask, state.writeMask, state.reference};
}

class VulkanDevice;

class VulkanCommandContext final : public CommandContext
{
public:
    explicit VulkanCommandContext(VulkanDevice& device) : mDevice(device) {}
    Status beginFrame() override;
    Status endFrame() override;
    Status copyBuffer(BufferHandle, BufferHandle, std::span<const BufferCopyRegion>) override;
    Status copyBufferToImage(BufferHandle, ImageHandle, std::span<const BufferImageCopyRegion>) override;
    Status copyImageToBuffer(ImageHandle, BufferHandle, std::span<const BufferImageCopyRegion>) override;
    Status generateMipmaps(ImageHandle, const ImageSubresourceRange&) override;
    Status resetQueryPool(QueryPoolHandle, std::uint32_t, std::uint32_t) override;
    Status writeTimestamp(QueryPoolHandle, std::uint32_t) override;
    Status beginQuery(QueryPoolHandle, std::uint32_t) override;
    Status endQuery(QueryPoolHandle, std::uint32_t) override;
    Status beginRendering(const RenderingInfo&) override;
    Status endRendering() override;
    Status resourceBarrier(ResourceBarrier) override;
    Status bindPipeline(PipelineHandle) override;
    Status bindBindingSet(std::uint8_t, BindingSetHandle, std::span<const std::uint32_t>) override;
    Status setViewport(const Viewport&) override;
    Status setScissor(const ScissorRect&) override;
    Status bindVertexBuffer(std::uint32_t, BufferHandle, std::uint64_t) override;
    Status bindIndexBuffer(BufferHandle, std::uint64_t, IndexType) override;
    Status draw(const DrawArguments&) override;
    Status drawIndexed(const DrawIndexedArguments&) override;
    bool frameActive() const { return mFrameActive; }
    void setFrameActive(bool value) { mFrameActive = value; }
    bool renderingActive() const { return mRenderingActive; }
    void resetDrawState();
private:
    friend class VulkanDevice;
    Status requireTransfer() const;
    VulkanDevice& mDevice;
    bool mFrameActive = false;
    bool mRenderingActive = false;
    bool mViewportSet = false;
    bool mScissorSet = false;
    std::uint32_t mRenderWidth = 0;
    std::uint32_t mRenderHeight = 0;
    std::vector<Format> mRenderColorFormats;
    std::optional<Format> mRenderDepthFormat;
    PipelineHandle mPipeline;
    std::set<std::uint8_t> mBoundGroups;
    BufferHandle mIndexBuffer;
    QueryPoolHandle mActiveQueryPool;
    std::uint32_t mActiveQuery = 0;
};

class VulkanDevice final : public Device
{
public:
    explicit VulkanDevice(const DeviceCreateInfo& info) : mFramesInFlight(info.framesInFlight), mCommands(*this) {}
    ~VulkanDevice() override { shutdown(); }
    Status initialize(const DeviceCreateInfo&);

    Backend backend() const override { return Backend::Vulkan; }
    const RendererCapabilities& capabilities() const override { return mCapabilities; }
    PipelineCacheDomain pipelineCacheDomain() const override
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(mPhysicalDevice, &properties);
        static constexpr char hex[] = "0123456789abcdef";
        std::string uuid;
        uuid.reserve(VK_UUID_SIZE * 2);
        for (std::uint8_t byte : properties.pipelineCacheUUID)
        {
            uuid.push_back(hex[byte >> 4]);
            uuid.push_back(hex[byte & 0x0f]);
        }
        return {std::to_string(properties.vendorID) + ":" +
                    std::to_string(properties.deviceID) + ":" + uuid,
                std::to_string(properties.driverVersion)};
    }
    CommandContext& commandContext() override { return mCommands; }
    BufferHandle createBuffer(const BufferDesc&, Status&) override;
    ImageHandle createImage(const ImageDesc&, Status&) override;
    ImageViewHandle createImageView(const ImageViewDesc&, Status&) override;
    SamplerHandle createSampler(const SamplerDesc&, Status&) override;
    QueryPoolHandle createQueryPool(const QueryPoolDesc&, Status&) override;
    ShaderPackageHandle createShaderPackage(const ShaderPackageDesc&, Status&) override;
    BindingSetHandle createBindingSet(const BindingSetDesc&, Status&) override;
    PipelineHandle createPipeline(const PipelineDesc&, Status&) override;
    Status destroy(BufferHandle) override;
    Status destroy(ImageHandle) override;
    Status destroy(ImageViewHandle) override;
    Status destroy(SamplerHandle) override;
    Status destroy(QueryPoolHandle) override;
    Status destroy(ShaderPackageHandle) override;
    Status destroy(BindingSetHandle) override;
    Status destroy(PipelineHandle) override;
    Status writeBuffer(BufferHandle, std::uint64_t, std::span<const std::byte>) override;
    Status readBuffer(BufferHandle, std::uint64_t, std::span<std::byte>) override;
    Status getQueryResults(QueryPoolHandle, std::uint32_t, std::span<std::uint64_t>, QueryReadMode) override;
    Status waitIdle() override;

    Status beginFrame();
    Status endFrame();
    Status copyBuffer(BufferHandle, BufferHandle, std::span<const BufferCopyRegion>);
    Status copyBufferToImage(BufferHandle, ImageHandle, std::span<const BufferImageCopyRegion>);
    Status copyImageToBuffer(ImageHandle, BufferHandle, std::span<const BufferImageCopyRegion>);
    Status generateMipmaps(ImageHandle, const ImageSubresourceRange&);
    Status resetQueryPool(QueryPoolHandle, std::uint32_t, std::uint32_t);
    Status writeTimestamp(QueryPoolHandle, std::uint32_t);
    Status beginQuery(QueryPoolHandle, std::uint32_t);
    Status endQuery(QueryPoolHandle, std::uint32_t);
    Status beginRendering(const RenderingInfo&);
    Status endRendering();
    Status resourceBarrier(ResourceBarrier);
    Status bindPipeline(PipelineHandle);
    Status bindBindingSet(std::uint8_t, BindingSetHandle, std::span<const std::uint32_t>);
    Status setViewport(const Viewport&);
    Status setScissor(const ScissorRect&);
    Status bindVertexBuffer(std::uint32_t, BufferHandle, std::uint64_t);
    Status bindIndexBuffer(BufferHandle, std::uint64_t, IndexType);
    Status draw(const DrawArguments&);
    Status drawIndexed(const DrawIndexedArguments&);

private:
    struct BufferRecord { BufferDesc desc; VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; std::uint64_t readySerial = 0; };
    struct ImageRecord { ImageDesc desc; VulkanFormat format; VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; std::vector<VkImageLayout> layouts; };
    struct ViewRecord { ImageViewDesc desc; VkImageView view = VK_NULL_HANDLE; };
    struct SamplerRecord { SamplerDesc desc; VkSampler sampler = VK_NULL_HANDLE; };
    struct QueryRecord { QueryPoolDesc desc; VkQueryPool pool = VK_NULL_HANDLE; std::vector<bool> written; };
    struct ShaderStageRecord
    {
        ShaderPackageDesc::Stage stage = ShaderPackageDesc::Stage::Vertex;
        std::string entryPoint;
        VkShaderModule module = VK_NULL_HANDLE;
    };
    struct ShaderRecord
    {
        ShaderPackageDesc desc;
        std::vector<ShaderStageRecord> stages;
        std::vector<VkDescriptorSetLayout> setLayouts;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    };
    struct BindingSetRecord
    {
        BindingSetDesc desc;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    struct PipelineRecord { PipelineDesc desc; VkPipeline pipeline = VK_NULL_HANDLE; };
    struct Frame { VkCommandBuffer commands = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; std::uint64_t serial = 0; };
    enum class RetireKind { Buffer, Image, ImageView, Sampler, QueryPool, Shader, BindingSet, Pipeline };
    struct Retirement
    {
        RetireKind kind;
        std::uint64_t releaseAfter = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkQueryPool queryPool = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::vector<VkShaderModule> shaderModules;
        std::vector<VkDescriptorSetLayout> setLayouts;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    Status canMutate() const;
    std::optional<std::uint32_t> memoryType(std::uint32_t bits, VkMemoryPropertyFlags required) const;
    void pollFrames();
    void drainRetirements(bool force);
    void shutdown();
    Status transition(ImageRecord&, std::uint32_t mip, VkImageLayout layout);
    VkCommandBuffer commands() const { return mFrames[mFrameIndex].commands; }
    static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT*,
        void*);

    VkInstance mInstance = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mQueue = VK_NULL_HANDLE;
    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT mDebugMessenger = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mMemoryProperties{};
    bool mSamplerAnisotropy = false;
    bool mImageCubeArray = false;
    std::atomic_bool mValidationError = false;
    float mMaxSamplerAnisotropy = 1.f;
    RendererCapabilities mCapabilities;
    std::uint32_t mQueueFamily = 0;
    std::uint32_t mFramesInFlight = 1;
    std::uint32_t mFrameIndex = 0;
    std::uint64_t mSubmittedSerial = 0;
    std::uint64_t mCompletedSerial = 0;
    HandlePool<BufferTag> mBufferPool;
    HandlePool<ImageTag> mImagePool;
    HandlePool<ImageViewTag> mViewPool;
    HandlePool<SamplerTag> mSamplerPool;
    HandlePool<QueryPoolTag> mQueryPool;
    HandlePool<ShaderPackageTag> mShaderPool;
    HandlePool<BindingSetTag> mBindingSetPool;
    HandlePool<PipelineTag> mPipelinePool;
    std::unordered_map<std::uint64_t, BufferRecord> mBuffers;
    std::unordered_map<std::uint64_t, ImageRecord> mImages;
    std::unordered_map<std::uint64_t, ViewRecord> mViews;
    std::unordered_map<std::uint64_t, SamplerRecord> mSamplers;
    std::unordered_map<std::uint64_t, QueryRecord> mQueries;
    std::unordered_map<std::uint64_t, ShaderRecord> mShaders;
    std::unordered_map<std::uint64_t, BindingSetRecord> mBindingSets;
    std::unordered_map<std::uint64_t, PipelineRecord> mPipelines;
    std::vector<Frame> mFrames;
    std::vector<Retirement> mRetirements;
    VulkanCommandContext mCommands;
};

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::validationCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* userData)
{
    auto* device = static_cast<VulkanDevice*>(userData);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        device->mValidationError = true;
    std::fprintf(stderr, "Vulkan validation: %s\n",
                 data && data->pMessage ? data->pMessage : "unspecified message");
    return VK_FALSE;
}

Status VulkanDevice::initialize(const DeviceCreateInfo& info)
{
    constexpr const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    if (info.enableValidation && !hasLayer(validationLayer))
        return unsupported("Vulkan validation was requested but VK_LAYER_KHRONOS_validation is unavailable");
    if (info.enableValidation && !hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        return unsupported("Vulkan validation requires VK_EXT_debug_utils");
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS || loaderVersion < VK_API_VERSION_1_3)
        return unsupported("Vulkan R3 requires a Vulkan 1.3 loader");

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Vulkanstorm R3 draw peer";
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = "Vulkanstorm GHI";
    application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    application.apiVersion = VK_API_VERSION_1_3;
    const char* layers[] = {validationLayer};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledLayerCount = info.enableValidation ? 1u : 0u;
    instanceInfo.ppEnabledLayerNames = info.enableValidation ? layers : nullptr;
    const char* validationExtensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    instanceInfo.enabledExtensionCount = info.enableValidation ? 1u : 0u;
    instanceInfo.ppEnabledExtensionNames = info.enableValidation ? validationExtensions : nullptr;
    const VkValidationFeatureEnableEXT validationEnable =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &validationEnable;
    VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugInfo.pfnUserCallback = validationCallback;
    debugInfo.pUserData = this;
    validationFeatures.pNext = &debugInfo;
    instanceInfo.pNext = info.enableValidation ? &validationFeatures : nullptr;
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &mInstance);
    if (result != VK_SUCCESS) return vkFailure("vkCreateInstance", result);
    if (info.enableValidation)
    {
        auto createDebugMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(mInstance, "vkCreateDebugUtilsMessengerEXT"));
        if (!createDebugMessenger)
            return unsupported("vkCreateDebugUtilsMessengerEXT is unavailable");
        result = createDebugMessenger(mInstance, &debugInfo, nullptr, &mDebugMessenger);
        if (result != VK_SUCCESS) return vkFailure("vkCreateDebugUtilsMessengerEXT", result);
    }

    std::uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(mInstance, &deviceCount, nullptr);
    if (result != VK_SUCCESS) return vkFailure("vkEnumeratePhysicalDevices", result);
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    result = vkEnumeratePhysicalDevices(mInstance, &deviceCount, physicalDevices.data());
    if (result != VK_SUCCESS) return vkFailure("vkEnumeratePhysicalDevices", result);
    std::vector<std::pair<VkPhysicalDevice, std::uint32_t>> suitable;
    for (VkPhysicalDevice physical : physicalDevices)
    {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        for (std::uint32_t family = 0; family < count; ++family)
        {
            if (families[family].queueCount && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                suitable.emplace_back(physical, family);
                break;
            }
        }
    }
    if (info.adapterIndex >= suitable.size()) return unsupported("the requested Vulkan graphics adapter is unavailable");
    mPhysicalDevice = suitable[info.adapterIndex].first;
    mQueueFamily = suitable[info.adapterIndex].second;

    const float priority = 1.f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = mQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures availableFeatures{};
    vkGetPhysicalDeviceFeatures(mPhysicalDevice, &availableFeatures);
    VkPhysicalDeviceVulkan13Features available13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 available2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    available2.pNext = &available13;
    vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &available2);
    if (!available13.dynamicRendering)
        return unsupported("Vulkan R3 requires the Vulkan 1.3 dynamicRendering feature");
    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.samplerAnisotropy = availableFeatures.samplerAnisotropy;
    enabledFeatures.depthClamp = availableFeatures.depthClamp;
    enabledFeatures.fillModeNonSolid = availableFeatures.fillModeNonSolid;
    enabledFeatures.wideLines = availableFeatures.wideLines;
    enabledFeatures.independentBlend = availableFeatures.independentBlend;
    enabledFeatures.imageCubeArray = availableFeatures.imageCubeArray;
    enabledFeatures.fragmentStoresAndAtomics =
        availableFeatures.fragmentStoresAndAtomics;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enabled13.dynamicRendering = VK_TRUE;
    deviceInfo.pNext = &enabled13;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.pEnabledFeatures = &enabledFeatures;
    result = vkCreateDevice(mPhysicalDevice, &deviceInfo, nullptr, &mDevice);
    if (result != VK_SUCCESS) return vkFailure("vkCreateDevice", result);
    vkGetDeviceQueue(mDevice, mQueueFamily, 0, &mQueue);
    vkGetPhysicalDeviceMemoryProperties(mPhysicalDevice, &mMemoryProperties);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = mQueueFamily;
    result = vkCreateCommandPool(mDevice, &poolInfo, nullptr, &mCommandPool);
    if (result != VK_SUCCESS) return vkFailure("vkCreateCommandPool", result);
    mFrames.resize(mFramesInFlight);
    std::vector<VkCommandBuffer> commandBuffers(mFrames.size());
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = mCommandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = static_cast<std::uint32_t>(commandBuffers.size());
    result = vkAllocateCommandBuffers(mDevice, &allocation, commandBuffers.data());
    if (result != VK_SUCCESS) return vkFailure("vkAllocateCommandBuffers", result);
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (std::size_t i = 0; i < mFrames.size(); ++i)
    {
        mFrames[i].commands = commandBuffers[i];
        result = vkCreateFence(mDevice, &fenceInfo, nullptr, &mFrames[i].fence);
        if (result != VK_SUCCESS) return vkFailure("vkCreateFence", result);
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(mPhysicalDevice, &properties);
    mCapabilities.maxFramesInFlight = mFramesInFlight;
    mCapabilities.maxColorAttachments = properties.limits.maxColorAttachments;
    mCapabilities.maxSampledImagesPerStage = properties.limits.maxPerStageDescriptorSampledImages;
    mCapabilities.maxStorageBuffersPerStage = properties.limits.maxPerStageDescriptorStorageBuffers;
    mCapabilities.maxTexture2DSize = properties.limits.maxImageDimension2D;
    mCapabilities.maxUniformBufferSize = properties.limits.maxUniformBufferRange;
    mCapabilities.maxVaryingVectors = properties.limits.maxVertexOutputComponents / 4;
    mCapabilities.maxSamples = (properties.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_8_BIT) ? 8 :
                              (properties.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) ? 4 :
                              (properties.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_2_BIT) ? 2 : 1;
    mCapabilities.maxLineWidth = enabledFeatures.wideLines
        ? properties.limits.lineWidthRange[1] : 1.f;
    mCapabilities.maxBufferSize = std::numeric_limits<VkDeviceSize>::max();
    mCapabilities.uniformBufferOffsetAlignment = properties.limits.minUniformBufferOffsetAlignment;
    mCapabilities.storageBufferOffsetAlignment = properties.limits.minStorageBufferOffsetAlignment;
    VkFormatProperties r32Properties{};
    vkGetPhysicalDeviceFormatProperties(
        mPhysicalDevice, VK_FORMAT_R32_UINT, &r32Properties);
    mCapabilities.storageImageAtomics =
        enabledFeatures.fragmentStoresAndAtomics &&
        (r32Properties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) != 0;
    for (Format candidate : {Format::Depth24Stencil8, Format::Depth32FloatStencil8})
    {
        const VulkanFormat translated = translateFormat(candidate);
        VkImageFormatProperties formatProperties{};
        if (vkGetPhysicalDeviceImageFormatProperties(
                mPhysicalDevice, translated.format, VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                0, &formatProperties) == VK_SUCCESS &&
            (formatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
        {
            mCapabilities.preferredDepthStencilFormat = candidate;
            break;
        }
    }
    if (mCapabilities.preferredDepthStencilFormat == Format::Undefined)
        return unsupported("Vulkan device has no supported R3 depth/stencil attachment format");
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, families.data());
    mCapabilities.timestampQueries = families[mQueueFamily].timestampValidBits != 0;
    mCapabilities.timestampPeriodNanoseconds = properties.limits.timestampPeriod;
    mCapabilities.occlusionQueries = true;
    mCapabilities.depthClamp = enabledFeatures.depthClamp == VK_TRUE;
    mCapabilities.nonSolidFill = enabledFeatures.fillModeNonSolid == VK_TRUE;
    mCapabilities.wideLines = enabledFeatures.wideLines == VK_TRUE;
    mCapabilities.independentBlend = enabledFeatures.independentBlend == VK_TRUE;
    mCapabilities.cubeMapArrays = enabledFeatures.imageCubeArray == VK_TRUE;
    mCapabilities.baselineGraphicsPipeline = true;
    mCapabilities.advancedGraphicsPipeline = false;
    mSamplerAnisotropy = enabledFeatures.samplerAnisotropy == VK_TRUE;
    mImageCubeArray = enabledFeatures.imageCubeArray == VK_TRUE;
    mMaxSamplerAnisotropy = properties.limits.maxSamplerAnisotropy;
    return Status::success();
}

std::optional<std::uint32_t> VulkanDevice::memoryType(std::uint32_t bits, VkMemoryPropertyFlags required) const
{
    for (std::uint32_t i = 0; i < mMemoryProperties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mMemoryProperties.memoryTypes[i].propertyFlags & required) == required) return i;
    return std::nullopt;
}

Status VulkanDevice::canMutate() const
{
    return mCommands.frameActive() ? invalidState("resources may not be created or destroyed during an active frame") : Status::success();
}

BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc, Status& status)
{
    status = canMutate(); if (!status) return {};
    const VkBufferUsageFlags usage = translateBufferUsage(desc.usage);
    if (!desc.size || !usage) { status = invalidArgument("buffer size and usage must be nonzero"); return {}; }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = desc.size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    BufferRecord record; record.desc = desc;
    VkResult result = vkCreateBuffer(mDevice, &info, nullptr, &record.buffer);
    if (result != VK_SUCCESS) { status = vkFailure("vkCreateBuffer", result); return {}; }
    VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(mDevice, record.buffer, &requirements);
    const VkMemoryPropertyFlags properties = desc.memory == MemoryClass::DeviceLocal
        ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    auto type = memoryType(requirements.memoryTypeBits, properties);
    if (!type) { vkDestroyBuffer(mDevice, record.buffer, nullptr); status = unsupported("no compatible Vulkan buffer memory type"); return {}; }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = *type;
    result = vkAllocateMemory(mDevice, &allocation, nullptr, &record.memory);
    if (result == VK_SUCCESS) result = vkBindBufferMemory(mDevice, record.buffer, record.memory, 0);
    if (result != VK_SUCCESS)
    {
        if (record.memory) vkFreeMemory(mDevice, record.memory, nullptr);
        vkDestroyBuffer(mDevice, record.buffer, nullptr);
        status = vkFailure("Vulkan buffer memory allocation", result); return {};
    }
    BufferHandle handle = mBufferPool.allocate(); mBuffers.emplace(handleKey(handle), record);
    status = Status::success(); return handle;
}

ImageHandle VulkanDevice::createImage(const ImageDesc& desc, Status& status)
{
    status = canMutate(); if (!status) return {};
    const VulkanFormat format = translateFormat(desc.format);
    const VkImageUsageFlags usage = translateImageUsage(desc.usage);
    const VkSampleCountFlagBits samples = translateSamples(desc.samples);
    std::uint32_t maxDimension = std::max({desc.extent.width, desc.extent.height, desc.extent.depth});
    std::uint16_t maxMips = 1; while (maxDimension > 1) { maxDimension >>= 1; ++maxMips; }
    if (!format.format || !usage || !desc.extent.width || !desc.extent.height || !desc.extent.depth ||
        !desc.arrayLayers || !desc.mipLevels || desc.mipLevels > maxMips || !samples ||
        (desc.samples > 1 && desc.mipLevels != 1) || (desc.extent.depth > 1 && desc.arrayLayers > 1) ||
        (desc.cubeCompatible && (!mCapabilities.cubeMapArrays ||
            desc.extent.depth != 1 ||
            desc.extent.width != desc.extent.height ||
            desc.arrayLayers < 6 || desc.arrayLayers % 6 != 0)))
    { status = invalidArgument("invalid Vulkan image descriptor"); return {}; }
    ImageRecord record; record.desc = desc; record.format = format; record.layouts.resize(desc.mipLevels, VK_IMAGE_LAYOUT_UNDEFINED);
    const VkImageType imageType = desc.extent.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    VkImageFormatProperties imageProperties{};
    const VkImageCreateFlags flags = desc.cubeCompatible ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    if (vkGetPhysicalDeviceImageFormatProperties(mPhysicalDevice, format.format, imageType,
            VK_IMAGE_TILING_OPTIMAL, usage, flags, &imageProperties) != VK_SUCCESS)
    {
        status = unsupported("Vulkan image format and usage combination is unsupported");
        return {};
    }
    if (desc.extent.width > imageProperties.maxExtent.width ||
        desc.extent.height > imageProperties.maxExtent.height ||
        desc.extent.depth > imageProperties.maxExtent.depth ||
        desc.mipLevels > imageProperties.maxMipLevels ||
        desc.arrayLayers > imageProperties.maxArrayLayers ||
        (imageProperties.sampleCounts & samples) == 0)
    {
        status = unsupported("Vulkan image descriptor exceeds format capabilities");
        return {};
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = flags;
    info.imageType = imageType;
    info.format = format.format; info.extent = {desc.extent.width, desc.extent.height, desc.extent.depth};
    info.mipLevels = desc.mipLevels; info.arrayLayers = desc.arrayLayers; info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vkCreateImage(mDevice, &info, nullptr, &record.image);
    if (result != VK_SUCCESS) { status = vkFailure("vkCreateImage", result); return {}; }
    VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(mDevice, record.image, &requirements);
    auto type = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type) { vkDestroyImage(mDevice, record.image, nullptr); status = unsupported("no device-local Vulkan image memory type"); return {}; }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = *type;
    result = vkAllocateMemory(mDevice, &allocation, nullptr, &record.memory);
    if (result == VK_SUCCESS) result = vkBindImageMemory(mDevice, record.image, record.memory, 0);
    if (result != VK_SUCCESS)
    {
        if (record.memory) vkFreeMemory(mDevice, record.memory, nullptr);
        vkDestroyImage(mDevice, record.image, nullptr); status = vkFailure("Vulkan image memory allocation", result); return {};
    }
    ImageHandle handle = mImagePool.allocate(); mImages.emplace(handleKey(handle), std::move(record));
    status = Status::success(); return handle;
}

ImageViewHandle VulkanDevice::createImageView(const ImageViewDesc& desc, Status& status)
{
    status = canMutate(); if (!status) return {};
    auto image = mImages.find(handleKey(desc.image)); const auto& range = desc.subresources;
    if (!mImagePool.isLive(desc.image) || image == mImages.end()) { status = invalidHandle("image view references an invalid image"); return {}; }
    const VkImageAspectFlags aspect = translateAspect(range.aspect);
    if (desc.format != image->second.desc.format || !aspect || (aspect & image->second.format.aspect) != aspect ||
        !range.mipLevelCount || !range.arrayLayerCount || range.baseMipLevel >= image->second.desc.mipLevels ||
        range.mipLevelCount > image->second.desc.mipLevels - range.baseMipLevel ||
        range.baseArrayLayer >= image->second.desc.arrayLayers ||
        range.arrayLayerCount > image->second.desc.arrayLayers - range.baseArrayLayer ||
        !imageViewTypeCompatible(image->second.desc, desc))
    { status = invalidArgument("invalid Vulkan image view descriptor"); return {}; }
    const ImageViewType resolvedType = resolvedImageViewType(image->second.desc, desc.type);
    if (resolvedType == ImageViewType::TextureCubeArray && !mImageCubeArray)
    { status = unsupported("Vulkan cube-array image views are unavailable"); return {}; }
    ViewRecord record; record.desc = desc;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image->second.image;
    info.viewType = translateViewType(resolvedType);
    info.format = image->second.format.format;
    info.subresourceRange = {aspect, range.baseMipLevel, range.mipLevelCount, range.baseArrayLayer, range.arrayLayerCount};
    VkResult result = vkCreateImageView(mDevice, &info, nullptr, &record.view);
    if (result != VK_SUCCESS) { status = vkFailure("vkCreateImageView", result); return {}; }
    ImageViewHandle handle = mViewPool.allocate(); mViews.emplace(handleKey(handle), record);
    status = Status::success(); return handle;
}

SamplerHandle VulkanDevice::createSampler(const SamplerDesc& desc, Status& status)
{
    status = canMutate(); if (!status) return {};
    if (desc.maxAnisotropy < 1.f) { status = invalidArgument("sampler anisotropy must be at least one"); return {}; }
    const auto filter = [](Filter value) { return value == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; };
    const auto mip = [](Filter value) { return value == Filter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR; };
    const auto address = [](AddressMode value)
    {
        switch (value) { case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    SamplerRecord record; record.desc = desc;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = filter(desc.magFilter); info.minFilter = filter(desc.minFilter); info.mipmapMode = mip(desc.mipFilter);
    info.addressModeU = address(desc.addressU); info.addressModeV = address(desc.addressV); info.addressModeW = address(desc.addressW);
    info.maxLod = VK_LOD_CLAMP_NONE;
    if (desc.maxAnisotropy > 1.f)
    {
        if (!mSamplerAnisotropy) { status = unsupported("Vulkan sampler anisotropy is unavailable"); return {}; }
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(desc.maxAnisotropy, mMaxSamplerAnisotropy);
    }
    VkResult result = vkCreateSampler(mDevice, &info, nullptr, &record.sampler);
    if (result != VK_SUCCESS) { status = vkFailure("vkCreateSampler", result); return {}; }
    SamplerHandle handle = mSamplerPool.allocate(); mSamplers.emplace(handleKey(handle), record);
    status = Status::success(); return handle;
}

QueryPoolHandle VulkanDevice::createQueryPool(const QueryPoolDesc& desc, Status& status)
{
    status = canMutate(); if (!status) return {};
    if (!desc.count) { status = invalidArgument("query pool count must be nonzero"); return {}; }
    if ((desc.type == QueryType::Timestamp && !mCapabilities.timestampQueries) ||
        (desc.type == QueryType::Occlusion && !mCapabilities.occlusionQueries))
    {
        status = unsupported("requested Vulkan query type is unavailable");
        return {};
    }
    QueryRecord record; record.desc = desc; record.written.resize(desc.count, false);
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = desc.type == QueryType::Timestamp
        ? VK_QUERY_TYPE_TIMESTAMP : VK_QUERY_TYPE_OCCLUSION;
    info.queryCount = desc.count;
    VkResult result = vkCreateQueryPool(mDevice, &info, nullptr, &record.pool);
    if (result != VK_SUCCESS) { status = vkFailure("vkCreateQueryPool", result); return {}; }
    QueryPoolHandle handle = mQueryPool.allocate(); mQueries.emplace(handleKey(handle), std::move(record));
    status = Status::success(); return handle;
}

ShaderPackageHandle VulkanDevice::createShaderPackage(
    const ShaderPackageDesc& desc, Status& status)
{
    status = canMutate(); if (!status) return {};
    if (desc.schemaVersion != ShaderPackageDesc::CURRENT_SCHEMA_VERSION)
    { status = invalidArgument("Vulkan shader package schema is unsupported"); return {}; }

    ShaderRecord record;
    record.desc = desc;
    auto cleanup = [&]()
    {
        if (record.pipelineLayout) vkDestroyPipelineLayout(mDevice, record.pipelineLayout, nullptr);
        for (VkDescriptorSetLayout layout : record.setLayouts)
            if (layout) vkDestroyDescriptorSetLayout(mDevice, layout, nullptr);
        for (const auto& stage : record.stages)
            if (stage.module) vkDestroyShaderModule(mDevice, stage.module, nullptr);
    };

    bool vertex = false;
    bool fragment = false;
    for (const auto& stage : desc.stages)
    {
        if (stage.stage == ShaderPackageDesc::Stage::Vertex) vertex = true;
        else if (stage.stage == ShaderPackageDesc::Stage::Fragment) fragment = true;
        else { status = unsupported("R3e Vulkan supports graphics shader packages only"); cleanup(); return {}; }
        auto artifact = std::find_if(stage.artifacts.begin(), stage.artifacts.end(), [](const auto& value)
        { return value.target == ShaderPackageDesc::TargetProfile::VulkanSpirV13; });
        if (artifact == stage.artifacts.end() || artifact->spirv.empty())
        { status = invalidArgument("shader package lacks Vulkan 1.3 SPIR-V"); cleanup(); return {}; }
        ShaderStageRecord native;
        native.stage = stage.stage;
        native.entryPoint = stage.entryPoint;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = artifact->spirv.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = artifact->spirv.data();
        VkResult result = vkCreateShaderModule(mDevice, &moduleInfo, nullptr, &native.module);
        if (result != VK_SUCCESS)
        { status = vkFailure("vkCreateShaderModule", result); cleanup(); return {}; }
        record.stages.push_back(std::move(native));
    }
    if (!vertex || !fragment)
    { status = invalidArgument("Vulkan graphics package requires vertex and fragment stages"); cleanup(); return {}; }

    std::uint8_t maxGroup = 0;
    for (const auto& binding : desc.bindings) maxGroup = std::max(maxGroup, binding.group);
    record.setLayouts.resize(desc.bindings.empty() ? 0 : static_cast<std::size_t>(maxGroup) + 1);
    for (std::size_t group = 0; group < record.setLayouts.size(); ++group)
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const auto& binding : desc.bindings)
        {
            if (binding.group != group) continue;
            VkDescriptorSetLayoutBinding native{};
            native.binding = binding.binding;
            native.descriptorType = translateDescriptorType(binding.type, binding.dynamicOffset);
            native.descriptorCount = binding.arrayCount;
            native.stageFlags = translateVisibility(binding.visibility);
            if (native.descriptorType == VK_DESCRIPTOR_TYPE_MAX_ENUM || !native.stageFlags || !native.descriptorCount)
            { status = invalidArgument("shader reflection contains an invalid Vulkan binding"); cleanup(); return {}; }
            bindings.push_back(native);
        }
        std::sort(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs)
        { return lhs.binding < rhs.binding; });
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(
            mDevice, &layoutInfo, nullptr, &record.setLayouts[group]);
        if (result != VK_SUCCESS)
        { status = vkFailure("vkCreateDescriptorSetLayout", result); cleanup(); return {}; }
    }
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = static_cast<std::uint32_t>(record.setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = record.setLayouts.data();
    VkResult result = vkCreatePipelineLayout(
        mDevice, &pipelineLayoutInfo, nullptr, &record.pipelineLayout);
    if (result != VK_SUCCESS)
    { status = vkFailure("vkCreatePipelineLayout", result); cleanup(); return {}; }

    ShaderPackageHandle handle = mShaderPool.allocate();
    mShaders.emplace(handleKey(handle), std::move(record));
    status = Status::success();
    return handle;
}

BindingSetHandle VulkanDevice::createBindingSet(
    const BindingSetDesc& desc, Status& status)
{
    status = canMutate(); if (!status) return {};
    auto shader = mShaders.find(handleKey(desc.shader));
    if (!mShaderPool.isLive(desc.shader) || shader == mShaders.end() ||
        desc.group >= shader->second.setLayouts.size())
    { status = invalidHandle("binding set references an invalid shader package or group"); return {}; }

    std::size_t expected = 0;
    for (const auto& reflected : shader->second.desc.bindings)
        if (reflected.group == desc.group) expected += reflected.arrayCount;
    if (expected != desc.resources.size())
    { status = invalidArgument("binding set does not exactly cover its reflected group"); return {}; }

    std::map<VkDescriptorType, std::uint32_t> counts;
    for (const auto& resource : desc.resources)
    {
        auto reflected = std::find_if(shader->second.desc.bindings.begin(),
            shader->second.desc.bindings.end(), [&](const auto& binding)
            {
                return binding.group == desc.group && binding.binding == resource.binding &&
                       binding.type == resource.type && resource.arrayElement < binding.arrayCount;
            });
        if (reflected == shader->second.desc.bindings.end())
        { status = invalidArgument("binding resource does not match Vulkan reflection"); return {}; }
        ++counts[translateDescriptorType(reflected->type, reflected->dynamicOffset)];
    }

    std::vector<VkDescriptorPoolSize> sizes;
    for (const auto& [type, count] : counts) sizes.push_back({type, count});
    BindingSetRecord record;
    record.desc = desc;
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    VkResult result = vkCreateDescriptorPool(mDevice, &poolInfo, nullptr, &record.pool);
    if (result != VK_SUCCESS)
    { status = vkFailure("vkCreateDescriptorPool", result); return {}; }
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = record.pool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &shader->second.setLayouts[desc.group];
    result = vkAllocateDescriptorSets(mDevice, &allocation, &record.set);
    if (result != VK_SUCCESS)
    {
        vkDestroyDescriptorPool(mDevice, record.pool, nullptr);
        status = vkFailure("vkAllocateDescriptorSets", result); return {};
    }

    std::vector<VkDescriptorBufferInfo> bufferInfos(desc.resources.size());
    std::vector<VkDescriptorImageInfo> imageInfos(desc.resources.size());
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(desc.resources.size());
    for (std::size_t i = 0; i < desc.resources.size(); ++i)
    {
        const auto& resource = desc.resources[i];
        const auto reflected = std::find_if(shader->second.desc.bindings.begin(),
            shader->second.desc.bindings.end(), [&](const auto& binding)
            { return binding.group == desc.group && binding.binding == resource.binding; });
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = record.set;
        write.dstBinding = resource.binding;
        write.dstArrayElement = resource.arrayElement;
        write.descriptorCount = 1;
        write.descriptorType = translateDescriptorType(reflected->type, reflected->dynamicOffset);
        if (resource.type == ShaderPackageDesc::BindingType::UniformBuffer ||
            resource.type == ShaderPackageDesc::BindingType::StorageBuffer)
        {
            auto buffer = mBuffers.find(handleKey(resource.buffer));
            const ResourceUsage required = resource.type == ShaderPackageDesc::BindingType::UniformBuffer
                ? ResourceUsage::Uniform : ResourceUsage::Storage;
            if (!mBufferPool.isLive(resource.buffer) || buffer == mBuffers.end() ||
                !hasUsage(buffer->second.desc.usage, required) || resource.bufferOffset >= buffer->second.desc.size)
            { vkDestroyDescriptorPool(mDevice, record.pool, nullptr); status = invalidArgument("buffer descriptor is invalid"); return {}; }
            const std::uint64_t range = resource.bufferRange ? resource.bufferRange :
                buffer->second.desc.size - resource.bufferOffset;
            if (!rangeFits(resource.bufferOffset, range, buffer->second.desc.size))
            { vkDestroyDescriptorPool(mDevice, record.pool, nullptr); status = invalidArgument("buffer descriptor range is invalid"); return {}; }
            bufferInfos[i] = {buffer->second.buffer, resource.bufferOffset, range};
            write.pBufferInfo = &bufferInfos[i];
        }
        else
        {
            if (resource.imageView)
            {
                auto view = mViews.find(handleKey(resource.imageView));
                if (!mViewPool.isLive(resource.imageView) || view == mViews.end())
                { vkDestroyDescriptorPool(mDevice, record.pool, nullptr); status = invalidHandle("image descriptor view is invalid"); return {}; }
                imageInfos[i].imageView = view->second.view;
                imageInfos[i].imageLayout = resource.type == ShaderPackageDesc::BindingType::StorageImage
                    ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            if (resource.sampler)
            {
                auto sampler = mSamplers.find(handleKey(resource.sampler));
                if (!mSamplerPool.isLive(resource.sampler) || sampler == mSamplers.end())
                { vkDestroyDescriptorPool(mDevice, record.pool, nullptr); status = invalidHandle("image descriptor sampler is invalid"); return {}; }
                imageInfos[i].sampler = sampler->second.sampler;
            }
            write.pImageInfo = &imageInfos[i];
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(mDevice, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    BindingSetHandle handle = mBindingSetPool.allocate();
    mBindingSets.emplace(handleKey(handle), std::move(record));
    status = Status::success();
    return handle;
}

PipelineHandle VulkanDevice::createPipeline(const PipelineDesc& desc, Status& status)
{
    status = canMutate(); if (!status) return {};
    auto shader = mShaders.find(handleKey(desc.shader));
    if (!mShaderPool.isLive(desc.shader) || shader == mShaders.end())
    { status = invalidHandle("pipeline references an invalid shader package"); return {}; }
    const VkSampleCountFlagBits samples = translateSamples(desc.samples);
    if (!samples ||
        (desc.colorFormats.empty() && !desc.depthStencilFormat.has_value()) ||
        desc.colorFormats.size() != desc.blendStates.size())
    { status = invalidArgument("invalid Vulkan graphics pipeline descriptor"); return {}; }
    if (desc.depthClamp && !mCapabilities.depthClamp)
    { status = unsupported("Vulkan depth clamp is unavailable"); return {}; }
    if (translatePolygonMode(desc.polygonMode) == VK_POLYGON_MODE_MAX_ENUM ||
        !std::isfinite(desc.lineWidth) || desc.lineWidth <= 0.f ||
        desc.lineWidth > mCapabilities.maxLineWidth ||
        !std::isfinite(desc.depthBiasConstantFactor) ||
        !std::isfinite(desc.depthBiasSlopeFactor))
    { status = invalidArgument("invalid Vulkan rasterization state"); return {}; }
    if (desc.polygonMode != PolygonMode::Fill && !mCapabilities.nonSolidFill)
    { status = unsupported("Vulkan non-solid polygon rasterization is unavailable"); return {}; }
    if (desc.lineWidth != 1.f && !mCapabilities.wideLines)
    { status = unsupported("Vulkan wide lines are unavailable"); return {}; }
    if (!mCapabilities.independentBlend && desc.blendStates.size() > 1 &&
        !std::all_of(desc.blendStates.begin() + 1, desc.blendStates.end(),
                     [&](const BlendState& blend) { return blend == desc.blendStates.front(); }))
    { status = unsupported("Vulkan independent color attachment state is unavailable"); return {}; }

    std::vector<VkSpecializationMapEntry> specializationEntries;
    std::vector<std::byte> specializationData;
    for (const auto& value : desc.specializationConstants)
    {
        specializationEntries.push_back({value.id,
            static_cast<std::uint32_t>(specializationData.size()), value.size});
        specializationData.insert(specializationData.end(), value.value.begin(),
                                  value.value.begin() + value.size);
    }
    VkSpecializationInfo specialization{};
    specialization.mapEntryCount = static_cast<std::uint32_t>(specializationEntries.size());
    specialization.pMapEntries = specializationEntries.data();
    specialization.dataSize = specializationData.size();
    specialization.pData = specializationData.data();

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    for (const auto& native : shader->second.stages)
    {
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = native.stage == ShaderPackageDesc::Stage::Vertex
            ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stage.module = native.module;
        stage.pName = native.entryPoint.c_str();
        stage.pSpecializationInfo = specializationEntries.empty() ? nullptr : &specialization;
        stages.push_back(stage);
    }

    std::vector<VkVertexInputBindingDescription> vertexBindings;
    for (const auto& value : desc.vertexBuffers)
        vertexBindings.push_back({value.slot, value.stride,
            value.inputRate == VertexInputRate::PerVertex ? VK_VERTEX_INPUT_RATE_VERTEX
                                                          : VK_VERTEX_INPUT_RATE_INSTANCE});
    std::vector<VkVertexInputAttributeDescription> vertexAttributes;
    for (const auto& value : desc.vertexAttributes)
        vertexAttributes.push_back({value.location, value.bufferSlot,
            translateVertexFormat(value.format), value.offset});
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(vertexBindings.size());
    vertexInput.pVertexBindingDescriptions = vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions = vertexAttributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = translateTopology(desc.topology);
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1; viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = desc.depthClamp;
    raster.polygonMode = translatePolygonMode(desc.polygonMode);
    raster.cullMode = desc.cullMode == CullMode::None ? VK_CULL_MODE_NONE :
        desc.cullMode == CullMode::Front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    // Front-face state is expressed in the GHI's canonical framebuffer
    // convention. The shader prelude owns the clip-space Y conversion, so the
    // native winding selection is not inverted again here.
    raster.frontFace = desc.frontFaceCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                      : VK_FRONT_FACE_CLOCKWISE;
    raster.depthBiasEnable = desc.depthBias;
    raster.depthBiasConstantFactor = desc.depthBiasConstantFactor;
    raster.depthBiasSlopeFactor = desc.depthBiasSlopeFactor;
    raster.lineWidth = desc.lineWidth;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = desc.depthTest;
    depth.depthWriteEnable = desc.depthWrite;
    depth.depthCompareOp = translateCompare(desc.depthCompare);
    depth.stencilTestEnable = desc.stencilTest;
    depth.front = translateStencilState(desc.frontStencil);
    depth.back = translateStencilState(desc.backStencil);
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
    for (const auto& value : desc.blendStates)
    {
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = value.enabled;
        blend.srcColorBlendFactor = translateBlendFactor(value.sourceColor);
        blend.dstColorBlendFactor = translateBlendFactor(value.destinationColor);
        blend.colorBlendOp = translateBlendOp(value.colorOp);
        blend.srcAlphaBlendFactor = translateBlendFactor(value.sourceAlpha);
        blend.dstAlphaBlendFactor = translateBlendFactor(value.destinationAlpha);
        blend.alphaBlendOp = translateBlendOp(value.alphaOp);
        if (value.colorWriteMask & 1) blend.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
        if (value.colorWriteMask & 2) blend.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
        if (value.colorWriteMask & 4) blend.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
        if (value.colorWriteMask & 8) blend.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
        blendAttachments.push_back(blend);
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
    blend.pAttachments = blendAttachments.data();
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    std::vector<VkFormat> colorFormats;
    for (Format format : desc.colorFormats) colorFormats.push_back(translateFormat(format).format);
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(colorFormats.size());
    rendering.pColorAttachmentFormats = colorFormats.empty()
        ? nullptr : colorFormats.data();
    if (desc.depthStencilFormat)
    {
        const auto format = translateFormat(*desc.depthStencilFormat);
        rendering.depthAttachmentFormat = format.format;
        if (format.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) rendering.stencilAttachmentFormat = format.format;
    }
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depth;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = shader->second.pipelineLayout;
    PipelineRecord record{desc};
    VkResult result = vkCreateGraphicsPipelines(
        mDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &record.pipeline);
    if (result != VK_SUCCESS)
    { status = vkFailure("vkCreateGraphicsPipelines", result); return {}; }
    PipelineHandle handle = mPipelinePool.allocate();
    mPipelines.emplace(handleKey(handle), std::move(record));
    status = Status::success();
    return handle;
}

Status VulkanDevice::destroy(BufferHandle handle)
{
    Status status = canMutate(); if (!status) return status; auto found = mBuffers.find(handleKey(handle));
    if (!mBufferPool.release(handle) || found == mBuffers.end()) return invalidHandle("invalid buffer handle");
    Retirement item{RetireKind::Buffer, mSubmittedSerial + mFramesInFlight}; item.buffer = found->second.buffer; item.memory = found->second.memory;
    mRetirements.push_back(item); mBuffers.erase(found); return Status::success();
}

Status VulkanDevice::destroy(ImageHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    for (const auto& [unused, view] : mViews) if (view.desc.image == handle) return invalidState("image must outlive its image views");
    auto found = mImages.find(handleKey(handle)); if (!mImagePool.release(handle) || found == mImages.end()) return invalidHandle("invalid image handle");
    Retirement item{RetireKind::Image, mSubmittedSerial + mFramesInFlight}; item.image = found->second.image; item.memory = found->second.memory;
    mRetirements.push_back(item); mImages.erase(found); return Status::success();
}

Status VulkanDevice::destroy(ImageViewHandle handle)
{
    Status status = canMutate(); if (!status) return status; auto found = mViews.find(handleKey(handle));
    if (!mViewPool.release(handle) || found == mViews.end()) return invalidHandle("invalid image-view handle");
    Retirement item{RetireKind::ImageView, mSubmittedSerial + mFramesInFlight}; item.view = found->second.view;
    mRetirements.push_back(item); mViews.erase(found); return Status::success();
}

Status VulkanDevice::destroy(SamplerHandle handle)
{
    Status status = canMutate(); if (!status) return status; auto found = mSamplers.find(handleKey(handle));
    if (!mSamplerPool.release(handle) || found == mSamplers.end()) return invalidHandle("invalid sampler handle");
    Retirement item{RetireKind::Sampler, mSubmittedSerial + mFramesInFlight}; item.sampler = found->second.sampler;
    mRetirements.push_back(item); mSamplers.erase(found); return Status::success();
}

Status VulkanDevice::destroy(QueryPoolHandle handle)
{
    Status status = canMutate(); if (!status) return status; auto found = mQueries.find(handleKey(handle));
    if (!mQueryPool.release(handle) || found == mQueries.end()) return invalidHandle("invalid query-pool handle");
    Retirement item{RetireKind::QueryPool, mSubmittedSerial + mFramesInFlight}; item.queryPool = found->second.pool;
    mRetirements.push_back(item); mQueries.erase(found); return Status::success();
}

Status VulkanDevice::destroy(ShaderPackageHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    for (const auto& [unused, set] : mBindingSets)
        if (set.desc.shader == handle) return invalidState("shader package must outlive its binding sets");
    for (const auto& [unused, pipeline] : mPipelines)
        if (pipeline.desc.shader == handle) return invalidState("shader package must outlive its pipelines");
    auto found = mShaders.find(handleKey(handle));
    if (!mShaderPool.release(handle) || found == mShaders.end())
        return invalidHandle("invalid shader-package handle");
    Retirement item{RetireKind::Shader, mSubmittedSerial + mFramesInFlight};
    item.pipelineLayout = found->second.pipelineLayout;
    item.setLayouts = std::move(found->second.setLayouts);
    for (const auto& stage : found->second.stages) item.shaderModules.push_back(stage.module);
    mRetirements.push_back(std::move(item));
    mShaders.erase(found);
    return Status::success();
}

Status VulkanDevice::destroy(BindingSetHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    auto found = mBindingSets.find(handleKey(handle));
    if (!mBindingSetPool.release(handle) || found == mBindingSets.end())
        return invalidHandle("invalid binding-set handle");
    Retirement item{RetireKind::BindingSet, mSubmittedSerial + mFramesInFlight};
    item.descriptorPool = found->second.pool;
    mRetirements.push_back(std::move(item));
    mBindingSets.erase(found);
    return Status::success();
}

Status VulkanDevice::destroy(PipelineHandle handle)
{
    Status status = canMutate(); if (!status) return status;
    auto found = mPipelines.find(handleKey(handle));
    if (!mPipelinePool.release(handle) || found == mPipelines.end())
        return invalidHandle("invalid pipeline handle");
    Retirement item{RetireKind::Pipeline, mSubmittedSerial + mFramesInFlight};
    item.pipeline = found->second.pipeline;
    mRetirements.push_back(std::move(item));
    mPipelines.erase(found);
    return Status::success();
}

Status VulkanDevice::writeBuffer(BufferHandle handle, std::uint64_t offset, std::span<const std::byte> data)
{
    if (mCommands.frameActive()) return invalidState("host writes are not allowed during an active frame");
    auto found = mBuffers.find(handleKey(handle)); if (!mBufferPool.isLive(handle) || found == mBuffers.end()) return invalidHandle("invalid buffer handle");
    if (found->second.desc.memory != MemoryClass::Upload) return invalidArgument("writeBuffer requires an upload buffer");
    if (!rangeFits(offset, data.size(), found->second.desc.size)) return invalidArgument("writeBuffer range exceeds the buffer");
    if (data.empty()) return Status::success();
    void* mapped = nullptr; VkResult result = vkMapMemory(mDevice, found->second.memory, offset, data.size(), 0, &mapped);
    if (result != VK_SUCCESS) return vkFailure("vkMapMemory for upload", result);
    std::memcpy(mapped, data.data(), data.size()); vkUnmapMemory(mDevice, found->second.memory); return Status::success();
}

void VulkanDevice::pollFrames()
{
    for (const Frame& frame : mFrames)
        if (frame.serial && vkGetFenceStatus(mDevice, frame.fence) == VK_SUCCESS) mCompletedSerial = std::max(mCompletedSerial, frame.serial);
}

Status VulkanDevice::readBuffer(BufferHandle handle, std::uint64_t offset, std::span<std::byte> data)
{
    if (mCommands.frameActive()) return invalidState("host reads are not allowed during an active frame");
    auto found = mBuffers.find(handleKey(handle)); if (!mBufferPool.isLive(handle) || found == mBuffers.end()) return invalidHandle("invalid buffer handle");
    if (found->second.desc.memory != MemoryClass::Readback) return invalidArgument("readBuffer requires a readback buffer");
    if (!rangeFits(offset, data.size(), found->second.desc.size)) return invalidArgument("readBuffer range exceeds the buffer");
    pollFrames(); if (found->second.readySerial > mCompletedSerial) return Status::failure(StatusCode::NotReady, "readback buffer is not ready");
    if (data.empty()) return Status::success();
    void* mapped = nullptr; VkResult result = vkMapMemory(mDevice, found->second.memory, offset, data.size(), 0, &mapped);
    if (result != VK_SUCCESS) return vkFailure("vkMapMemory for readback", result);
    std::memcpy(data.data(), mapped, data.size()); vkUnmapMemory(mDevice, found->second.memory); return Status::success();
}

Status VulkanDevice::getQueryResults(QueryPoolHandle pool, std::uint32_t first, std::span<std::uint64_t> results, QueryReadMode mode)
{
    if (mCommands.frameActive()) return invalidState("query results may not be read during an active frame");
    auto found = mQueries.find(handleKey(pool)); if (!mQueryPool.isLive(pool) || found == mQueries.end()) return invalidHandle("invalid query-pool handle");
    if (results.empty() || first >= found->second.desc.count || results.size() > found->second.desc.count - first) return invalidArgument("query result range is empty or out of bounds");
    for (std::size_t i = 0; i < results.size(); ++i) if (!found->second.written[first + i]) return Status::failure(StatusCode::NotReady, "query has not been written");
    VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
    if (mode == QueryReadMode::Wait) flags |= VK_QUERY_RESULT_WAIT_BIT;
    VkResult result = vkGetQueryPoolResults(mDevice, found->second.pool, first, static_cast<std::uint32_t>(results.size()),
        results.size_bytes(), results.data(), sizeof(std::uint64_t), flags);
    if (result == VK_NOT_READY) return Status::failure(StatusCode::NotReady, "query result is not ready");
    return result == VK_SUCCESS ? Status::success() : vkFailure("vkGetQueryPoolResults", result);
}

Status VulkanDevice::beginFrame()
{
    if (mCommands.frameActive()) return invalidState("a frame is already active");
    mFrameIndex = static_cast<std::uint32_t>(mSubmittedSerial % mFrames.size()); Frame& frame = mFrames[mFrameIndex];
    VkResult result = vkWaitForFences(mDevice, 1, &frame.fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
    if (result != VK_SUCCESS) return vkFailure("vkWaitForFences", result);
    if (frame.serial) mCompletedSerial = std::max(mCompletedSerial, frame.serial);
    drainRetirements(false);
    result = vkResetFences(mDevice, 1, &frame.fence); if (result != VK_SUCCESS) return vkFailure("vkResetFences", result);
    result = vkResetCommandBuffer(frame.commands, 0); if (result != VK_SUCCESS) return vkFailure("vkResetCommandBuffer", result);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(frame.commands, &begin); if (result != VK_SUCCESS) return vkFailure("vkBeginCommandBuffer", result);
    mCommands.resetDrawState();
    mCommands.setFrameActive(true); return Status::success();
}

Status VulkanDevice::endFrame()
{
    if (!mCommands.frameActive()) return invalidState("no frame is active");
    if (mCommands.renderingActive()) return invalidState("endFrame requires endRendering first");
    if (mCommands.mActiveQueryPool) return invalidState("endFrame has an active occlusion query");
    Frame& frame = mFrames[mFrameIndex];
    VkResult result = vkEndCommandBuffer(frame.commands); if (result != VK_SUCCESS) { mCommands.setFrameActive(false); return vkFailure("vkEndCommandBuffer", result); }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &frame.commands;
    result = vkQueueSubmit(mQueue, 1, &submit, frame.fence); mCommands.setFrameActive(false);
    if (result != VK_SUCCESS) return vkFailure("vkQueueSubmit", result);
    frame.serial = ++mSubmittedSerial;
    if (mValidationError.load())
        return Status::failure(StatusCode::BackendError,
            "Khronos validation reported a Vulkan error");
    return Status::success();
}

Status VulkanDevice::transition(ImageRecord& image, std::uint32_t mip, VkImageLayout layout)
{
    if (image.layouts[mip] == layout) return Status::success();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = image.layouts[mip]; barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {image.format.aspect, mip, 1, 0, image.desc.arrayLayers};
    const auto source = [](VkImageLayout value)
    {
        switch (value)
        {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return std::pair<VkPipelineStageFlags, VkAccessFlags>{VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return std::pair<VkPipelineStageFlags, VkAccessFlags>{VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return std::pair<VkPipelineStageFlags, VkAccessFlags>{VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return std::pair<VkPipelineStageFlags, VkAccessFlags>{
                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT};
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return std::pair<VkPipelineStageFlags, VkAccessFlags>{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return std::pair<VkPipelineStageFlags, VkAccessFlags>{
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_GENERAL:
            return std::pair<VkPipelineStageFlags, VkAccessFlags>{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        default:
            return std::pair<VkPipelineStageFlags, VkAccessFlags>{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        }
    };
    const auto [sourceStage, sourceAccess] = source(barrier.oldLayout);
    const auto [destinationStage, destinationAccess] = source(layout);
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    vkCmdPipelineBarrier(commands(), sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    image.layouts[mip] = layout; return Status::success();
}

Status VulkanDevice::copyBuffer(BufferHandle source, BufferHandle destination, std::span<const BufferCopyRegion> regions)
{
    auto src = mBuffers.find(handleKey(source)); auto dst = mBuffers.find(handleKey(destination));
    if (!mBufferPool.isLive(source) || !mBufferPool.isLive(destination) || src == mBuffers.end() || dst == mBuffers.end()) return invalidHandle("copyBuffer received an invalid buffer");
    if (!hasUsage(src->second.desc.usage, ResourceUsage::TransferSource) || !hasUsage(dst->second.desc.usage, ResourceUsage::TransferDestination) || regions.empty()) return invalidArgument("copyBuffer usage or regions are invalid");
    std::vector<VkBufferCopy> copies; copies.reserve(regions.size());
    for (const auto& region : regions)
    {
        if (!region.size || ((region.sourceOffset | region.destinationOffset | region.size) & 3u) != 0 ||
            !rangeFits(region.sourceOffset, region.size, src->second.desc.size) ||
            !rangeFits(region.destinationOffset, region.size, dst->second.desc.size))
            return invalidArgument("copyBuffer region is unaligned or out of bounds");
        copies.push_back({region.sourceOffset, region.destinationOffset, region.size});
    }
    // A dynamic destination can still be consumed by an earlier in-flight
    // submission on this queue. Establish a buffer-scoped dependency before
    // rewriting it; submission order alone is not a memory dependency.
    VkBufferMemoryBarrier reuseBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    reuseBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    reuseBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    reuseBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    reuseBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    reuseBarrier.buffer = dst->second.buffer;
    reuseBarrier.offset = 0;
    reuseBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &reuseBarrier,
        0, nullptr);
    vkCmdCopyBuffer(commands(), src->second.buffer, dst->second.buffer, static_cast<std::uint32_t>(copies.size()), copies.data());
    // The GHI transfer contract permits a later transfer command in the same
    // frame to consume this destination. Until explicit transfer batches are
    // introduced, preserve that ordering conservatively at the backend seam.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commands(), VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (dst->second.desc.memory == MemoryClass::Readback) dst->second.readySerial = mSubmittedSerial + 1;
    return Status::success();
}

Status VulkanDevice::copyBufferToImage(BufferHandle source, ImageHandle destination, std::span<const BufferImageCopyRegion> regions)
{
    auto src = mBuffers.find(handleKey(source)); auto dst = mImages.find(handleKey(destination));
    if (!mBufferPool.isLive(source) || !mImagePool.isLive(destination) || src == mBuffers.end() || dst == mImages.end()) return invalidHandle("buffer-to-image copy received an invalid resource");
    if (!hasUsage(src->second.desc.usage, ResourceUsage::TransferSource) || !hasUsage(dst->second.desc.usage, ResourceUsage::TransferDestination) || regions.empty()) return invalidArgument("buffer-to-image usage or regions are invalid");
    if (dst->second.format.aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return unsupported("Vulkan R2 buffer-to-image copies currently support color images only");
    std::vector<VkBufferImageCopy> copies; copies.reserve(regions.size());
    for (const auto& region : regions)
    {
        const auto& sub = region.imageSubresource;
        if (translateAspect(sub.aspect) != dst->second.format.aspect || sub.mipLevel >= dst->second.desc.mipLevels ||
            !sub.arrayLayerCount || sub.baseArrayLayer + sub.arrayLayerCount > dst->second.desc.arrayLayers ||
            region.imageOffset.x < 0 || region.imageOffset.y < 0 || region.imageOffset.z < 0) return invalidArgument("invalid buffer-to-image subresource");
        const std::uint32_t width = std::max(1u, dst->second.desc.extent.width >> sub.mipLevel);
        const std::uint32_t height = std::max(1u, dst->second.desc.extent.height >> sub.mipLevel);
        const std::uint32_t depth = std::max(1u, dst->second.desc.extent.depth >> sub.mipLevel);
        if (!region.imageExtent.width || !region.imageExtent.height || !region.imageExtent.depth ||
            (region.bufferRowLength && region.bufferRowLength < region.imageExtent.width) ||
            (region.bufferImageHeight && region.bufferImageHeight < region.imageExtent.height) ||
            (region.bufferOffset & 3u) != 0 ||
            static_cast<std::uint32_t>(region.imageOffset.x) + region.imageExtent.width > width ||
            static_cast<std::uint32_t>(region.imageOffset.y) + region.imageExtent.height > height ||
            static_cast<std::uint32_t>(region.imageOffset.z) + region.imageExtent.depth > depth) return invalidArgument("buffer-to-image extent is out of bounds");
        const std::uint32_t row = region.bufferRowLength ? region.bufferRowLength : region.imageExtent.width;
        const std::uint32_t rows = region.bufferImageHeight ? region.bufferImageHeight : region.imageExtent.height;
        const std::uint32_t slices = dst->second.desc.extent.depth > 1 ? region.imageExtent.depth : sub.arrayLayerCount;
        if (!rangeFits(region.bufferOffset, static_cast<std::uint64_t>(row) * rows * slices * dst->second.format.bytes, src->second.desc.size)) return invalidArgument("buffer-to-image source range is out of bounds");
        transition(dst->second, sub.mipLevel, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{}; copy.bufferOffset = region.bufferOffset; copy.bufferRowLength = region.bufferRowLength; copy.bufferImageHeight = region.bufferImageHeight;
        copy.imageSubresource = {dst->second.format.aspect, sub.mipLevel, sub.baseArrayLayer, sub.arrayLayerCount};
        copy.imageOffset = {region.imageOffset.x, region.imageOffset.y, region.imageOffset.z};
        copy.imageExtent = {region.imageExtent.width, region.imageExtent.height, region.imageExtent.depth}; copies.push_back(copy);
    }
    VkMemoryBarrier reuseBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    reuseBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    reuseBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commands(), VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuseBarrier, 0, nullptr, 0, nullptr);
    vkCmdCopyBufferToImage(commands(), src->second.buffer, dst->second.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(copies.size()), copies.data()); return Status::success();
}

Status VulkanDevice::copyImageToBuffer(ImageHandle source, BufferHandle destination, std::span<const BufferImageCopyRegion> regions)
{
    auto src = mImages.find(handleKey(source)); auto dst = mBuffers.find(handleKey(destination));
    if (!mImagePool.isLive(source) || !mBufferPool.isLive(destination) || src == mImages.end() || dst == mBuffers.end()) return invalidHandle("image-to-buffer copy received an invalid resource");
    if (!hasUsage(src->second.desc.usage, ResourceUsage::TransferSource) || !hasUsage(dst->second.desc.usage, ResourceUsage::TransferDestination) || regions.empty()) return invalidArgument("image-to-buffer usage or regions are invalid");
    if (src->second.format.aspect != VK_IMAGE_ASPECT_COLOR_BIT &&
        src->second.format.aspect != VK_IMAGE_ASPECT_DEPTH_BIT)
        return unsupported("Vulkan image-to-buffer copies require a single color or depth aspect");
    std::vector<VkBufferImageCopy> copies; copies.reserve(regions.size());
    for (const auto& region : regions)
    {
        const auto& sub = region.imageSubresource;
        if (translateAspect(sub.aspect) != src->second.format.aspect || sub.mipLevel >= src->second.desc.mipLevels || !sub.arrayLayerCount ||
            sub.baseArrayLayer + sub.arrayLayerCount > src->second.desc.arrayLayers ||
            region.imageOffset.x < 0 || region.imageOffset.y < 0 || region.imageOffset.z < 0 ||
            !region.imageExtent.width || !region.imageExtent.height || !region.imageExtent.depth ||
            (region.bufferRowLength && region.bufferRowLength < region.imageExtent.width) ||
            (region.bufferImageHeight && region.bufferImageHeight < region.imageExtent.height) ||
            (region.bufferOffset & 3u) != 0) return invalidArgument("invalid image-to-buffer subresource");
        const std::uint32_t width = std::max(1u, src->second.desc.extent.width >> sub.mipLevel);
        const std::uint32_t height = std::max(1u, src->second.desc.extent.height >> sub.mipLevel);
        const std::uint32_t depth = std::max(1u, src->second.desc.extent.depth >> sub.mipLevel);
        if (static_cast<std::uint32_t>(region.imageOffset.x) + region.imageExtent.width > width ||
            static_cast<std::uint32_t>(region.imageOffset.y) + region.imageExtent.height > height ||
            static_cast<std::uint32_t>(region.imageOffset.z) + region.imageExtent.depth > depth)
            return invalidArgument("image-to-buffer extent is out of bounds");
        const std::uint32_t row = region.bufferRowLength ? region.bufferRowLength : region.imageExtent.width;
        const std::uint32_t rows = region.bufferImageHeight ? region.bufferImageHeight : region.imageExtent.height;
        const std::uint32_t slices = src->second.desc.extent.depth > 1 ? region.imageExtent.depth : sub.arrayLayerCount;
        if (!rangeFits(region.bufferOffset, static_cast<std::uint64_t>(row) * rows * slices * src->second.format.bytes, dst->second.desc.size)) return invalidArgument("image-to-buffer destination range is out of bounds");
        transition(src->second, sub.mipLevel, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{}; copy.bufferOffset = region.bufferOffset; copy.bufferRowLength = region.bufferRowLength; copy.bufferImageHeight = region.bufferImageHeight;
        copy.imageSubresource = {src->second.format.aspect, sub.mipLevel, sub.baseArrayLayer, sub.arrayLayerCount};
        copy.imageOffset = {region.imageOffset.x, region.imageOffset.y, region.imageOffset.z};
        copy.imageExtent = {region.imageExtent.width, region.imageExtent.height, region.imageExtent.depth}; copies.push_back(copy);
    }
    vkCmdCopyImageToBuffer(commands(), src->second.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->second.buffer,
                           static_cast<std::uint32_t>(copies.size()), copies.data());
    if (dst->second.desc.memory == MemoryClass::Readback) dst->second.readySerial = mSubmittedSerial + 1;
    return Status::success();
}

Status VulkanDevice::generateMipmaps(ImageHandle image, const ImageSubresourceRange& range)
{
    auto found = mImages.find(handleKey(image)); if (!mImagePool.isLive(image) || found == mImages.end()) return invalidHandle("invalid image handle");
    if (!hasUsage(found->second.desc.usage, ResourceUsage::TransferSource) || !hasUsage(found->second.desc.usage, ResourceUsage::TransferDestination)) return invalidArgument("mipmap generation requires transfer usage");
    if (translateAspect(range.aspect) != VK_IMAGE_ASPECT_COLOR_BIT || !found->second.format.filterable || !range.mipLevelCount ||
        range.baseMipLevel + range.mipLevelCount > found->second.desc.mipLevels || range.baseArrayLayer != 0 ||
        range.arrayLayerCount != found->second.desc.arrayLayers) return unsupported("Vulkan mip generation requires a filterable complete-layer color range");
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, found->second.format.format, &formatProperties);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                          VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((formatProperties.optimalTilingFeatures & required) != required)
        return unsupported("Vulkan format does not support linear mip blits");
    const std::uint32_t endMip = static_cast<std::uint32_t>(range.baseMipLevel) + range.mipLevelCount;
    for (std::uint32_t mip = static_cast<std::uint32_t>(range.baseMipLevel) + 1; mip < endMip; ++mip)
    {
        transition(found->second, mip - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(found->second, mip, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, found->second.desc.arrayLayers};
        blit.srcOffsets[1] = {static_cast<std::int32_t>(std::max(1u, found->second.desc.extent.width >> (mip - 1))),
                              static_cast<std::int32_t>(std::max(1u, found->second.desc.extent.height >> (mip - 1))),
                              static_cast<std::int32_t>(std::max(1u, found->second.desc.extent.depth >> (mip - 1)))};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, found->second.desc.arrayLayers};
        blit.dstOffsets[1] = {static_cast<std::int32_t>(std::max(1u, found->second.desc.extent.width >> mip)),
                              static_cast<std::int32_t>(std::max(1u, found->second.desc.extent.height >> mip)),
                              static_cast<std::int32_t>(std::max(1u, found->second.desc.extent.depth >> mip))};
        vkCmdBlitImage(commands(), found->second.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            found->second.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    return Status::success();
}

Status VulkanDevice::resetQueryPool(QueryPoolHandle pool, std::uint32_t first, std::uint32_t count)
{
    auto found = mQueries.find(handleKey(pool)); if (!mQueryPool.isLive(pool) || found == mQueries.end()) return invalidHandle("invalid query-pool handle");
    if (!count || first >= found->second.desc.count || count > found->second.desc.count - first) return invalidArgument("query reset range is empty or out of bounds");
    vkCmdResetQueryPool(commands(), found->second.pool, first, count);
    std::fill(found->second.written.begin() + first, found->second.written.begin() + first + count, false); return Status::success();
}

Status VulkanDevice::writeTimestamp(QueryPoolHandle pool, std::uint32_t query)
{
    auto found = mQueries.find(handleKey(pool)); if (!mQueryPool.isLive(pool) || found == mQueries.end()) return invalidHandle("invalid query-pool handle");
    if (found->second.desc.type != QueryType::Timestamp || query >= found->second.desc.count)
        return invalidArgument("timestamp query index or pool type is invalid");
    if (found->second.written[query]) return invalidState("timestamp query must be reset before reuse");
    vkCmdWriteTimestamp(commands(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, found->second.pool, query);
    found->second.written[query] = true; return Status::success();
}

Status VulkanDevice::beginQuery(QueryPoolHandle pool, std::uint32_t query)
{
    if (!mCommands.renderingActive())
        return invalidState("beginQuery requires a rendering scope");
    if (mCommands.mActiveQueryPool)
        return invalidState("occlusion queries may not overlap");
    auto found = mQueries.find(handleKey(pool));
    if (!mQueryPool.isLive(pool) || found == mQueries.end())
        return invalidHandle("invalid query-pool handle");
    if (found->second.desc.type != QueryType::Occlusion ||
        query >= found->second.desc.count)
        return invalidArgument("occlusion query index or pool type is invalid");
    if (found->second.written[query])
        return invalidState("occlusion query must be reset before reuse");
    vkCmdBeginQuery(commands(), found->second.pool, query, 0);
    mCommands.mActiveQueryPool = pool;
    mCommands.mActiveQuery = query;
    return Status::success();
}

Status VulkanDevice::endQuery(QueryPoolHandle pool, std::uint32_t query)
{
    if (!mCommands.renderingActive())
        return invalidState("endQuery requires a rendering scope");
    if (!mCommands.mActiveQueryPool || mCommands.mActiveQueryPool != pool ||
        mCommands.mActiveQuery != query)
        return invalidState("endQuery does not match the active occlusion query");
    auto found = mQueries.find(handleKey(pool));
    if (!mQueryPool.isLive(pool) || found == mQueries.end())
        return invalidHandle("invalid query-pool handle");
    vkCmdEndQuery(commands(), found->second.pool, query);
    found->second.written[query] = true;
    mCommands.mActiveQueryPool = {};
    mCommands.mActiveQuery = 0;
    return Status::success();
}

Status VulkanDevice::beginRendering(const RenderingInfo& info)
{
    if (!mCommands.frameActive()) return invalidState("beginRendering requires an active frame");
    if (mCommands.renderingActive()) return invalidState("a rendering scope is already active");
    if (!info.width || !info.height ||
        (info.colors.empty() && !info.depthStencil.has_value()) ||
        info.colors.size() > mCapabilities.maxColorAttachments)
        return invalidArgument("invalid Vulkan rendering scope");

    VkMemoryBarrier transferBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    transferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    transferBarrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
        VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commands(), VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 1, &transferBarrier, 0, nullptr, 0, nullptr);
    // Resource bindings occur inside the rendering scope, so sampled/storage
    // images must reach their shader layouts before dynamic rendering begins.
    for (auto& [unused, image] : mImages)
    {
        if (hasUsage(image.desc.usage, ResourceUsage::Storage))
        {
            for (std::uint32_t mip = 0; mip < image.desc.mipLevels; ++mip)
                transition(image, mip, VK_IMAGE_LAYOUT_GENERAL);
        }
        else if (hasUsage(image.desc.usage, ResourceUsage::Sampled))
        {
            for (std::uint32_t mip = 0; mip < image.desc.mipLevels; ++mip)
                transition(image, mip, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    std::vector<VkRenderingAttachmentInfo> colors;
    colors.reserve(info.colors.size());
    for (const auto& attachment : info.colors)
    {
        auto view = mViews.find(handleKey(attachment.view));
        if (!mViewPool.isLive(attachment.view) || view == mViews.end())
            return invalidHandle("rendering references an invalid color view");
        auto image = mImages.find(handleKey(view->second.desc.image));
        if (image == mImages.end() || attachment.format != image->second.desc.format ||
            !hasUsage(image->second.desc.usage, ResourceUsage::ColorAttachment) ||
            image->second.desc.extent.width < info.width || image->second.desc.extent.height < info.height)
            return invalidArgument("Vulkan color rendering attachment is incompatible");
        Status status = transition(image->second, view->second.desc.subresources.baseMipLevel,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (!status) return status;
        VkRenderingAttachmentInfo native{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        native.imageView = view->second.view;
        native.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        native.loadOp = translateLoadOp(attachment.load);
        native.storeOp = translateStoreOp(attachment.store);
        std::copy(attachment.clear.color.begin(), attachment.clear.color.end(),
                  native.clearValue.color.float32);
        colors.push_back(native);
    }

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool hasStencil = false;
    if (info.depthStencil)
    {
        const auto& attachment = *info.depthStencil;
        auto view = mViews.find(handleKey(attachment.view));
        if (!mViewPool.isLive(attachment.view) || view == mViews.end())
            return invalidHandle("rendering references an invalid depth/stencil view");
        auto image = mImages.find(handleKey(view->second.desc.image));
        if (image == mImages.end() || attachment.format != image->second.desc.format ||
            !hasUsage(image->second.desc.usage, ResourceUsage::DepthStencilAttachment) ||
            image->second.desc.extent.width < info.width || image->second.desc.extent.height < info.height)
            return invalidArgument("Vulkan depth/stencil rendering attachment is incompatible");
        Status status = transition(image->second, view->second.desc.subresources.baseMipLevel,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        if (!status) return status;
        depth.imageView = view->second.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = translateLoadOp(attachment.load);
        depth.storeOp = translateStoreOp(attachment.store);
        depth.clearValue.depthStencil = {attachment.clear.depth, attachment.clear.stencil};
        hasStencil = (image->second.format.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    }
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {info.width, info.height};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
    rendering.pColorAttachments = colors.empty() ? nullptr : colors.data();
    rendering.pDepthAttachment = info.depthStencil ? &depth : nullptr;
    rendering.pStencilAttachment = info.depthStencil && hasStencil ? &depth : nullptr;
    vkCmdBeginRendering(commands(), &rendering);
    mCommands.resetDrawState();
    mCommands.mRenderingActive = true;
    mCommands.mRenderWidth = info.width;
    mCommands.mRenderHeight = info.height;
    for (const auto& attachment : info.colors)
        mCommands.mRenderColorFormats.push_back(attachment.format);
    if (info.depthStencil) mCommands.mRenderDepthFormat = info.depthStencil->format;
    return Status::success();
}

Status VulkanDevice::endRendering()
{
    if (!mCommands.renderingActive()) return invalidState("no rendering scope is active");
    if (mCommands.mActiveQueryPool)
        return invalidState("endRendering has an active occlusion query");
    vkCmdEndRendering(commands());
    mCommands.mRenderingActive = false;
    mCommands.resetDrawState();
    return Status::success();
}

Status VulkanDevice::resourceBarrier(ResourceBarrier barrier)
{
    if (!mCommands.frameActive() || mCommands.renderingActive())
        return invalidState("resourceBarrier requires a frame outside a rendering scope");
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    if (barrier == ResourceBarrier::StorageWriteToRead)
    {
        memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commands(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &memory, 0, nullptr, 0, nullptr);
    }
    else if (barrier == ResourceBarrier::DepthAttachmentWriteToSampledRead)
    {
        memory.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands(),
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 1, &memory, 0, nullptr, 0, nullptr);
    }
    else if (barrier == ResourceBarrier::ColorAttachmentWriteToSampledRead)
    {
        memory.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 1, &memory, 0, nullptr, 0, nullptr);
    }
    else return invalidArgument("unknown Vulkan resource barrier");
    return Status::success();
}

Status VulkanDevice::bindPipeline(PipelineHandle handle)
{
    if (!mCommands.renderingActive()) return invalidState("bindPipeline requires a rendering scope");
    auto pipeline = mPipelines.find(handleKey(handle));
    if (!mPipelinePool.isLive(handle) || pipeline == mPipelines.end())
        return invalidHandle("invalid pipeline handle");
    if (pipeline->second.desc.colorFormats != mCommands.mRenderColorFormats ||
        pipeline->second.desc.depthStencilFormat != mCommands.mRenderDepthFormat)
        return invalidArgument("pipeline formats do not match the Vulkan rendering scope");
    vkCmdBindPipeline(commands(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->second.pipeline);
    mCommands.mPipeline = handle;
    return Status::success();
}

Status VulkanDevice::bindBindingSet(
    std::uint8_t group, BindingSetHandle handle, std::span<const std::uint32_t> dynamicOffsets)
{
    if (!mCommands.renderingActive() || !mCommands.mPipeline)
        return invalidState("bindBindingSet requires a bound pipeline");
    auto set = mBindingSets.find(handleKey(handle));
    if (!mBindingSetPool.isLive(handle) || set == mBindingSets.end())
        return invalidHandle("invalid binding-set handle");
    auto pipeline = mPipelines.find(handleKey(mCommands.mPipeline));
    auto shader = mShaders.find(handleKey(pipeline->second.desc.shader));
    if (set->second.desc.shader != pipeline->second.desc.shader || set->second.desc.group != group)
        return invalidArgument("binding set is incompatible with the Vulkan pipeline or group");
    std::size_t expectedDynamic = 0;
    for (const auto& binding : shader->second.desc.bindings)
        if (binding.group == group && binding.dynamicOffset) expectedDynamic += binding.arrayCount;
    if (expectedDynamic != dynamicOffsets.size())
        return invalidArgument("Vulkan dynamic offset count does not match reflection");
    std::vector<const ShaderPackageDesc::Binding*> dynamicBindings;
    for (const auto& binding : shader->second.desc.bindings)
        if (binding.group == group && binding.dynamicOffset) dynamicBindings.push_back(&binding);
    std::sort(dynamicBindings.begin(), dynamicBindings.end(), [](const auto* lhs, const auto* rhs)
    { return lhs->binding < rhs->binding; });
    std::size_t dynamicIndex = 0;
    for (const auto* binding : dynamicBindings)
    {
        const std::uint64_t alignment = binding->type == ShaderPackageDesc::BindingType::UniformBuffer
            ? mCapabilities.uniformBufferOffsetAlignment : mCapabilities.storageBufferOffsetAlignment;
        for (std::uint16_t element = 0; element < binding->arrayCount; ++element)
        {
            auto resource = std::find_if(set->second.desc.resources.begin(),
                set->second.desc.resources.end(), [&](const auto& value)
                { return value.binding == binding->binding && value.arrayElement == element; });
            if (resource == set->second.desc.resources.end())
                return invalidArgument("dynamic descriptor resource is missing");
            auto buffer = mBuffers.find(handleKey(resource->buffer));
            if (buffer == mBuffers.end()) return invalidHandle("dynamic descriptor buffer is stale");
            const std::uint64_t offset = resource->bufferOffset + dynamicOffsets[dynamicIndex++];
            const std::uint64_t range = resource->bufferRange ? resource->bufferRange :
                buffer->second.desc.size - resource->bufferOffset;
            if (offset % std::max<std::uint64_t>(1, alignment) != 0 ||
                !rangeFits(offset, range, buffer->second.desc.size))
                return invalidArgument("Vulkan dynamic descriptor offset is unaligned or out of range");
        }
    }
    for (const auto& resource : set->second.desc.resources)
    {
        if (!resource.imageView) continue;
        auto view = mViews.find(handleKey(resource.imageView));
        if (view == mViews.end()) return invalidHandle("binding set image view is stale");
        auto image = mImages.find(handleKey(view->second.desc.image));
        if (image == mImages.end()) return invalidHandle("binding set image is stale");
        const auto& range = view->second.desc.subresources;
        const VkImageLayout layout = resource.type == ShaderPackageDesc::BindingType::StorageImage
            ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for (std::uint16_t mip = 0; mip < range.mipLevelCount; ++mip)
        {
            Status status = transition(image->second, range.baseMipLevel + mip, layout);
            if (!status) return status;
        }
    }
    vkCmdBindDescriptorSets(commands(), VK_PIPELINE_BIND_POINT_GRAPHICS,
        shader->second.pipelineLayout, group, 1, &set->second.set,
        static_cast<std::uint32_t>(dynamicOffsets.size()), dynamicOffsets.data());
    mCommands.mBoundGroups.insert(group);
    return Status::success();
}

Status VulkanDevice::setViewport(const Viewport& viewport)
{
    if (!mCommands.renderingActive()) return invalidState("setViewport requires a rendering scope");
    if (viewport.x < 0.f || viewport.y < 0.f || viewport.width <= 0.f || viewport.height <= 0.f ||
        viewport.x + viewport.width > mCommands.mRenderWidth ||
        viewport.y + viewport.height > mCommands.mRenderHeight ||
        viewport.minDepth < 0.f || viewport.maxDepth > 1.f || viewport.minDepth > viewport.maxDepth)
        return invalidArgument("invalid Vulkan viewport");
    const VkViewport native{viewport.x, viewport.y, viewport.width, viewport.height,
                            viewport.minDepth, viewport.maxDepth};
    vkCmdSetViewport(commands(), 0, 1, &native);
    mCommands.mViewportSet = true;
    return Status::success();
}

Status VulkanDevice::setScissor(const ScissorRect& scissor)
{
    if (!mCommands.renderingActive()) return invalidState("setScissor requires a rendering scope");
    if (scissor.x < 0 || scissor.y < 0 || !scissor.width || !scissor.height ||
        static_cast<std::uint64_t>(scissor.x) + scissor.width > mCommands.mRenderWidth ||
        static_cast<std::uint64_t>(scissor.y) + scissor.height > mCommands.mRenderHeight)
        return invalidArgument("invalid Vulkan scissor rectangle");
    const VkRect2D native{{scissor.x, scissor.y}, {scissor.width, scissor.height}};
    vkCmdSetScissor(commands(), 0, 1, &native);
    mCommands.mScissorSet = true;
    return Status::success();
}

Status VulkanDevice::bindVertexBuffer(
    std::uint32_t slot, BufferHandle handle, std::uint64_t offset)
{
    if (!mCommands.renderingActive() || !mCommands.mPipeline)
        return invalidState("bindVertexBuffer requires a bound pipeline");
    auto buffer = mBuffers.find(handleKey(handle));
    if (!mBufferPool.isLive(handle) || buffer == mBuffers.end())
        return invalidHandle("invalid vertex-buffer handle");
    if (!hasUsage(buffer->second.desc.usage, ResourceUsage::Vertex) || offset >= buffer->second.desc.size)
        return invalidArgument("vertex-buffer usage or offset is invalid");
    const VkDeviceSize nativeOffset = offset;
    vkCmdBindVertexBuffers(commands(), slot, 1, &buffer->second.buffer, &nativeOffset);
    return Status::success();
}

Status VulkanDevice::bindIndexBuffer(BufferHandle handle, std::uint64_t offset, IndexType type)
{
    if (!mCommands.renderingActive() || !mCommands.mPipeline)
        return invalidState("bindIndexBuffer requires a bound pipeline");
    auto buffer = mBuffers.find(handleKey(handle));
    if (!mBufferPool.isLive(handle) || buffer == mBuffers.end())
        return invalidHandle("invalid index-buffer handle");
    const std::uint64_t alignment = type == IndexType::UInt16 ? 2 : 4;
    if (!hasUsage(buffer->second.desc.usage, ResourceUsage::Index) ||
        offset >= buffer->second.desc.size || offset % alignment != 0)
        return invalidArgument("index-buffer usage or offset is invalid");
    vkCmdBindIndexBuffer(commands(), buffer->second.buffer, offset,
        type == IndexType::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    mCommands.mIndexBuffer = handle;
    return Status::success();
}

Status VulkanDevice::draw(const DrawArguments& arguments)
{
    if (!mCommands.renderingActive() || !mCommands.mPipeline)
        return invalidState("draw requires a bound pipeline");
    if (!mCommands.mViewportSet || !mCommands.mScissorSet)
        return invalidState("draw requires explicit viewport and scissor state");
    if (!arguments.vertexCount || !arguments.instanceCount)
        return invalidArgument("draw counts must be nonzero");
    auto pipeline = mPipelines.find(handleKey(mCommands.mPipeline));
    auto shader = mShaders.find(handleKey(pipeline->second.desc.shader));
    for (const auto& binding : shader->second.desc.bindings)
        if (!mCommands.mBoundGroups.contains(binding.group))
            return invalidState("draw is missing a reflected binding group");
    vkCmdDraw(commands(), arguments.vertexCount, arguments.instanceCount,
              arguments.firstVertex, arguments.firstInstance);
    return Status::success();
}

Status VulkanDevice::drawIndexed(const DrawIndexedArguments& arguments)
{
    if (!mCommands.mIndexBuffer) return invalidState("drawIndexed requires an index buffer");
    if (!arguments.indexCount || !arguments.instanceCount)
        return invalidArgument("drawIndexed counts must be nonzero");
    if (!mCommands.renderingActive() || !mCommands.mPipeline ||
        !mCommands.mViewportSet || !mCommands.mScissorSet)
        return invalidState("drawIndexed requires pipeline, viewport, and scissor state");
    auto pipeline = mPipelines.find(handleKey(mCommands.mPipeline));
    auto shader = mShaders.find(handleKey(pipeline->second.desc.shader));
    for (const auto& binding : shader->second.desc.bindings)
        if (!mCommands.mBoundGroups.contains(binding.group))
            return invalidState("drawIndexed is missing a reflected binding group");
    vkCmdDrawIndexed(commands(), arguments.indexCount, arguments.instanceCount,
        arguments.firstIndex, arguments.vertexOffset, arguments.firstInstance);
    return Status::success();
}

void VulkanDevice::drainRetirements(bool force)
{
    auto end = std::remove_if(mRetirements.begin(), mRetirements.end(), [&](const Retirement& item)
    {
        if (!force && item.releaseAfter > mCompletedSerial) return false;
        switch (item.kind)
        {
        case RetireKind::Buffer: vkDestroyBuffer(mDevice, item.buffer, nullptr); vkFreeMemory(mDevice, item.memory, nullptr); break;
        case RetireKind::Image: vkDestroyImage(mDevice, item.image, nullptr); vkFreeMemory(mDevice, item.memory, nullptr); break;
        case RetireKind::ImageView: vkDestroyImageView(mDevice, item.view, nullptr); break;
        case RetireKind::Sampler: vkDestroySampler(mDevice, item.sampler, nullptr); break;
        case RetireKind::QueryPool: vkDestroyQueryPool(mDevice, item.queryPool, nullptr); break;
        case RetireKind::Shader:
            vkDestroyPipelineLayout(mDevice, item.pipelineLayout, nullptr);
            for (VkDescriptorSetLayout layout : item.setLayouts)
                vkDestroyDescriptorSetLayout(mDevice, layout, nullptr);
            for (VkShaderModule module : item.shaderModules)
                vkDestroyShaderModule(mDevice, module, nullptr);
            break;
        case RetireKind::BindingSet:
            vkDestroyDescriptorPool(mDevice, item.descriptorPool, nullptr);
            break;
        case RetireKind::Pipeline:
            vkDestroyPipeline(mDevice, item.pipeline, nullptr);
            break;
        }
        return true;
    });
    mRetirements.erase(end, mRetirements.end());
}

Status VulkanDevice::waitIdle()
{
    if (!mDevice) return Status::success();
    if (mCommands.frameActive()) return invalidState("waitIdle is not allowed during an active frame");
    VkResult result = vkDeviceWaitIdle(mDevice); if (result != VK_SUCCESS) return vkFailure("vkDeviceWaitIdle", result);
    mCompletedSerial = mSubmittedSerial; drainRetirements(true);
    if (mValidationError.load())
        return Status::failure(StatusCode::BackendError,
            "Khronos validation reported a Vulkan error");
    return Status::success();
}

void VulkanDevice::shutdown()
{
    if (!mInstance) return;
    if (mDevice)
    {
        if (mCommands.frameActive()) mCommands.setFrameActive(false);
        vkDeviceWaitIdle(mDevice); drainRetirements(true);
        for (const auto& [unused, pipeline] : mPipelines)
            vkDestroyPipeline(mDevice, pipeline.pipeline, nullptr);
        for (const auto& [unused, set] : mBindingSets)
            vkDestroyDescriptorPool(mDevice, set.pool, nullptr);
        for (const auto& [unused, shader] : mShaders)
        {
            vkDestroyPipelineLayout(mDevice, shader.pipelineLayout, nullptr);
            for (VkDescriptorSetLayout layout : shader.setLayouts)
                vkDestroyDescriptorSetLayout(mDevice, layout, nullptr);
            for (const auto& stage : shader.stages)
                vkDestroyShaderModule(mDevice, stage.module, nullptr);
        }
        for (const auto& [unused, view] : mViews) vkDestroyImageView(mDevice, view.view, nullptr);
        for (const auto& [unused, sampler] : mSamplers) vkDestroySampler(mDevice, sampler.sampler, nullptr);
        for (const auto& [unused, query] : mQueries) vkDestroyQueryPool(mDevice, query.pool, nullptr);
        for (const auto& [unused, image] : mImages) { vkDestroyImage(mDevice, image.image, nullptr); vkFreeMemory(mDevice, image.memory, nullptr); }
        for (const auto& [unused, buffer] : mBuffers) { vkDestroyBuffer(mDevice, buffer.buffer, nullptr); vkFreeMemory(mDevice, buffer.memory, nullptr); }
        for (const Frame& frame : mFrames) if (frame.fence) vkDestroyFence(mDevice, frame.fence, nullptr);
        if (mCommandPool) vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
        vkDestroyDevice(mDevice, nullptr); mDevice = VK_NULL_HANDLE;
    }
    if (mDebugMessenger)
    {
        auto destroyDebugMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(mInstance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyDebugMessenger)
            destroyDebugMessenger(mInstance, mDebugMessenger, nullptr);
        mDebugMessenger = VK_NULL_HANDLE;
    }
    vkDestroyInstance(mInstance, nullptr); mInstance = VK_NULL_HANDLE;
}

Status VulkanCommandContext::requireTransfer() const
{
    if (!mFrameActive) return invalidState("transfer commands require an active frame");
    if (mRenderingActive) return invalidState("transfer commands are not allowed inside rendering");
    return Status::success();
}
void VulkanCommandContext::resetDrawState()
{
    mViewportSet = false;
    mScissorSet = false;
    mRenderWidth = 0;
    mRenderHeight = 0;
    mRenderColorFormats.clear();
    mRenderDepthFormat.reset();
    mPipeline = {};
    mBoundGroups.clear();
    mIndexBuffer = {};
    mActiveQueryPool = {};
    mActiveQuery = 0;
}
Status VulkanCommandContext::beginFrame() { return mDevice.beginFrame(); }
Status VulkanCommandContext::endFrame() { return mDevice.endFrame(); }
Status VulkanCommandContext::copyBuffer(BufferHandle a, BufferHandle b, std::span<const BufferCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyBuffer(a,b,r) : s; }
Status VulkanCommandContext::copyBufferToImage(BufferHandle a, ImageHandle b, std::span<const BufferImageCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyBufferToImage(a,b,r) : s; }
Status VulkanCommandContext::copyImageToBuffer(ImageHandle a, BufferHandle b, std::span<const BufferImageCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyImageToBuffer(a,b,r) : s; }
Status VulkanCommandContext::generateMipmaps(ImageHandle a, const ImageSubresourceRange& r) { Status s=requireTransfer(); return s ? mDevice.generateMipmaps(a,r) : s; }
Status VulkanCommandContext::resetQueryPool(QueryPoolHandle a, std::uint32_t b, std::uint32_t c) { Status s=requireTransfer(); return s ? mDevice.resetQueryPool(a,b,c) : s; }
Status VulkanCommandContext::writeTimestamp(QueryPoolHandle a, std::uint32_t b) { Status s=requireTransfer(); return s ? mDevice.writeTimestamp(a,b) : s; }
Status VulkanCommandContext::beginQuery(QueryPoolHandle a, std::uint32_t b) { return mDevice.beginQuery(a,b); }
Status VulkanCommandContext::endQuery(QueryPoolHandle a, std::uint32_t b) { return mDevice.endQuery(a,b); }
Status VulkanCommandContext::beginRendering(const RenderingInfo& a) { return mDevice.beginRendering(a); }
Status VulkanCommandContext::endRendering() { return mDevice.endRendering(); }
Status VulkanCommandContext::resourceBarrier(ResourceBarrier barrier)
{
    return mDevice.resourceBarrier(barrier);
}
Status VulkanCommandContext::bindPipeline(PipelineHandle a) { return mDevice.bindPipeline(a); }
Status VulkanCommandContext::bindBindingSet(std::uint8_t a, BindingSetHandle b, std::span<const std::uint32_t> c) { return mDevice.bindBindingSet(a,b,c); }
Status VulkanCommandContext::setViewport(const Viewport& a) { return mDevice.setViewport(a); }
Status VulkanCommandContext::setScissor(const ScissorRect& a) { return mDevice.setScissor(a); }
Status VulkanCommandContext::bindVertexBuffer(std::uint32_t a, BufferHandle b, std::uint64_t c) { return mDevice.bindVertexBuffer(a,b,c); }
Status VulkanCommandContext::bindIndexBuffer(BufferHandle a, std::uint64_t b, IndexType c) { return mDevice.bindIndexBuffer(a,b,c); }
Status VulkanCommandContext::draw(const DrawArguments& a) { return mDevice.draw(a); }
Status VulkanCommandContext::drawIndexed(const DrawIndexedArguments& a) { return mDevice.drawIndexed(a); }

} // namespace

DeviceCreationResult createVulkanDevice(const DeviceCreateInfo& info)
{
    if (info.backend != Backend::Vulkan) return {nullptr, invalidArgument("Vulkan factory received a different backend")};
    auto device = std::make_unique<VulkanDevice>(info);
    Status status = device->initialize(info);
    if (!status) return {nullptr, status};
    return {std::move(device), Status::success()};
}

} // namespace LL::GHI
