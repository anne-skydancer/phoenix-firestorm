/**
 * @file llghirendererinfo.h
 * @brief Backend-neutral renderer identity and capability snapshot.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIRENDERERINFO_H
#define LL_LLGHIRENDERERINFO_H

#include "llghitypes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace LL::GHI
{

enum class RendererProvider : std::uint8_t
{
    System,
    MesaZink,
    NativeVulkan,
    Validation,
};

enum class DeviceVendor : std::uint8_t
{
    Unknown,
    AMD,
    NVIDIA,
    Intel,
    Apple,
};

struct ApiVersion
{
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    std::string text;

    friend bool operator==(const ApiVersion&, const ApiVersion&) = default;
};

struct RendererIdentity
{
    Backend backend = Backend::Validation;
    RendererProvider provider = RendererProvider::Validation;
    DeviceVendor vendor = DeviceVendor::Unknown;

    std::string apiName;
    ApiVersion apiVersion;
    std::string rendererName;
    std::string deviceName;
    std::string stableDeviceId;
    std::string vendorName;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::string driverName;
    std::string driverVersion;
    std::uint64_t dedicatedVideoMemoryBytes = 0;
    std::uint64_t detectedVideoMemoryBytes = 0;
    std::uint64_t videoMemoryBudgetBytes = 0;
    bool softwareDevice = false;

    friend bool operator==(const RendererIdentity&, const RendererIdentity&) = default;
};

struct RendererSnapshot
{
    RendererIdentity identity;
    RendererCapabilities capabilities;

    bool complete() const;
    friend bool operator==(const RendererSnapshot&, const RendererSnapshot&) = default;
};

// Canonical support/about fields derived from the same snapshot used by
// feature policy and renderer-change notifications.
struct RendererSupportInfo
{
    std::string api;
    std::string apiVersion;
    std::string backend;
    std::string provider;
    std::string summary;
    std::string vendor;
    std::string renderer;
    std::uint64_t videoMemoryBytes = 0;
    std::uint64_t detectedVideoMemoryBytes = 0;
};

std::string backendDisplayName(Backend backend);
std::string providerDisplayName(RendererProvider provider);
std::string vendorDisplayName(DeviceVendor vendor);
std::string formatApiVersion(const ApiVersion& version);
std::string formatRendererSummary(const RendererIdentity& identity);
RendererSupportInfo makeRendererSupportInfo(const RendererSnapshot& snapshot);

// The lifecycle owner publishes exactly one active snapshot after device
// creation. Consumers receive copies so backend teardown cannot invalidate
// About, diagnostics, or settings-policy readers.
void publishRendererSnapshot(RendererSnapshot snapshot);
std::optional<RendererSnapshot> activeRendererSnapshot();
void clearRendererSnapshot();

// Compatibility lifecycle hook used by the existing OpenGL manager. Native
// discovery remains implemented exclusively by the OpenGL GHI backend.
void publishInitializedOpenGLRendererSnapshot();

} // namespace LL::GHI

#endif // LL_LLGHIRENDERERINFO_H
