/**
 * @file llghivulkanpresentation.cpp
 * @brief Native Vulkan R1 instance/device/surface/swapchain lifecycle.
 */

#if !defined(_WIN32)
#error The R1 Vulkan presentation slice currently supports Windows only
#endif

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "llghivulkanpresentation.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace LL::GHI
{
namespace
{

Status vkFailure(const char* operation, VkResult result)
{
    std::ostringstream message;
    message << operation << " failed with VkResult " << static_cast<int>(result);
    const StatusCode code = result == VK_ERROR_DEVICE_LOST
        ? StatusCode::DeviceLost
        : StatusCode::BackendError;
    return Status::failure(code, message.str());
}

std::string hexBytes(const std::uint8_t* bytes, std::size_t count)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < count; ++i)
    {
        output << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return output.str();
}

std::string numericVersion(std::uint32_t version)
{
    std::ostringstream output;
    output << VK_VERSION_MAJOR(version) << '.'
           << VK_VERSION_MINOR(version) << '.'
           << VK_VERSION_PATCH(version);
    return output.str();
}

DeviceVendor deviceVendor(std::uint32_t vendor)
{
    switch (vendor)
    {
        case 0x1002: return DeviceVendor::AMD;
        case 0x10de: return DeviceVendor::NVIDIA;
        case 0x8086: return DeviceVendor::Intel;
        case 0x106b: return DeviceVendor::Apple;
        default: return DeviceVendor::Unknown;
    }
}

std::uint32_t sampleCount(VkSampleCountFlags flags)
{
    if (flags & VK_SAMPLE_COUNT_64_BIT) return 64;
    if (flags & VK_SAMPLE_COUNT_32_BIT) return 32;
    if (flags & VK_SAMPLE_COUNT_16_BIT) return 16;
    if (flags & VK_SAMPLE_COUNT_8_BIT) return 8;
    if (flags & VK_SAMPLE_COUNT_4_BIT) return 4;
    if (flags & VK_SAMPLE_COUNT_2_BIT) return 2;
    return 1;
}

bool hasLayer(const char* name)
{
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS)
    {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
    {
        return false;
    }
    return std::any_of(layers.begin(), layers.end(), [name](const auto& layer)
    {
        return std::strcmp(layer.layerName, name) == 0;
    });
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* name)
{
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
    {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(
            device, nullptr, &count, extensions.data()) != VK_SUCCESS)
    {
        return false;
    }
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension)
    {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

class VulkanPresentationSurface final : public PresentationSurface
{
public:
    ~VulkanPresentationSurface() override { shutdown(); }

    Status initialize(const PresentationCreateInfo& info);
    Backend backend() const override { return Backend::Vulkan; }
    const RendererSnapshot& rendererSnapshot() const override { return mSnapshot; }
    Status presentClear(const ClearColor& color) override;
    Status resize(std::uint32_t width, std::uint32_t height) override;
    Status displayChanged() override;
    Status setSuspended(bool suspended) override;
    Status shutdown() override;

private:
    struct Frame
    {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence completion = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
    };

    Status createInstance(const PresentationCreateInfo& info);
    Status selectPhysicalDevice(std::uint32_t adapterIndex);
    Status createDevice(std::uint32_t frameCount);
    Status createSwapchain();
    void destroySwapchain();
    void captureSnapshot();

    VkInstance mInstance = VK_NULL_HANDLE;
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mGraphicsQueue = VK_NULL_HANDLE;
    VkQueue mPresentQueue = VK_NULL_HANDLE;
    std::uint32_t mGraphicsFamily = 0;
    std::uint32_t mPresentFamily = 0;
    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
    VkFormat mSwapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D mExtent{};
    std::vector<VkImage> mImages;
    std::vector<VkImageView> mImageViews;
    std::vector<VkSemaphore> mRenderComplete;
    std::vector<bool> mImageInitialized;
    std::vector<Frame> mFrames;
    RendererSnapshot mSnapshot;
    std::uint32_t mRequestedWidth = 0;
    std::uint32_t mRequestedHeight = 0;
    std::uint32_t mFrameIndex = 0;
    bool mVsync = true;
    bool mSuspended = false;
    bool mSwapchainDirty = false;
    bool mSnapshotPublished = false;
};

Status VulkanPresentationSurface::createInstance(const PresentationCreateInfo& info)
{
    constexpr const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    if (info.enableValidation && !hasLayer(validationLayer))
    {
        return Status::failure(
            StatusCode::Unsupported,
            "Vulkan validation was requested but VK_LAYER_KHRONOS_validation is unavailable");
    }

    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS)
    {
        loaderVersion = VK_API_VERSION_1_0;
    }
    if (loaderVersion < VK_API_VERSION_1_3)
    {
        return Status::failure(
            StatusCode::Unsupported,
            "Native Vulkan R1 requires a Vulkan 1.3 loader");
    }

    const VkApplicationInfo application{
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        nullptr,
        "Vulkanstorm R1",
        VK_MAKE_VERSION(1, 0, 0),
        "Vulkanstorm GHI",
        VK_MAKE_VERSION(1, 0, 0),
        VK_API_VERSION_1_3
    };
    const std::array<const char*, 2> extensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };
    const char* layers[] = { validationLayer };
    const VkInstanceCreateInfo createInfo{
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        nullptr,
        0,
        &application,
        info.enableValidation ? 1u : 0u,
        info.enableValidation ? layers : nullptr,
        static_cast<std::uint32_t>(extensions.size()),
        extensions.data()
    };
    const VkResult result = vkCreateInstance(&createInfo, nullptr, &mInstance);
    return result == VK_SUCCESS ? Status::success() : vkFailure("vkCreateInstance", result);
}

Status VulkanPresentationSurface::selectPhysicalDevice(std::uint32_t adapterIndex)
{
    std::uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(mInstance, &count, nullptr);
    if (result != VK_SUCCESS)
    {
        return vkFailure("vkEnumeratePhysicalDevices", result);
    }
    if (count == 0)
    {
        return Status::failure(StatusCode::Unsupported, "No Vulkan physical devices were found");
    }

    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(mInstance, &count, devices.data());
    if (result != VK_SUCCESS)
    {
        return vkFailure("vkEnumeratePhysicalDevices", result);
    }

    std::vector<VkPhysicalDevice> suitable;
    for (VkPhysicalDevice device : devices)
    {
        if (!hasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        {
            continue;
        }

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

        std::optional<std::uint32_t> graphics;
        std::optional<std::uint32_t> present;
        for (std::uint32_t family = 0; family < familyCount; ++family)
        {
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                families[family].queueCount > 0)
            {
                graphics = family;
            }
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, family, mSurface, &supported);
            if (supported)
            {
                present = family;
            }
            if (graphics && present)
            {
                break;
            }
        }
        if (graphics && present)
        {
            suitable.push_back(device);
        }
    }

    if (adapterIndex >= suitable.size())
    {
        return Status::failure(
            StatusCode::Unsupported,
            "The requested Vulkan adapter does not support graphics and presentation");
    }
    mPhysicalDevice = suitable[adapterIndex];

    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, families.data());
    bool foundGraphics = false;
    bool foundPresent = false;
    for (std::uint32_t family = 0; family < familyCount; ++family)
    {
        if (!foundGraphics && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            mGraphicsFamily = family;
            foundGraphics = true;
        }
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(mPhysicalDevice, family, mSurface, &supported);
        if (!foundPresent && supported)
        {
            mPresentFamily = family;
            foundPresent = true;
        }
    }
    return Status::success();
}

Status VulkanPresentationSurface::createDevice(std::uint32_t frameCount)
{
    const float priority = 1.f;
    std::vector<VkDeviceQueueCreateInfo> queues;
    queues.push_back({
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0,
        mGraphicsFamily, 1, &priority
    });
    if (mPresentFamily != mGraphicsFamily)
    {
        queues.push_back({
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0,
            mPresentFamily, 1, &priority
        });
    }

    const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const VkDeviceCreateInfo createInfo{
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        nullptr,
        0,
        static_cast<std::uint32_t>(queues.size()),
        queues.data(),
        0,
        nullptr,
        1,
        extensions,
        nullptr
    };
    VkResult result = vkCreateDevice(mPhysicalDevice, &createInfo, nullptr, &mDevice);
    if (result != VK_SUCCESS)
    {
        return vkFailure("vkCreateDevice", result);
    }
    vkGetDeviceQueue(mDevice, mGraphicsFamily, 0, &mGraphicsQueue);
    vkGetDeviceQueue(mDevice, mPresentFamily, 0, &mPresentQueue);

    const VkCommandPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        mGraphicsFamily
    };
    result = vkCreateCommandPool(mDevice, &poolInfo, nullptr, &mCommandPool);
    if (result != VK_SUCCESS)
    {
        return vkFailure("vkCreateCommandPool", result);
    }

    mFrames.resize(std::clamp(frameCount, 1u, 3u));
    std::vector<VkCommandBuffer> commands(mFrames.size());
    const VkCommandBufferAllocateInfo allocation{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr,
        mCommandPool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        static_cast<std::uint32_t>(commands.size())
    };
    result = vkAllocateCommandBuffers(mDevice, &allocation, commands.data());
    if (result != VK_SUCCESS)
    {
        return vkFailure("vkAllocateCommandBuffers", result);
    }

    const VkFenceCreateInfo fenceInfo{
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT
    };
    const VkSemaphoreCreateInfo semaphoreInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0
    };
    for (std::size_t i = 0; i < mFrames.size(); ++i)
    {
        mFrames[i].commands = commands[i];
        result = vkCreateFence(mDevice, &fenceInfo, nullptr, &mFrames[i].completion);
        if (result != VK_SUCCESS) return vkFailure("vkCreateFence", result);
        result = vkCreateSemaphore(mDevice, &semaphoreInfo, nullptr, &mFrames[i].acquired);
        if (result != VK_SUCCESS) return vkFailure("vkCreateSemaphore", result);
    }
    return Status::success();
}

void VulkanPresentationSurface::destroySwapchain()
{
    for (VkImageView view : mImageViews)
    {
        vkDestroyImageView(mDevice, view, nullptr);
    }
    mImageViews.clear();
    for (VkSemaphore semaphore : mRenderComplete)
    {
        vkDestroySemaphore(mDevice, semaphore, nullptr);
    }
    mRenderComplete.clear();
    mImages.clear();
    mImageInitialized.clear();
    if (mSwapchain)
    {
        vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
        mSwapchain = VK_NULL_HANDLE;
    }
}

Status VulkanPresentationSurface::createSwapchain()
{
    if (mRequestedWidth == 0 || mRequestedHeight == 0)
    {
        mSuspended = true;
        return Status::success();
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        mPhysicalDevice, mSurface, &capabilities);
    if (result != VK_SUCCESS)
    {
        return vkFailure("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
    }
    constexpr VkImageUsageFlags requiredUsage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((capabilities.supportedUsageFlags & requiredUsage) != requiredUsage)
    {
        return Status::failure(
            StatusCode::Unsupported,
            "The Vulkan presentation surface lacks required transfer/color-attachment usage");
    }

    std::uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        mPhysicalDevice, mSurface, &formatCount, nullptr);
    if (result != VK_SUCCESS || formatCount == 0)
    {
        return result == VK_SUCCESS
            ? Status::failure(StatusCode::Unsupported, "The Vulkan surface reports no formats")
            : vkFailure("vkGetPhysicalDeviceSurfaceFormatsKHR", result);
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        mPhysicalDevice, mSurface, &formatCount, formats.data());
    VkSurfaceFormatKHR chosen = formats.front();
    for (const VkSurfaceFormatKHR& format : formats)
    {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = format;
            break;
        }
    }

    std::uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysicalDevice, mSurface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysicalDevice, mSurface, &modeCount, modes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!mVsync)
    {
        for (VkPresentModeKHR mode : modes)
        {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
            {
                presentMode = mode;
                break;
            }
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            {
                presentMode = mode;
            }
        }
    }

    VkExtent2D extent{};
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
    {
        extent = capabilities.currentExtent;
    }
    else
    {
        extent.width = std::clamp(
            mRequestedWidth,
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        extent.height = std::clamp(
            mRequestedHeight,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);
    }

    std::uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0)
    {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(capabilities.supportedCompositeAlpha & composite))
    {
        constexpr std::array<VkCompositeAlphaFlagBitsKHR, 3> alternatives{
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
        };
        for (auto candidate : alternatives)
        {
            if (capabilities.supportedCompositeAlpha & candidate)
            {
                composite = candidate;
                break;
            }
        }
    }

    const std::uint32_t families[] = { mGraphicsFamily, mPresentFamily };
    const bool concurrent = mGraphicsFamily != mPresentFamily;
    const VkSwapchainCreateInfoKHR createInfo{
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        nullptr,
        0,
        mSurface,
        imageCount,
        chosen.format,
        chosen.colorSpace,
        extent,
        1,
        requiredUsage,
        concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        concurrent ? 2u : 0u,
        concurrent ? families : nullptr,
        capabilities.currentTransform,
        composite,
        presentMode,
        VK_TRUE,
        mSwapchain
    };

    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    result = vkCreateSwapchainKHR(mDevice, &createInfo, nullptr, &replacement);
    if (result != VK_SUCCESS)
    {
        return vkFailure("vkCreateSwapchainKHR", result);
    }

    if (mSwapchain)
    {
        vkDeviceWaitIdle(mDevice);
        destroySwapchain();
    }
    mSwapchain = replacement;
    mSwapchainFormat = chosen.format;
    mExtent = extent;

    imageCount = 0;
    vkGetSwapchainImagesKHR(mDevice, mSwapchain, &imageCount, nullptr);
    mImages.resize(imageCount);
    result = vkGetSwapchainImagesKHR(mDevice, mSwapchain, &imageCount, mImages.data());
    if (result != VK_SUCCESS)
    {
        return vkFailure("vkGetSwapchainImagesKHR", result);
    }
    mImageViews.resize(imageCount);
    mRenderComplete.resize(imageCount, VK_NULL_HANDLE);
    mImageInitialized.assign(imageCount, false);
    const VkSemaphoreCreateInfo semaphoreInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0
    };
    for (std::uint32_t i = 0; i < imageCount; ++i)
    {
        const VkImageViewCreateInfo viewInfo{
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            nullptr,
            0,
            mImages[i],
            VK_IMAGE_VIEW_TYPE_2D,
            mSwapchainFormat,
            {},
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        result = vkCreateImageView(mDevice, &viewInfo, nullptr, &mImageViews[i]);
        if (result != VK_SUCCESS)
        {
            return vkFailure("vkCreateImageView", result);
        }
        result = vkCreateSemaphore(mDevice, &semaphoreInfo, nullptr, &mRenderComplete[i]);
        if (result != VK_SUCCESS)
        {
            return vkFailure("vkCreateSemaphore", result);
        }
    }
    mSwapchainDirty = false;
    mSuspended = false;
    return Status::success();
}

void VulkanPresentationSurface::captureSnapshot()
{
    VkPhysicalDeviceIDProperties id{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES
    };
    VkPhysicalDeviceDriverProperties driver{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
        &id
    };
    VkPhysicalDeviceProperties2 properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        &driver
    };
    vkGetPhysicalDeviceProperties2(mPhysicalDevice, &properties);

    VkPhysicalDeviceVulkan12Features features12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
    };
    VkPhysicalDeviceFeatures2 features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        &features12
    };
    vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &features);

    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(mPhysicalDevice, &memory);
    std::uint64_t localMemory = 0;
    for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i)
    {
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            localMemory += memory.memoryHeaps[i].size;
        }
    }

    RendererIdentity& identity = mSnapshot.identity;
    identity.backend = Backend::Vulkan;
    identity.provider = RendererProvider::NativeVulkan;
    identity.vendor = deviceVendor(properties.properties.vendorID);
    identity.apiName = "Vulkan";
    identity.apiVersion = {
        VK_VERSION_MAJOR(properties.properties.apiVersion),
        VK_VERSION_MINOR(properties.properties.apiVersion),
        VK_VERSION_PATCH(properties.properties.apiVersion),
        numericVersion(properties.properties.apiVersion)
    };
    identity.rendererName = properties.properties.deviceName;
    identity.deviceName = properties.properties.deviceName;
    identity.vendorName = vendorDisplayName(identity.vendor);
    identity.vendorId = properties.properties.vendorID;
    identity.deviceId = properties.properties.deviceID;
    identity.driverName = driver.driverName;
    identity.driverVersion = driver.driverInfo[0]
        ? driver.driverInfo
        : numericVersion(properties.properties.driverVersion);
    identity.dedicatedVideoMemoryBytes = localMemory;
    identity.detectedVideoMemoryBytes = localMemory;
    identity.videoMemoryBudgetBytes = localMemory;
    identity.softwareDevice = properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (id.deviceLUIDValid)
    {
        identity.stableDeviceId = "luid:" + hexBytes(id.deviceLUID, VK_LUID_SIZE);
    }
    else
    {
        identity.stableDeviceId = "uuid:" + hexBytes(id.deviceUUID, VK_UUID_SIZE);
    }

    const VkPhysicalDeviceLimits& limits = properties.properties.limits;
    RendererCapabilities& capabilities = mSnapshot.capabilities;
    capabilities.maxFramesInFlight = static_cast<std::uint32_t>(mFrames.size());
    capabilities.maxColorAttachments = limits.maxColorAttachments;
    capabilities.maxSampledImagesPerStage = limits.maxPerStageDescriptorSampledImages;
    capabilities.maxStorageBuffersPerStage = limits.maxPerStageDescriptorStorageBuffers;
    capabilities.maxTexture2DSize = limits.maxImageDimension2D;
    capabilities.maxUniformBufferSize = limits.maxUniformBufferRange;
    capabilities.maxVaryingVectors = limits.maxVertexOutputComponents / 4;
    capabilities.maxSamples = sampleCount(
        limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts);
    capabilities.maxBufferSize = std::max<std::uint64_t>(
        limits.maxStorageBufferRange, limits.maxUniformBufferRange);
    capabilities.timestampQueries = limits.timestampComputeAndGraphics != 0;
    capabilities.timestampPeriodNanoseconds = limits.timestampPeriod;
    capabilities.occlusionQueries = true;
    capabilities.descriptorIndexing = features12.descriptorIndexing != 0;
    capabilities.storageImageAtomics = features.features.fragmentStoresAndAtomics != 0;
    capabilities.depthClamp = features.features.depthClamp != 0;
    capabilities.independentBlend = features.features.independentBlend != 0;
    capabilities.cubeMapArrays = features.features.imageCubeArray != 0;
    // Device creation already enforces the Vulkan baseline required by this
    // presentation peer. These are viewer feature tiers, not API versions.
    capabilities.baselineGraphicsPipeline = true;
    capabilities.advancedGraphicsPipeline = true;
}

Status VulkanPresentationSurface::initialize(const PresentationCreateInfo& info)
{
    if (!info.nativeWindow || info.width == 0 || info.height == 0)
    {
        return Status::failure(
            StatusCode::InvalidArgument,
            "Vulkan presentation requires a native window and non-zero extent");
    }
    mRequestedWidth = info.width;
    mRequestedHeight = info.height;
    mVsync = info.enableVsync;

    Status status = createInstance(info);
    if (!status) return status;

    const VkWin32SurfaceCreateInfoKHR surfaceInfo{
        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        nullptr,
        0,
        static_cast<HINSTANCE>(info.nativeInstance
            ? info.nativeInstance
            : GetModuleHandle(nullptr)),
        static_cast<HWND>(info.nativeWindow)
    };
    VkResult result = vkCreateWin32SurfaceKHR(mInstance, &surfaceInfo, nullptr, &mSurface);
    if (result != VK_SUCCESS) return vkFailure("vkCreateWin32SurfaceKHR", result);

    status = selectPhysicalDevice(info.adapterIndex);
    if (!status) return status;
    status = createDevice(info.framesInFlight);
    if (!status) return status;
    status = createSwapchain();
    if (!status) return status;

    captureSnapshot();
    if (!mSnapshot.complete())
    {
        return Status::failure(
            StatusCode::BackendError,
            "Vulkan initialized but did not produce a complete renderer snapshot");
    }
    publishRendererSnapshot(mSnapshot);
    mSnapshotPublished = true;
    return Status::success();
}

Status VulkanPresentationSurface::presentClear(const ClearColor& color)
{
    if (!mDevice)
    {
        return Status::failure(StatusCode::InvalidState, "Vulkan presentation is not initialized");
    }
    if (mSuspended)
    {
        return Status::success();
    }
    if (mSwapchainDirty)
    {
        Status status = createSwapchain();
        if (!status || mSuspended) return status;
    }

    Frame& frame = mFrames[mFrameIndex];
    VkResult result = vkWaitForFences(
        mDevice, 1, &frame.completion, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
    if (result != VK_SUCCESS) return vkFailure("vkWaitForFences", result);

    std::uint32_t imageIndex = 0;
    result = vkAcquireNextImageKHR(
        mDevice,
        mSwapchain,
        std::numeric_limits<std::uint64_t>::max(),
        frame.acquired,
        VK_NULL_HANDLE,
        &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        mSwapchainDirty = true;
        return createSwapchain();
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        return vkFailure("vkAcquireNextImageKHR", result);
    }
    if (result == VK_SUBOPTIMAL_KHR) mSwapchainDirty = true;

    vkResetFences(mDevice, 1, &frame.completion);
    vkResetCommandBuffer(frame.commands, 0);
    const VkCommandBufferBeginInfo begin{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        nullptr
    };
    result = vkBeginCommandBuffer(frame.commands, &begin);
    if (result != VK_SUCCESS) return vkFailure("vkBeginCommandBuffer", result);

    VkImageMemoryBarrier toClear{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        nullptr,
        0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        mImageInitialized[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        mImages[imageIndex],
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    vkCmdPipelineBarrier(
        frame.commands,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toClear);

    const VkClearColorValue clear{ { color.red, color.green, color.blue, color.alpha } };
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(
        frame.commands,
        mImages[imageIndex],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &clear,
        1,
        &range);

    VkImageMemoryBarrier toPresent = toClear;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(
        frame.commands,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toPresent);
    result = vkEndCommandBuffer(frame.commands);
    if (result != VK_SUCCESS) return vkFailure("vkEndCommandBuffer", result);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit{
        VK_STRUCTURE_TYPE_SUBMIT_INFO,
        nullptr,
        1,
        &frame.acquired,
        &waitStage,
        1,
        &frame.commands,
        1,
        &mRenderComplete[imageIndex]
    };
    result = vkQueueSubmit(mGraphicsQueue, 1, &submit, frame.completion);
    if (result != VK_SUCCESS) return vkFailure("vkQueueSubmit", result);

    const VkPresentInfoKHR present{
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        nullptr,
        1,
        &mRenderComplete[imageIndex],
        1,
        &mSwapchain,
        &imageIndex,
        nullptr
    };
    result = vkQueuePresentKHR(mPresentQueue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        mSwapchainDirty = true;
    }
    else if (result != VK_SUCCESS)
    {
        return vkFailure("vkQueuePresentKHR", result);
    }
    mImageInitialized[imageIndex] = true;
    mFrameIndex = (mFrameIndex + 1) % static_cast<std::uint32_t>(mFrames.size());
    return Status::success();
}

Status VulkanPresentationSurface::resize(std::uint32_t width, std::uint32_t height)
{
    mRequestedWidth = width;
    mRequestedHeight = height;
    mSuspended = width == 0 || height == 0;
    mSwapchainDirty = !mSuspended;
    return Status::success();
}

Status VulkanPresentationSurface::displayChanged()
{
    if (!mDevice || !mSurface)
    {
        return Status::failure(StatusCode::InvalidState, "Vulkan presentation is shut down");
    }
    // Surface capabilities, format support, and present modes may change with
    // the desktop topology. Re-query all of them through swapchain recreation
    // on the next present without exposing a platform event above the backend.
    if (!mSuspended)
    {
        mSwapchainDirty = true;
    }
    return Status::success();
}

Status VulkanPresentationSurface::setSuspended(bool suspended)
{
    mSuspended = suspended;
    if (!suspended)
    {
        mSwapchainDirty = true;
    }
    return Status::success();
}

Status VulkanPresentationSurface::shutdown()
{
    if (mDevice)
    {
        vkDeviceWaitIdle(mDevice);
        destroySwapchain();
        for (Frame& frame : mFrames)
        {
            if (frame.acquired) vkDestroySemaphore(mDevice, frame.acquired, nullptr);
            if (frame.completion) vkDestroyFence(mDevice, frame.completion, nullptr);
        }
        mFrames.clear();
        if (mCommandPool) vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
        vkDestroyDevice(mDevice, nullptr);
        mDevice = VK_NULL_HANDLE;
    }
    if (mSurface)
    {
        vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
        mSurface = VK_NULL_HANDLE;
    }
    if (mInstance)
    {
        vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
    }
    if (mSnapshotPublished)
    {
        const auto active = activeRendererSnapshot();
        if (active && *active == mSnapshot)
        {
            clearRendererSnapshot();
        }
        mSnapshotPublished = false;
    }
    return Status::success();
}

} // namespace

PresentationCreationResult createVulkanPresentationSurface(
    const PresentationCreateInfo& info)
{
    auto surface = std::make_unique<VulkanPresentationSurface>();
    Status status = surface->initialize(info);
    if (!status)
    {
        surface->shutdown();
        return { nullptr, std::move(status) };
    }
    return { std::move(surface), Status::success() };
}

} // namespace LL::GHI
