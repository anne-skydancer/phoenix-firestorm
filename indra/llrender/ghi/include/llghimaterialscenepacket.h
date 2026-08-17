/**
 * @file llghimaterialscenepacket.h
 * @brief Serializable material and skin resources observed at submission.
 */

#ifndef LL_LLGHIMATERIALSCENEPACKET_H
#define LL_LLGHIMATERIALSCENEPACKET_H

#include "llghitypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace LL::GHI
{

inline constexpr std::uint32_t MATERIAL_SCENE_PACKET_VERSION = 2;
inline constexpr std::uint32_t NO_RESOURCE = 0xffffffffu;
using ResourceDigest = std::array<std::byte, 32>;

enum class ResourceComparability : std::uint32_t
{
    Comparable = 0,
    MissingCpuTexture = 1u << 0,
    TextureStillFetching = 1u << 1,
    MissingSkinPalette = 1u << 2,
    AlphaDeferred = 1u << 3,
    UnsupportedVertexLayout = 1u << 4,
};

constexpr ResourceComparability operator|(ResourceComparability lhs,
                                            ResourceComparability rhs)
{
    return static_cast<ResourceComparability>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

enum class TextureSemantic : std::uint32_t
{
    BaseColor,
    Normal,
    MetallicRoughness,
    Emissive,
    LegacySpecular,
};

enum class TextureColorSpace : std::uint32_t { Linear, SRGB };
enum class MaterialModel : std::uint32_t { Legacy, MetallicRoughness };
enum class MaterialAlphaMode : std::uint32_t { Opaque, Mask, Blend };

struct MaterialTextureBinding
{
    TextureSemantic semantic = TextureSemantic::BaseColor;
    std::uint32_t texture = NO_RESOURCE;
    std::uint32_t texcoord = 0;
    // offset.xy, scale.xy, rotation. This is the source contract rather than
    // a backend-specific texture matrix.
    std::array<float, 5> transform{{0.f, 0.f, 1.f, 1.f, 0.f}};

    friend bool operator==(const MaterialTextureBinding&,
                           const MaterialTextureBinding&) = default;
};

struct MaterialTextureResource
{
    // sourceIdentity identifies the asset/version. contentIdentity identifies
    // the exact decoded bytes below. They are deliberately not interchangeable.
    ResourceDigest sourceIdentity{};
    ResourceDigest contentIdentity{};
    TextureColorSpace colorSpace = TextureColorSpace::Linear;
    ResourceComparability comparability = ResourceComparability::Comparable;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t components = 0;
    std::uint32_t discardLevel = 0;
    std::vector<std::byte> decodedPixels;

    friend bool operator==(const MaterialTextureResource&,
                           const MaterialTextureResource&) = default;
};

struct MaterialResource
{
    ResourceDigest identity{};
    MaterialModel model = MaterialModel::Legacy;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    ResourceComparability comparability = ResourceComparability::Comparable;
    std::array<float, 4> baseColor{{1.f, 1.f, 1.f, 1.f}};
    std::array<float, 3> emissive{{0.f, 0.f, 0.f}};
    std::array<float, 4> legacySpecular{{0.f, 0.f, 0.f, 0.f}};
    float metallic = 0.f;
    float roughness = 1.f;
    float alphaCutoff = 0.f;
    float environmentIntensity = 0.f;
    bool doubleSided = false;
    bool fullbright = false;
    std::vector<MaterialTextureBinding> textures;

    friend bool operator==(const MaterialResource&, const MaterialResource&) = default;
};

struct SkinResource
{
    ResourceDigest identity{};
    ResourceComparability comparability = ResourceComparability::Comparable;
    std::uint32_t jointCount = 0;
    // Canonical row-major 3x4 affine matrices, 12 floats per joint.
    std::vector<float> matrixPalette;

    friend bool operator==(const SkinResource&, const SkinResource&) = default;
};

struct MaterialSceneDraw
{
    std::uint64_t semanticId = 0;
    std::uint32_t material = NO_RESOURCE;
    std::uint32_t skin = NO_RESOURCE;
    ResourceComparability comparability = ResourceComparability::Comparable;
    // A zero indexCount is a semantic-only R5 observation. I3 packets carry a
    // real post-cull range and clip transform without backend-native handles.
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    // Camera view-projection and object model are separate so the material
    // shader can transform normals without trying to invert a clip matrix.
    std::array<float, 16> transform{{
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f}};
    std::array<float, 16> modelTransform{{
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f}};

    friend bool operator==(const MaterialSceneDraw&, const MaterialSceneDraw&) = default;
};

// Canonical R5 shader input. This deliberately matches neither an OpenGL VBO
// nor a Vulkan allocation: it is an immutable render-agnostic hand-off.
struct MaterialSceneVertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{{0.f, 0.f, 1.f}};
    std::array<float, 4> tangent{{1.f, 0.f, 0.f, 1.f}};
    std::array<float, 2> texCoord{};
    std::array<std::uint8_t, 4> color{{255, 255, 255, 255}};
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{{1.f, 0.f, 0.f, 0.f}};

    friend bool operator==(const MaterialSceneVertex&,
                           const MaterialSceneVertex&) = default;
};
static_assert(sizeof(MaterialSceneVertex) == 76);

// Immutable after encoding. No native handles, capability URLs, credentials,
// or GPU readback data are permitted in this hand-off.
struct MaterialScenePacket
{
    std::uint32_t version = MATERIAL_SCENE_PACKET_VERSION;
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::vector<MaterialTextureResource> textures;
    std::vector<MaterialResource> materials;
    std::vector<SkinResource> skins;
    std::vector<MaterialSceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MaterialSceneDraw> draws;

    friend bool operator==(const MaterialScenePacket&, const MaterialScenePacket&) = default;
};

Status encodeMaterialScenePacket(const MaterialScenePacket& packet,
                                 std::vector<std::byte>& encoded);
Status decodeMaterialScenePacket(std::span<const std::byte> encoded,
                                 MaterialScenePacket& packet);
std::string materialScenePacketSha256(const MaterialScenePacket& packet);

} // namespace LL::GHI

#endif // LL_LLGHIMATERIALSCENEPACKET_H
