/**
 * @file llghialphascenepacket.h
 * @brief Serializable production alpha work and routing intent.
 */
#ifndef LL_LLGHIALPHASCENEPACKET_H
#define LL_LLGHIALPHASCENEPACKET_H

#include "llghialphacontract.h"
#include "llghimaterialscenepacket.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace LL::GHI
{
inline constexpr std::uint32_t ALPHA_SCENE_PACKET_VERSION = 1;

enum class AlphaBlendFactor : std::uint8_t
{
    Zero,
    One,
    SourceColor,
    OneMinusSourceColor,
    DestinationColor,
    OneMinusDestinationColor,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationAlpha,
    OneMinusDestinationAlpha
};

enum class AlphaBlendOperation : std::uint8_t
{
    Add,
    Subtract,
    ReverseSubtract,
    Minimum,
    Maximum
};

struct AlphaBlendDescription
{
    AlphaBlendFactor sourceColor = AlphaBlendFactor::SourceAlpha;
    AlphaBlendFactor destinationColor = AlphaBlendFactor::OneMinusSourceAlpha;
    AlphaBlendFactor sourceAlpha = AlphaBlendFactor::SourceAlpha;
    AlphaBlendFactor destinationAlpha = AlphaBlendFactor::OneMinusSourceAlpha;
    AlphaBlendOperation colorOperation = AlphaBlendOperation::Add;
    AlphaBlendOperation alphaOperation = AlphaBlendOperation::Add;

    friend bool operator==(const AlphaBlendDescription&,
                           const AlphaBlendDescription&) = default;
};

// Entries align one-to-one with MaterialScenePacket::draws. Vector order is
// the production legacy sort order and is therefore semantic input.
struct AlphaSceneDraw
{
    AlphaSubmissionClass classification =
        AlphaSubmissionClass::StandardBlend;
    bool rigged = false;
    bool fullbright = false;
    bool emissive = false;
    AlphaBlendDescription blend;
    float minimumAlpha = 0.004f;

    friend bool operator==(const AlphaSceneDraw&, const AlphaSceneDraw&) = default;
};

struct AlphaScenePacket
{
    std::uint32_t version = ALPHA_SCENE_PACKET_VERSION;
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    AlphaViewPhase phase = AlphaViewPhase::MainPostWater;
    AlphaMethod requestedMethod = AlphaMethod::LegacySorted;
    bool transientLoad = false;
    AlphaPPLLPolicy ppllPolicy;
    AlphaDepthPeelPolicy depthPeelPolicy;
    MaterialScenePacket materials;
    std::vector<AlphaSceneDraw> draws;

    friend bool operator==(const AlphaScenePacket&,
                           const AlphaScenePacket&) = default;
};

Status validateAlphaScenePacket(const AlphaScenePacket& packet);
Status encodeAlphaScenePacket(const AlphaScenePacket& packet,
                              std::vector<std::byte>& encoded);
Status decodeAlphaScenePacket(std::span<const std::byte> encoded,
                              AlphaScenePacket& packet);
std::string alphaScenePacketSha256(const AlphaScenePacket& packet);
} // namespace LL::GHI

#endif // LL_LLGHIALPHASCENEPACKET_H
