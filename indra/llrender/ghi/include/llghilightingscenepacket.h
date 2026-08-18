/**
 * @file llghilightingscenepacket.h
 * @brief Backend-neutral live deferred-lighting state observed by the viewer.
 */

#ifndef LL_LLGHILIGHTINGSCENEPACKET_H
#define LL_LLGHILIGHTINGSCENEPACKET_H

#include "llghitypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace LL::GHI
{

inline constexpr std::uint32_t LIGHTING_SCENE_PACKET_VERSION = 1;
inline constexpr std::uint32_t LIGHTING_DIRECTIONAL_SHADOW_CASCADES = 4;
inline constexpr std::uint32_t LIGHTING_PROJECTOR_SHADOWS = 2;

enum class LocalLightKind : std::uint32_t
{
    Point,
    Projector
};

enum class LightingComparability : std::uint32_t
{
    Comparable = 0,
    // OpenGL shadow depth images are deliberately not read back or shared.
    // A later integration slice must render the same caster packets natively.
    ShadowImagesDeferred = 1u << 0,
    ProjectorImageDeferred = 1u << 1
};

constexpr LightingComparability operator|(LightingComparability lhs,
                                            LightingComparability rhs)
{
    return static_cast<LightingComparability>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

struct DirectionalLightRecord
{
    std::array<float, 3> direction{{0.f, 0.f, -1.f}};
    float intensity = 1.f;
    std::array<float, 3> color{{1.f, 1.f, 1.f}};
    bool active = false;

    friend bool operator==(const DirectionalLightRecord&,
                           const DirectionalLightRecord&) = default;
};

struct LocalLightRecord
{
    std::uint64_t semanticId = 0;
    LocalLightKind kind = LocalLightKind::Point;
    LightingComparability comparability = LightingComparability::Comparable;
    std::array<float, 3> position{};
    float radius = 1.f;
    std::array<float, 3> color{{1.f, 1.f, 1.f}};
    float falloff = 1.f;
    // Quaternion xyzw and object scale preserve the projector construction
    // above either native API without carrying a viewer or driver object.
    std::array<float, 4> rotation{{0.f, 0.f, 0.f, 1.f}};
    std::array<float, 3> scale{{1.f, 1.f, 1.f}};
    // fov, focus, ambiance from the production spotlight state.
    std::array<float, 3> projectorParams{};
    std::array<std::uint8_t, 16> projectorTextureIdentity{};
    std::int32_t shadowSlot = -1;
    float shadowFade = 1.f;

    friend bool operator==(const LocalLightRecord&,
                           const LocalLightRecord&) = default;
};

struct LightingShadowState
{
    bool enabled = false;
    std::uint32_t directionalCascadeCount = 0;
    std::uint32_t projectorShadowCount = 0;
    LightingComparability comparability = LightingComparability::Comparable;
    std::array<std::array<float, 16>,
               LIGHTING_DIRECTIONAL_SHADOW_CASCADES +
                   LIGHTING_PROJECTOR_SHADOWS> matrices{};
    std::array<float, 4> clipPlanes{};
    float directionalBias = 0.f;
    float spotShadowOffset = 0.f;
    float spotShadowBias = 0.f;
    std::array<std::uint64_t, LIGHTING_PROJECTOR_SHADOWS> projectorLightIds{};
    std::array<float, LIGHTING_PROJECTOR_SHADOWS> projectorFade{{1.f, 1.f}};

    friend bool operator==(const LightingShadowState&,
                           const LightingShadowState&) = default;
};

// Immutable after encoding. It contains no OpenGL object names, Vulkan
// handles, capability URLs, credentials, or GPU readback data.
struct LightingScenePacket
{
    std::uint32_t version = LIGHTING_SCENE_PACKET_VERSION;
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::array<float, 16> viewMatrix{};
    std::array<float, 16> projectionMatrix{};
    std::array<float, 3> cameraOrigin{};
    std::array<float, 3> ambientColor{};
    DirectionalLightRecord sun;
    DirectionalLightRecord moon;
    LightingShadowState shadows;
    std::vector<LocalLightRecord> localLights;

    friend bool operator==(const LightingScenePacket&,
                           const LightingScenePacket&) = default;
};

Status encodeLightingScenePacket(const LightingScenePacket& packet,
                                 std::vector<std::byte>& encoded);
Status decodeLightingScenePacket(std::span<const std::byte> encoded,
                                 LightingScenePacket& packet);
std::string lightingScenePacketSha256(const LightingScenePacket& packet);

} // namespace LL::GHI

#endif // LL_LLGHILIGHTINGSCENEPACKET_H
