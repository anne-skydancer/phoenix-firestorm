/**
 * @file llghilightingpacketconsumer.h
 * @brief Bounded transfer-only native consumption of live lighting state.
 */

#ifndef LL_LLGHILIGHTINGPACKETCONSUMER_H
#define LL_LLGHILIGHTINGPACKETCONSUMER_H

#include "llghilightingscenepacket.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace LL::GHI
{

class Device;

struct LightingPacketTransferLimits
{
    std::size_t maxLocalLights = 256;
    std::uint64_t maxUploadBytes = 1024ull * 1024ull;
};

struct LightingPacketTransferResult
{
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint64_t uploadBytes = 0;
    std::uint32_t localLights = 0;
    std::uint32_t projectorLights = 0;
    std::uint32_t shadowCascades = 0;
    std::string packetSha256;
};

// Validates, serializes, and copies the packet into backend-local storage in
// one GHI frame. It records no draw, creates no image or surface, and cannot
// present. I7b will consume the same contract in a deferred-lighting pass.
Status consumeLightingPacketTransfer(
    Device& device,
    const LightingScenePacket& packet,
    const LightingPacketTransferLimits& limits,
    LightingPacketTransferResult& result);

} // namespace LL::GHI

#endif // LL_LLGHILIGHTINGPACKETCONSUMER_H
