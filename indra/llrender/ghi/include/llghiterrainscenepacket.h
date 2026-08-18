/**
 * @file llghiterrainscenepacket.h
 * @brief Serializable production-terrain resources observed at submission.
 */

#ifndef LL_LLGHITERRAINSCENEPACKET_H
#define LL_LLGHITERRAINSCENEPACKET_H

#include "llghimaterialscenepacket.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace LL::GHI
{

inline constexpr std::uint32_t TERRAIN_SCENE_PACKET_VERSION = 1;
inline constexpr std::size_t TERRAIN_LAYER_COUNT = 4;

enum class TerrainPaintMode : std::uint32_t
{
    HeightmapWithNoise,
    PBRPaintMap,
};

enum class TerrainProjection : std::uint32_t
{
    Planar = 1,
    Triplanar = 3,
};

enum class TerrainDetailMode : std::uint32_t
{
    BaseColor,
    MetallicRoughness,
    Normal,
    Occlusion,
    Emissive,
};

struct TerrainLayerResource
{
    ResourceDigest identity{};
    MaterialModel model = MaterialModel::Legacy;
    ResourceComparability comparability = ResourceComparability::Comparable;
    std::array<float, 4> baseColor{{1.f, 1.f, 1.f, 1.f}};
    std::array<float, 3> emissive{{0.f, 0.f, 0.f}};
    float metallic = 0.f;
    float roughness = 1.f;
    float alphaCutoff = 0.f;
    // KHR_texture_transform source values: offset.xy, scale.xy, rotation.
    std::array<float, 5> transform{{0.f, 0.f, 1.f, 1.f, 0.f}};
    std::uint32_t baseColorTexture = NO_RESOURCE;
    std::uint32_t normalTexture = NO_RESOURCE;
    std::uint32_t metallicRoughnessTexture = NO_RESOURCE;
    std::uint32_t emissiveTexture = NO_RESOURCE;

    friend bool operator==(const TerrainLayerResource&,
                           const TerrainLayerResource&) = default;
};

// A packet may carry several regions. Keeping composition state per region is
// required at region borders and during teleport hand-off.
struct TerrainRegionResource
{
    ResourceDigest identity{};
    MaterialModel model = MaterialModel::Legacy;
    TerrainPaintMode paintMode = TerrainPaintMode::HeightmapWithNoise;
    TerrainProjection projection = TerrainProjection::Planar;
    TerrainDetailMode detailMode = TerrainDetailMode::BaseColor;
    ResourceComparability comparability = ResourceComparability::Comparable;
    float regionScale = 256.f;
    float detailScale = 1.f;
    // Alpha ramp for height/noise composition, RGB difference paint map for
    // PBR paint-map composition.
    std::uint32_t compositionTexture = NO_RESOURCE;
    std::array<TerrainLayerResource, TERRAIN_LAYER_COUNT> layers;

    friend bool operator==(const TerrainRegionResource&,
                           const TerrainRegionResource&) = default;
};

// Canonical terrain input. It matches neither LLVertexBuffer nor a Vulkan
// allocation and retains texcoord1 because it is the legacy composition input.
struct TerrainSceneVertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{{0.f, 0.f, 1.f}};
    std::array<float, 4> tangent{{1.f, 0.f, 0.f, 1.f}};
    std::array<float, 2> compositionCoord{};

    friend bool operator==(const TerrainSceneVertex&,
                           const TerrainSceneVertex&) = default;
};
static_assert(sizeof(TerrainSceneVertex) == 48);

struct TerrainSceneDraw
{
    std::uint64_t semanticId = 0;
    std::uint32_t region = NO_RESOURCE;
    ResourceComparability comparability = ResourceComparability::Comparable;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::array<float, 16> viewProjection{{
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f}};
    std::array<float, 16> modelTransform{{
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f}};

    friend bool operator==(const TerrainSceneDraw&,
                           const TerrainSceneDraw&) = default;
};

// Immutable after encoding. It contains source data only: no OpenGL names,
// Vulkan handles, viewer texture pointers, URLs, credentials, or readbacks.
struct TerrainScenePacket
{
    std::uint32_t version = TERRAIN_SCENE_PACKET_VERSION;
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::vector<MaterialTextureResource> textures;
    std::vector<TerrainRegionResource> regions;
    std::vector<TerrainSceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TerrainSceneDraw> draws;

    friend bool operator==(const TerrainScenePacket&,
                           const TerrainScenePacket&) = default;
};

Status encodeTerrainScenePacket(const TerrainScenePacket& packet,
                                std::vector<std::byte>& encoded);
Status decodeTerrainScenePacket(std::span<const std::byte> encoded,
                                TerrainScenePacket& packet);
std::string terrainScenePacketSha256(const TerrainScenePacket& packet);

} // namespace LL::GHI

#endif // LL_LLGHITERRAINSCENEPACKET_H
