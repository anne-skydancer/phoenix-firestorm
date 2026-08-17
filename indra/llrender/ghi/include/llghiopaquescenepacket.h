/**
 * @file llghiopaquescenepacket.h
 * @brief Serializable post-cull opaque scene packet for GHI peer verification.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIOPAQUESCENEPACKET_H
#define LL_LLGHIOPAQUESCENEPACKET_H

#include "llghitypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace LL::GHI
{

inline constexpr std::uint32_t OPAQUE_SCENE_PACKET_VERSION = 1;

enum class OpaqueSceneDrawFlags : std::uint32_t
{
    None = 0,
    Fullbright = 1u << 0,
    AlphaMask = 1u << 1,
    DoubleSided = 1u << 2,
};

constexpr OpaqueSceneDrawFlags operator|(OpaqueSceneDrawFlags lhs,
                                         OpaqueSceneDrawFlags rhs)
{
    return static_cast<OpaqueSceneDrawFlags>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

struct OpaqueSceneVertex
{
    std::array<float, 3> position{};
    std::array<std::uint8_t, 4> color{{255, 255, 255, 255}};

    friend bool operator==(const OpaqueSceneVertex&, const OpaqueSceneVertex&) = default;
};

static_assert(sizeof(OpaqueSceneVertex) == 16);

struct OpaqueSceneDraw
{
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::array<float, 16> transform{};
    std::uint64_t semanticId = 0;
    OpaqueSceneDrawFlags flags = OpaqueSceneDrawFlags::None;

    friend bool operator==(const OpaqueSceneDraw&, const OpaqueSceneDraw&) = default;
};

struct OpaqueSceneStatistics
{
    std::uint64_t submittedDraws = 0;
    std::uint64_t submittedTriangles = 0;
    std::uint64_t capturedDraws = 0;
    std::uint64_t capturedTriangles = 0;
    std::uint64_t skippedRiggedDraws = 0;
    std::uint64_t skippedMaterialDraws = 0;
    std::uint64_t invalidDraws = 0;

    friend bool operator==(const OpaqueSceneStatistics&,
                           const OpaqueSceneStatistics&) = default;
};

// This is an immutable hand-off once encoded. It contains renderer inputs only:
// never native handles, credentials, capability URLs, or simulator payloads.
struct OpaqueScenePacket
{
    std::uint32_t version = OPAQUE_SCENE_PACKET_VERSION;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    bool productionOcclusionEnabled = false;
    OpaqueSceneStatistics statistics;
    std::vector<OpaqueSceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<OpaqueSceneDraw> draws;

    friend bool operator==(const OpaqueScenePacket&, const OpaqueScenePacket&) = default;
};

Status encodeOpaqueScenePacket(const OpaqueScenePacket& packet,
                               std::vector<std::byte>& encoded);
Status decodeOpaqueScenePacket(std::span<const std::byte> encoded,
                               OpaqueScenePacket& packet);
std::string opaqueScenePacketSha256(const OpaqueScenePacket& packet);

} // namespace LL::GHI

#endif // LL_LLGHIOPAQUESCENEPACKET_H
