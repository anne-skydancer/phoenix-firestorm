/**
 * @file llghirendererinfo.cpp
 * @brief Backend-neutral renderer identity formatting and publication.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "ghi/include/llghirendererinfo.h"

#include <mutex>
#include <sstream>
#include <utility>

namespace LL::GHI
{

namespace
{
std::mutex gSnapshotMutex;
std::optional<RendererSnapshot> gActiveSnapshot;
}

bool RendererSnapshot::complete() const
{
    return !identity.apiName.empty() &&
           !identity.deviceName.empty() &&
           !identity.stableDeviceId.empty();
}

std::string backendDisplayName(Backend backend)
{
    switch (backend)
    {
        case Backend::OpenGL: return "OpenGL";
        case Backend::Vulkan: return "Vulkan";
        case Backend::Validation: return "Validation";
    }
    return "Unknown";
}

std::string providerDisplayName(RendererProvider provider)
{
    switch (provider)
    {
        case RendererProvider::System: return "System OpenGL";
        case RendererProvider::MesaZink: return "Mesa + Zink";
        case RendererProvider::NativeVulkan: return "Native Vulkan";
        case RendererProvider::Validation: return "Validation";
    }
    return "Unknown";
}

std::string vendorDisplayName(DeviceVendor vendor)
{
    switch (vendor)
    {
        case DeviceVendor::AMD: return "AMD";
        case DeviceVendor::NVIDIA: return "NVIDIA";
        case DeviceVendor::Intel: return "Intel";
        case DeviceVendor::Apple: return "Apple";
        case DeviceVendor::Unknown: break;
    }
    return "Unknown";
}

std::string formatApiVersion(const ApiVersion& version)
{
    if (!version.text.empty())
    {
        return version.text;
    }

    std::ostringstream output;
    output << version.major << '.' << version.minor;
    if (version.patch != 0)
    {
        output << '.' << version.patch;
    }
    return output.str();
}

std::string formatRendererSummary(const RendererIdentity& identity)
{
    std::ostringstream output;
    output << (identity.apiName.empty() ? backendDisplayName(identity.backend) : identity.apiName);
    const std::string version = formatApiVersion(identity.apiVersion);
    if (!version.empty() && version != "0.0")
    {
        output << ' ' << version;
    }
    if (!identity.deviceName.empty())
    {
        output << " (" << identity.deviceName << " - "
               << providerDisplayName(identity.provider) << ')';
    }
    return output.str();
}

void publishRendererSnapshot(RendererSnapshot snapshot)
{
    std::lock_guard<std::mutex> lock(gSnapshotMutex);
    gActiveSnapshot = std::move(snapshot);
}

std::optional<RendererSnapshot> activeRendererSnapshot()
{
    std::lock_guard<std::mutex> lock(gSnapshotMutex);
    return gActiveSnapshot;
}

void clearRendererSnapshot()
{
    std::lock_guard<std::mutex> lock(gSnapshotMutex);
    gActiveSnapshot.reset();
}

} // namespace LL::GHI
