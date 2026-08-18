/**
 * @file llghiproductionframepacket.h
 * @brief Versioned backend-neutral assembly of one production world frame.
 */

#ifndef LL_LLGHIPRODUCTIONFRAMEPACKET_H
#define LL_LLGHIPRODUCTIONFRAMEPACKET_H

#include "llghilightingscenepacket.h"
#include "llghimaterialscenepacket.h"
#include "llghiterrainscenepacket.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace LL::GHI
{

inline constexpr std::uint32_t PRODUCTION_FRAME_PACKET_VERSION = 1;

enum class ProductionFramePass : std::uint32_t
{
    MaterialGBuffer = 1u << 0,
    TerrainGBuffer = 1u << 1,
    DirectionalShadow = 1u << 2,
    ProjectorShadow = 1u << 3,
    DeferredLighting = 1u << 4,
    ProjectorLighting = 1u << 5,
};

using ProductionFramePassMask = std::uint32_t;

constexpr ProductionFramePassMask productionFramePassBit(
    ProductionFramePass pass)
{
    return static_cast<ProductionFramePassMask>(pass);
}

constexpr bool productionFrameHasPass(ProductionFramePassMask mask,
                                      ProductionFramePass pass)
{
    return (mask & productionFramePassBit(pass)) != 0;
}

// I8a assembles already established material, terrain, and lighting contracts
// without introducing a native handle or importing an OpenGL resource. The
// child epochs remain authoritative for their individual resource streams;
// assemblyEpoch identifies the accepted whole-frame observation.
struct ProductionFramePacket
{
    std::uint32_t version = PRODUCTION_FRAME_PACKET_VERSION;
    std::uint64_t frameId = 0;
    std::uint64_t assemblyEpoch = 0;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    ProductionFramePassMask passes = 0;
    MaterialScenePacket materials;
    TerrainScenePacket terrain;
    LightingScenePacket lighting;

    friend bool operator==(const ProductionFramePacket&,
                           const ProductionFramePacket&) = default;
};

struct ProductionFrameResourceSummary
{
    std::uint64_t decodedTextureBytes = 0;
    std::uint32_t uniqueResources = 0;
    std::uint32_t materialTextures = 0;
    std::uint32_t terrainTextures = 0;
    std::uint32_t projectorTextures = 0;
    std::uint32_t materials = 0;
    std::uint32_t skins = 0;
    std::uint32_t terrainRegions = 0;
    std::uint32_t materialDraws = 0;
    std::uint32_t terrainDraws = 0;
    std::uint32_t vertices = 0;
    std::uint32_t indices = 0;

    friend bool operator==(const ProductionFrameResourceSummary&,
                           const ProductionFrameResourceSummary&) = default;
};

Status validateProductionFramePacket(
    const ProductionFramePacket& packet,
    ProductionFrameResourceSummary* summary = nullptr);
Status encodeProductionFramePacket(const ProductionFramePacket& packet,
                                   std::vector<std::byte>& encoded);
Status decodeProductionFramePacket(std::span<const std::byte> encoded,
                                   ProductionFramePacket& packet);
std::string productionFramePacketSha256(const ProductionFramePacket& packet);

} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONFRAMEPACKET_H
