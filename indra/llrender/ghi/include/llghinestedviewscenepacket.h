/**
 * @file llghinestedviewscenepacket.h
 * @brief Deterministic backend-neutral recursive and offscreen view schedule.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHINESTEDVIEWSCENEPACKET_H
#define LL_LLGHINESTEDVIEWSCENEPACKET_H

#include "llghioffscreencontract.h"
#include "llghitypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace LL::GHI
{

inline constexpr std::uint32_t NESTED_VIEW_SCENE_PACKET_VERSION = 2;

struct NestedViewPass
{
    std::uint64_t semanticId = 0;
    std::uint64_t resourceGeneration = 0;
    OffscreenPassDesc pass;

    friend bool operator==(const NestedViewPass&, const NestedViewPass&) = default;
};

struct NestedViewScenePacket
{
    std::uint32_t version = NESTED_VIEW_SCENE_PACKET_VERSION;
    std::uint64_t frameId = 0;
    std::uint64_t sceneGeneration = 0;
    std::uint64_t resourceGeneration = 0;
    std::vector<NestedViewPass> passes;

    friend bool operator==(const NestedViewScenePacket&,
                           const NestedViewScenePacket&) = default;
};

Status validateNestedViewScenePacket(const NestedViewScenePacket& packet);
Status encodeNestedViewScenePacket(const NestedViewScenePacket& packet,
                                   std::vector<std::byte>& encoded);
Status decodeNestedViewScenePacket(std::span<const std::byte> encoded,
                                   NestedViewScenePacket& packet);
std::string nestedViewScenePacketSha256(const NestedViewScenePacket& packet);

} // namespace LL::GHI

#endif // LL_LLGHINESTEDVIEWSCENEPACKET_H