/**
 * @file llghiopaquepacketconsumer.h
 * @brief Bounded, transfer-only consumption of a live opaque scene packet.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIOPAQUEPACKETCONSUMER_H
#define LL_LLGHIOPAQUEPACKETCONSUMER_H

#include "llghiopaquescenepacket.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace LL::GHI
{

class Device;

// These fixed ceilings keep a developer probe from turning an unusually large
// live scene into an unbounded main-thread allocation. They are intentionally
// lower than the serialized packet format's architectural maxima.
struct OpaquePacketTransferLimits
{
    std::size_t maxVertices = 131072;
    std::size_t maxIndices = 393216;
    std::size_t maxDraws = 256;
    std::uint64_t maxUploadBytes = 16ull * 1024ull * 1024ull;
};

struct OpaquePacketTransferResult
{
    std::uint64_t frameId = 0;
    std::uint64_t uploadBytes = 0;
    std::uint64_t encodedBytes = 0;
    std::uint32_t vertices = 0;
    std::uint32_t indices = 0;
    std::uint32_t draws = 0;
    std::string packetSha256;
};

// Validates and uploads the packet's vertex, index, and transform payloads to
// backend-local buffers in one GHI frame. It records no draw calls, creates no
// render targets, performs no readback, and cannot present.
Status consumeOpaquePacketTransfer(
    Device& device,
    const OpaqueScenePacket& packet,
    const OpaquePacketTransferLimits& limits,
    OpaquePacketTransferResult& result);

} // namespace LL::GHI

#endif // LL_LLGHIOPAQUEPACKETCONSUMER_H
