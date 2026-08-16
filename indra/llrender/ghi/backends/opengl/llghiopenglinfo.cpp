/**
 * @file llghiopenglinfo.cpp
 * @brief OpenGL renderer identity and semantic capability adapter.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "llghiopenglinfo.h"

#include "llgl.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#if LL_WINDOWS
#include <dxgi.h>
#endif

namespace LL::GHI
{

namespace
{
constexpr std::uint64_t BYTES_PER_MEBIBYTE = 1024ull * 1024ull;

std::string uppercase(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}

void trim(std::string& value)
{
    const auto content = [](unsigned char character) { return !std::isspace(character); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), content));
    value.erase(std::find_if(value.rbegin(), value.rend(), content).base(), value.end());
}

bool endsWithCaseInsensitive(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
           uppercase(value.substr(value.size() - suffix.size())) == uppercase(suffix);
}

DeviceVendor detectVendor(const std::string& vendor, const std::string& device)
{
    const std::string combined = uppercase(vendor + ' ' + device);
    if (combined.find("AMD") != std::string::npos ||
        combined.find("RADEON") != std::string::npos ||
        combined.find("ATI ") != std::string::npos)
    {
        return DeviceVendor::AMD;
    }
    if (combined.find("NVIDIA") != std::string::npos)
    {
        return DeviceVendor::NVIDIA;
    }
    if (combined.find("INTEL") != std::string::npos)
    {
        return DeviceVendor::Intel;
    }
    if (combined.find("APPLE") != std::string::npos)
    {
        return DeviceVendor::Apple;
    }
    return DeviceVendor::Unknown;
}

struct ZinkIdentity
{
    bool active = false;
    std::string vulkanVersion;
    std::string deviceName;
    bool vendorIcd = false;
};

ZinkIdentity parseZinkRenderer(const std::string& renderer)
{
    static const std::string marker = "ZINK VULKAN ";
    const std::string normalized = uppercase(renderer);
    const std::size_t marker_at = normalized.find(marker);
    if (marker_at == std::string::npos)
    {
        return {};
    }

    const std::size_t version_at = marker_at + marker.size();
    const std::size_t device_open = renderer.find('(', version_at);
    const std::size_t device_close = renderer.rfind(')');
    if (device_open == std::string::npos || device_close == std::string::npos ||
        device_close <= device_open)
    {
        return {true, {}, renderer, false};
    }

    ZinkIdentity result;
    result.active = true;
    result.vulkanVersion = renderer.substr(version_at, device_open - version_at);
    trim(result.vulkanVersion);
    result.deviceName = renderer.substr(device_open + 1, device_close - device_open - 1);
    trim(result.deviceName);

    static const std::string unknown_driver = " (Driver Unknown)";
    if (endsWithCaseInsensitive(result.deviceName, unknown_driver))
    {
        result.deviceName.erase(result.deviceName.size() - unknown_driver.size());
        trim(result.deviceName);
        result.vendorIcd = true;
    }
    return result;
}

std::string stableNameIdentity(std::string device)
{
    trim(device);
    return uppercase(std::move(device));
}

#if LL_WINDOWS
struct AdapterIdentity
{
    std::string stableId;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedMemory = 0;
};

std::string wideToUtf8(const wchar_t* value)
{
    if (!value || !*value)
    {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr);
    result.pop_back();
    return result;
}

DeviceVendor vendorFromId(std::uint32_t vendor)
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

std::string luidIdentity(const LUID& luid)
{
    std::array<unsigned char, sizeof(LUID)> bytes{};
    std::memcpy(bytes.data(), &luid, bytes.size());
    std::ostringstream output;
    output << "luid:" << std::hex << std::setfill('0');
    for (unsigned char byte : bytes)
    {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

std::optional<AdapterIdentity> queryDxgiAdapter(
    const std::string& deviceName,
    DeviceVendor vendor)
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory))))
    {
        return std::nullopt;
    }

    const std::string wanted = stableNameIdentity(deviceName);
    int bestScore = 0;
    int vendorMatches = 0;
    std::optional<AdapterIdentity> best;
    for (UINT index = 0;; ++index)
    {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (!adapter)
        {
            continue;
        }

        DXGI_ADAPTER_DESC1 description{};
        const HRESULT result = adapter->GetDesc1(&description);
        adapter->Release();
        if (FAILED(result) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            continue;
        }

        const std::string candidate = stableNameIdentity(wideToUtf8(description.Description));
        const DeviceVendor candidateVendor = vendorFromId(description.VendorId);
        if (vendor != DeviceVendor::Unknown && candidateVendor == vendor)
        {
            ++vendorMatches;
        }
        int score = 0;
        if (!wanted.empty() && candidate == wanted)
        {
            score = 4;
        }
        else if (!wanted.empty() &&
                 (candidate.find(wanted) != std::string::npos ||
                  wanted.find(candidate) != std::string::npos))
        {
            score = 3;
        }
        else if (vendor != DeviceVendor::Unknown && candidateVendor == vendor)
        {
            score = 1;
        }
        if (score > bestScore)
        {
            bestScore = score;
            best = AdapterIdentity{
                luidIdentity(description.AdapterLuid),
                description.VendorId,
                description.DeviceId,
                static_cast<std::uint64_t>(description.DedicatedVideoMemory)
            };
        }
    }
    factory->Release();
    // A vendor-only match is useful on a single-adapter desktop but unsafe on
    // laptops and multi-GPU systems when more than one adapter shares a vendor.
    if (bestScore == 1 && vendorMatches != 1)
    {
        return std::nullopt;
    }
    return best;
}
#endif
}

RendererSnapshot queryOpenGLRendererSnapshot()
{
    RendererSnapshot snapshot;
    RendererIdentity& identity = snapshot.identity;
    RendererCapabilities& device_caps = snapshot.capabilities;

    identity.backend = Backend::OpenGL;
    identity.apiName = "OpenGL";

    const char* vendor_string = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer_string = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version_string = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const std::string raw_vendor = vendor_string ? vendor_string : gGLManager.mGLVendor;
    const std::string raw_renderer = renderer_string ? renderer_string : gGLManager.mGLRenderer;

    identity.apiVersion.major = static_cast<std::uint32_t>(
        std::max(gGLManager.mDriverVersionMajor, 0));
    identity.apiVersion.minor = static_cast<std::uint32_t>(
        std::max(gGLManager.mDriverVersionMinor, 0));
    identity.apiVersion.patch = static_cast<std::uint32_t>(
        std::max(gGLManager.mDriverVersionRelease, 0));
    identity.apiVersion.text = version_string ? version_string : gGLManager.mGLVersionString;

    const ZinkIdentity zink = parseZinkRenderer(raw_renderer);
    identity.provider = zink.active
        ? RendererProvider::MesaZink
        : RendererProvider::System;
    identity.deviceName = zink.active && !zink.deviceName.empty()
        ? zink.deviceName
        : raw_renderer;
    identity.rendererName = raw_renderer;
    if (zink.active && !zink.vulkanVersion.empty() && !zink.deviceName.empty())
    {
        identity.rendererName = "Mesa zink Vulkan " + zink.vulkanVersion +
            " (" + zink.deviceName + (zink.vendorIcd ? " - Vendor ICD)" : ")");
    }
    identity.vendor = detectVendor(raw_vendor, identity.deviceName);
    identity.vendorName = identity.vendor == DeviceVendor::Unknown
        ? raw_vendor
        : vendorDisplayName(identity.vendor);
    identity.driverName = providerDisplayName(identity.provider);
    identity.driverVersion = gGLManager.mDriverVersionVendorString;
    identity.dedicatedVideoMemoryBytes =
        static_cast<std::uint64_t>(gGLManager.mVRAM) * BYTES_PER_MEBIBYTE;
    identity.detectedVideoMemoryBytes = gGLManager.mVRAMDetected > 0
        ? static_cast<std::uint64_t>(gGLManager.mVRAMDetected) * BYTES_PER_MEBIBYTE
        : 0;
    identity.stableDeviceId = stableNameIdentity(identity.deviceName);
#if LL_WINDOWS
    if (const auto adapter = queryDxgiAdapter(identity.deviceName, identity.vendor))
    {
        identity.stableDeviceId = adapter->stableId;
        identity.vendorId = adapter->vendorId;
        identity.deviceId = adapter->deviceId;
        if (adapter->dedicatedMemory > 0)
        {
            identity.dedicatedVideoMemoryBytes = adapter->dedicatedMemory;
        }
    }
#endif

    GLint color_attachments = 1;
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &color_attachments);
    GLint storage_bindings = 0;
    if (gGLManager.mGLVersion >= 4.3f)
    {
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &storage_bindings);
    }

    device_caps.maxFramesInFlight = 1;
    device_caps.maxColorAttachments = static_cast<std::uint32_t>(
        std::max(color_attachments, 1));
    device_caps.maxSampledImagesPerStage = static_cast<std::uint32_t>(
        std::max(gGLManager.mNumTextureImageUnits, 1));
    device_caps.maxStorageBuffersPerStage = static_cast<std::uint32_t>(
        std::max(storage_bindings, 0));
    device_caps.maxTexture2DSize = static_cast<std::uint32_t>(
        std::max(gGLManager.mGLMaxTextureSize, 1));
    device_caps.maxUniformBufferSize = static_cast<std::uint32_t>(
        std::max(gGLManager.mMaxUniformBlockSize, 0));
    device_caps.maxVaryingVectors = static_cast<std::uint32_t>(
        std::max(gGLManager.mMaxVaryingVectors, 0));
    device_caps.maxSamples = static_cast<std::uint32_t>(
        std::max(gGLManager.mMaxSamples, 1));
    device_caps.maxBufferSize = 0; // OpenGL exposes target-specific limits only.
    device_caps.timestampQueries = gGLManager.mGLVersion >= 3.3f;
    device_caps.timestampPeriodNanoseconds = device_caps.timestampQueries ? 1.0 : 0.0;
    device_caps.occlusionQueries = gGLManager.mGLVersion >= 1.5f;
    device_caps.descriptorIndexing = false;
    device_caps.storageImageAtomics = gGLManager.mGLVersion >= 4.2f;
    device_caps.depthClamp = gGLManager.mGLVersion >= 3.2f;
    device_caps.baselineGraphicsPipeline = gGLManager.mGLVersion >= 3.f;
    device_caps.advancedGraphicsPipeline = gGLManager.mGLVersion >= 3.99f;

    return snapshot;
}

void publishInitializedOpenGLRendererSnapshot()
{
    publishRendererSnapshot(queryOpenGLRendererSnapshot());
}

} // namespace LL::GHI
