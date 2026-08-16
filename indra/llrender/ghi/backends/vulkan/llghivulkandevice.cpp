/**
 * @file llghivulkandevice.cpp
 * @brief Vulkan 1.3 implementation of the R2 GHI resource contract.
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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
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
    Status beginRendering(const RenderingInfo&) override { return unsupported("Vulkan rendering begins in R3"); }
    Status endRendering() override { return unsupported("Vulkan rendering begins in R3"); }
    Status bindPipeline(PipelineHandle) override { return unsupported("Vulkan pipelines begin in R3"); }
    Status bindBindingSet(std::uint8_t, BindingSetHandle, std::span<const std::uint32_t>) override { return unsupported("Vulkan binding sets begin in R3"); }
    Status setViewport(const Viewport&) override { return unsupported("Vulkan dynamic state begins in R3"); }
    Status setScissor(const ScissorRect&) override { return unsupported("Vulkan dynamic state begins in R3"); }
    Status bindVertexBuffer(std::uint32_t, BufferHandle, std::uint64_t) override { return unsupported("Vulkan vertex binding begins in R3"); }
    Status bindIndexBuffer(BufferHandle, std::uint64_t, IndexType) override { return unsupported("Vulkan index binding begins in R3"); }
    Status draw(const DrawArguments&) override { return unsupported("Vulkan drawing begins in R3"); }
    Status drawIndexed(const DrawIndexedArguments&) override { return unsupported("Vulkan drawing begins in R3"); }
    bool frameActive() const { return mFrameActive; }
    void setFrameActive(bool value) { mFrameActive = value; }
private:
    Status requireTransfer() const;
    VulkanDevice& mDevice;
    bool mFrameActive = false;
};

class VulkanDevice final : public Device
{
public:
    explicit VulkanDevice(const DeviceCreateInfo& info) : mFramesInFlight(info.framesInFlight), mCommands(*this) {}
    ~VulkanDevice() override { shutdown(); }
    Status initialize(const DeviceCreateInfo&);

    Backend backend() const override { return Backend::Vulkan; }
    const RendererCapabilities& capabilities() const override { return mCapabilities; }
    CommandContext& commandContext() override { return mCommands; }
    BufferHandle createBuffer(const BufferDesc&, Status&) override;
    ImageHandle createImage(const ImageDesc&, Status&) override;
    ImageViewHandle createImageView(const ImageViewDesc&, Status&) override;
    SamplerHandle createSampler(const SamplerDesc&, Status&) override;
    QueryPoolHandle createQueryPool(const QueryPoolDesc&, Status&) override;
    ShaderPackageHandle createShaderPackage(const ShaderPackageDesc&, Status& status) override { status = unsupported("Vulkan shader packages begin in R3"); return {}; }
    BindingSetHandle createBindingSet(const BindingSetDesc&, Status& status) override { status = unsupported("Vulkan binding sets begin in R3"); return {}; }
    PipelineHandle createPipeline(const PipelineDesc&, Status& status) override { status = unsupported("Vulkan pipelines begin in R3"); return {}; }
    Status destroy(BufferHandle) override;
    Status destroy(ImageHandle) override;
    Status destroy(ImageViewHandle) override;
    Status destroy(SamplerHandle) override;
    Status destroy(QueryPoolHandle) override;
    Status destroy(ShaderPackageHandle) override { return unsupported("Vulkan shader packages begin in R3"); }
    Status destroy(BindingSetHandle) override { return unsupported("Vulkan binding sets begin in R3"); }
    Status destroy(PipelineHandle) override { return unsupported("Vulkan pipelines begin in R3"); }
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

private:
    struct BufferRecord { BufferDesc desc; VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; std::uint64_t readySerial = 0; };
    struct ImageRecord { ImageDesc desc; VulkanFormat format; VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; std::vector<VkImageLayout> layouts; };
    struct ViewRecord { ImageViewDesc desc; VkImageView view = VK_NULL_HANDLE; };
    struct SamplerRecord { SamplerDesc desc; VkSampler sampler = VK_NULL_HANDLE; };
    struct QueryRecord { QueryPoolDesc desc; VkQueryPool pool = VK_NULL_HANDLE; std::vector<bool> written; };
    struct Frame { VkCommandBuffer commands = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; std::uint64_t serial = 0; };
    enum class RetireKind { Buffer, Image, ImageView, Sampler, QueryPool };
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
    };

    Status canMutate() const;
    std::optional<std::uint32_t> memoryType(std::uint32_t bits, VkMemoryPropertyFlags required) const;
    void pollFrames();
    void drainRetirements(bool force);
    void shutdown();
    Status transition(ImageRecord&, std::uint32_t mip, VkImageLayout layout);
    VkCommandBuffer commands() const { return mFrames[mFrameIndex].commands; }

    VkInstance mInstance = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mQueue = VK_NULL_HANDLE;
    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mMemoryProperties{};
    bool mSamplerAnisotropy = false;
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
    std::unordered_map<std::uint64_t, BufferRecord> mBuffers;
    std::unordered_map<std::uint64_t, ImageRecord> mImages;
    std::unordered_map<std::uint64_t, ViewRecord> mViews;
    std::unordered_map<std::uint64_t, SamplerRecord> mSamplers;
    std::unordered_map<std::uint64_t, QueryRecord> mQueries;
    std::vector<Frame> mFrames;
    std::vector<Retirement> mRetirements;
    VulkanCommandContext mCommands;
};

Status VulkanDevice::initialize(const DeviceCreateInfo& info)
{
    constexpr const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    if (info.enableValidation && !hasLayer(validationLayer))
        return unsupported("Vulkan validation was requested but VK_LAYER_KHRONOS_validation is unavailable");
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS || loaderVersion < VK_API_VERSION_1_3)
        return unsupported("Vulkan R2 requires a Vulkan 1.3 loader");

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Vulkanstorm R2 resources";
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = "Vulkanstorm GHI";
    application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    application.apiVersion = VK_API_VERSION_1_3;
    const char* layers[] = {validationLayer};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledLayerCount = info.enableValidation ? 1u : 0u;
    instanceInfo.ppEnabledLayerNames = info.enableValidation ? layers : nullptr;
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &mInstance);
    if (result != VK_SUCCESS) return vkFailure("vkCreateInstance", result);

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
    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.samplerAnisotropy = availableFeatures.samplerAnisotropy;
    enabledFeatures.depthClamp = availableFeatures.depthClamp;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
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
    mCapabilities.maxBufferSize = std::numeric_limits<VkDeviceSize>::max();
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, families.data());
    mCapabilities.timestampQueries = families[mQueueFamily].timestampValidBits != 0;
    mCapabilities.timestampPeriodNanoseconds = properties.limits.timestampPeriod;
    mCapabilities.occlusionQueries = true;
    mCapabilities.depthClamp = enabledFeatures.depthClamp == VK_TRUE;
    mCapabilities.baselineGraphicsPipeline = true;
    mCapabilities.advancedGraphicsPipeline = false;
    mSamplerAnisotropy = enabledFeatures.samplerAnisotropy == VK_TRUE;
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
        (desc.samples > 1 && desc.mipLevels != 1) || (desc.extent.depth > 1 && desc.arrayLayers > 1))
    { status = invalidArgument("invalid Vulkan image descriptor"); return {}; }
    ImageRecord record; record.desc = desc; record.format = format; record.layouts.resize(desc.mipLevels, VK_IMAGE_LAYOUT_UNDEFINED);
    const VkImageType imageType = desc.extent.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    VkImageFormatProperties imageProperties{};
    if (vkGetPhysicalDeviceImageFormatProperties(mPhysicalDevice, format.format, imageType,
            VK_IMAGE_TILING_OPTIMAL, usage, 0, &imageProperties) != VK_SUCCESS)
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
        range.arrayLayerCount > image->second.desc.arrayLayers - range.baseArrayLayer)
    { status = invalidArgument("invalid Vulkan image view descriptor"); return {}; }
    ViewRecord record; record.desc = desc;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image->second.image;
    info.viewType = image->second.desc.extent.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D :
                    image->second.desc.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
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
    if (!mCapabilities.timestampQueries) { status = unsupported("timestamp queries are unavailable"); return {}; }
    QueryRecord record; record.desc = desc; record.written.resize(desc.count, false);
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; info.queryType = VK_QUERY_TYPE_TIMESTAMP; info.queryCount = desc.count;
    VkResult result = vkCreateQueryPool(mDevice, &info, nullptr, &record.pool);
    if (result != VK_SUCCESS) { status = vkFailure("vkCreateQueryPool", result); return {}; }
    QueryPoolHandle handle = mQueryPool.allocate(); mQueries.emplace(handleKey(handle), std::move(record));
    status = Status::success(); return handle;
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
    mCommands.setFrameActive(true); return Status::success();
}

Status VulkanDevice::endFrame()
{
    if (!mCommands.frameActive()) return invalidState("no frame is active"); Frame& frame = mFrames[mFrameIndex];
    VkResult result = vkEndCommandBuffer(frame.commands); if (result != VK_SUCCESS) { mCommands.setFrameActive(false); return vkFailure("vkEndCommandBuffer", result); }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &frame.commands;
    result = vkQueueSubmit(mQueue, 1, &submit, frame.fence); mCommands.setFrameActive(false);
    if (result != VK_SUCCESS) return vkFailure("vkQueueSubmit", result);
    frame.serial = ++mSubmittedSerial; return Status::success();
}

Status VulkanDevice::transition(ImageRecord& image, std::uint32_t mip, VkImageLayout layout)
{
    if (image.layouts[mip] == layout) return Status::success();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = image.layouts[mip]; barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {image.format.aspect, mip, 1, 0, image.desc.arrayLayers};
    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (barrier.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) { barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT; }
    else if (barrier.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) { barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT; }
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier.dstAccessMask = layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
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
    vkCmdCopyBuffer(commands(), src->second.buffer, dst->second.buffer, static_cast<std::uint32_t>(copies.size()), copies.data());
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
    vkCmdCopyBufferToImage(commands(), src->second.buffer, dst->second.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(copies.size()), copies.data()); return Status::success();
}

Status VulkanDevice::copyImageToBuffer(ImageHandle source, BufferHandle destination, std::span<const BufferImageCopyRegion> regions)
{
    auto src = mImages.find(handleKey(source)); auto dst = mBuffers.find(handleKey(destination));
    if (!mImagePool.isLive(source) || !mBufferPool.isLive(destination) || src == mImages.end() || dst == mBuffers.end()) return invalidHandle("image-to-buffer copy received an invalid resource");
    if (!hasUsage(src->second.desc.usage, ResourceUsage::TransferSource) || !hasUsage(dst->second.desc.usage, ResourceUsage::TransferDestination) || regions.empty()) return invalidArgument("image-to-buffer usage or regions are invalid");
    if (src->second.format.aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return unsupported("Vulkan R2 image-to-buffer copies currently support color images only");
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
    if (query >= found->second.desc.count) return invalidArgument("timestamp query is out of bounds");
    if (found->second.written[query]) return invalidState("timestamp query must be reset before reuse");
    vkCmdWriteTimestamp(commands(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, found->second.pool, query);
    found->second.written[query] = true; return Status::success();
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
    mCompletedSerial = mSubmittedSerial; drainRetirements(true); return Status::success();
}

void VulkanDevice::shutdown()
{
    if (!mInstance) return;
    if (mDevice)
    {
        if (mCommands.frameActive()) mCommands.setFrameActive(false);
        vkDeviceWaitIdle(mDevice); drainRetirements(true);
        for (const auto& [unused, view] : mViews) vkDestroyImageView(mDevice, view.view, nullptr);
        for (const auto& [unused, sampler] : mSamplers) vkDestroySampler(mDevice, sampler.sampler, nullptr);
        for (const auto& [unused, query] : mQueries) vkDestroyQueryPool(mDevice, query.pool, nullptr);
        for (const auto& [unused, image] : mImages) { vkDestroyImage(mDevice, image.image, nullptr); vkFreeMemory(mDevice, image.memory, nullptr); }
        for (const auto& [unused, buffer] : mBuffers) { vkDestroyBuffer(mDevice, buffer.buffer, nullptr); vkFreeMemory(mDevice, buffer.memory, nullptr); }
        for (const Frame& frame : mFrames) if (frame.fence) vkDestroyFence(mDevice, frame.fence, nullptr);
        if (mCommandPool) vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
        vkDestroyDevice(mDevice, nullptr); mDevice = VK_NULL_HANDLE;
    }
    vkDestroyInstance(mInstance, nullptr); mInstance = VK_NULL_HANDLE;
}

Status VulkanCommandContext::requireTransfer() const { return mFrameActive ? Status::success() : invalidState("transfer commands require an active frame"); }
Status VulkanCommandContext::beginFrame() { return mDevice.beginFrame(); }
Status VulkanCommandContext::endFrame() { return mDevice.endFrame(); }
Status VulkanCommandContext::copyBuffer(BufferHandle a, BufferHandle b, std::span<const BufferCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyBuffer(a,b,r) : s; }
Status VulkanCommandContext::copyBufferToImage(BufferHandle a, ImageHandle b, std::span<const BufferImageCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyBufferToImage(a,b,r) : s; }
Status VulkanCommandContext::copyImageToBuffer(ImageHandle a, BufferHandle b, std::span<const BufferImageCopyRegion> r) { Status s=requireTransfer(); return s ? mDevice.copyImageToBuffer(a,b,r) : s; }
Status VulkanCommandContext::generateMipmaps(ImageHandle a, const ImageSubresourceRange& r) { Status s=requireTransfer(); return s ? mDevice.generateMipmaps(a,r) : s; }
Status VulkanCommandContext::resetQueryPool(QueryPoolHandle a, std::uint32_t b, std::uint32_t c) { Status s=requireTransfer(); return s ? mDevice.resetQueryPool(a,b,c) : s; }
Status VulkanCommandContext::writeTimestamp(QueryPoolHandle a, std::uint32_t b) { Status s=requireTransfer(); return s ? mDevice.writeTimestamp(a,b) : s; }

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
