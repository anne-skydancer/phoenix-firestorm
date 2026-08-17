/**
 * @file llghiopaqueoffscreenprobe.h
 * @brief Asynchronous, non-presenting replay of a live opaque scene packet.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIOPAQUEOFFSCREENPROBE_H
#define LL_LLGHIOPAQUEOFFSCREENPROBE_H

#include "llghiopaquepacketconsumer.h"
#include "llghidescriptors.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace LL::GHI
{

class Device;

struct OpaqueOffscreenProbeResult
{
    std::uint64_t frameId = 0;
    std::uint32_t vertices = 0;
    std::uint32_t indices = 0;
    std::uint32_t draws = 0;
    std::string packetSha256;
    std::array<std::string, 4> colorSha256;
    std::array<std::uint64_t, 4> nonClearPixels{};
};

// Records one packet into private color/depth attachments and copies the color
// results to readback buffers. submit() never waits. poll() returns NotReady
// until the backend reports completion, so the production render loop can
// observe the result on a later frame without a queue-idle or fence wait.
// This class owns no surface, swapchain, or presentation object.
class OpaqueOffscreenProbe
{
public:
    OpaqueOffscreenProbe(Device& device, ShaderPackageDesc shader_package);
    ~OpaqueOffscreenProbe();

    OpaqueOffscreenProbe(const OpaqueOffscreenProbe&) = delete;
    OpaqueOffscreenProbe& operator=(const OpaqueOffscreenProbe&) = delete;

    Status submit(const OpaqueScenePacket& packet,
                  const OpaquePacketTransferLimits& limits);
    Status poll(OpaqueOffscreenProbeResult& result);
    bool pending() const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace LL::GHI

#endif // LL_LLGHIOPAQUEOFFSCREENPROBE_H
