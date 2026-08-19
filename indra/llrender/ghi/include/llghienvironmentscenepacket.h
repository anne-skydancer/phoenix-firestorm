/**
 * @file llghienvironmentscenepacket.h
 * @brief Backend-neutral sky, cloud, celestial, and water scene input.
 */

#ifndef LL_LLGHIENVIRONMENTSCENEPACKET_H
#define LL_LLGHIENVIRONMENTSCENEPACKET_H

#include "llghimaterialscenepacket.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace LL::GHI
{

inline constexpr std::uint32_t ENVIRONMENT_SCENE_PACKET_VERSION = 1;

enum class EnvironmentViewKind : std::uint32_t
{
    Main,
    WaterReflection,
    ReflectionProbeRadiance,
    ReflectionProbeIrradiance,
    CubeSnapshot,
    Impostor,
    Preview,
};

enum class EnvironmentPass : std::uint32_t
{
    Atmosphere = 1u << 0,
    HdriSky = 1u << 1,
    Sun = 1u << 2,
    Moon = 1u << 3,
    Stars = 1u << 4,
    Clouds = 1u << 5,
    WaterSurface = 1u << 6,
    Underwater = 1u << 7,
};

constexpr std::uint32_t environmentPassBit(EnvironmentPass pass)
{
    return static_cast<std::uint32_t>(pass);
}

enum class EnvironmentDependency : std::uint32_t
{
    ProductionLighting = 1u << 0,
    ProductionDepth = 1u << 1,
    WaterExclusionMask = 1u << 2,
    ReflectionColor = 1u << 3,
    RefractionColor = 1u << 4,
};

constexpr std::uint32_t environmentDependencyBit(EnvironmentDependency dependency)
{
    return static_cast<std::uint32_t>(dependency);
}

enum class EnvironmentTextureSemantic : std::uint32_t
{
    Hdri,
    Rainbow,
    Halo,
    Sun,
    SunNext,
    Moon,
    MoonNext,
    StarBloom,
    StarBloomNext,
    CloudNoise,
    CloudNoiseNext,
    WaterNormal,
    WaterNormalNext,
};

struct EnvironmentTextureBinding
{
    EnvironmentTextureSemantic semantic = EnvironmentTextureSemantic::Hdri;
    std::uint32_t texture = NO_RESOURCE;

    friend bool operator==(const EnvironmentTextureBinding&,
                           const EnvironmentTextureBinding&) = default;
};

struct AtmosphereState
{
    std::array<float, 3> ambient{};
    std::array<float, 3> blueDensity{};
    std::array<float, 3> blueHorizon{};
    std::array<float, 3> sunlight{};
    std::array<float, 3> moonlight{};
    std::array<float, 3> glow{};
    float hazeDensity = 0.f;
    float hazeHorizon = 0.f;
    float densityMultiplier = 0.f;
    float distanceMultiplier = 0.f;
    float maxAltitude = 0.f;
    float gamma = 1.f;
    float planetRadius = 0.f;
    float skyBottomRadius = 0.f;
    float skyTopRadius = 0.f;
    float sunArcRadians = 0.f;
    float mieAnisotropy = 0.f;
    float moisture = 0.f;
    float dropletRadius = 0.f;
    float iceLevel = 0.f;
    float reflectionProbeAmbiance = 0.f;
    float skyHdrScale = 1.f;
    float skySunlightScale = 1.f;
    float skyAmbientScale = 1.f;
    float sceneLightStrength = 1.f;
    float tonemapMix = 1.f;
    bool classicMode = false;

    friend bool operator==(const AtmosphereState&,
                           const AtmosphereState&) = default;
};

struct SkyLayerState
{
    // Explicit transforms replace ambient model-view stack manipulation.
    std::array<float, 16> domeTransform{};
    std::array<float, 3> lightDirection{{0.f, 0.f, -1.f}};
    std::array<float, 3> sunDirection{{0.f, 0.f, -1.f}};
    std::array<float, 3> moonDirection{{0.f, 0.f, 1.f}};
    std::array<float, 4> sunColor{{1.f, 1.f, 1.f, 1.f}};
    std::array<float, 4> moonColor{{1.f, 1.f, 1.f, 1.f}};
    std::array<float, 3> cloudColor{{1.f, 1.f, 1.f}};
    std::array<float, 3> cloudPositionDensity1{};
    std::array<float, 3> cloudPositionDensity2{};
    std::array<float, 2> cloudScrollDelta{};
    float cloudScale = 1.f;
    float cloudShadow = 0.f;
    float cloudVariance = 0.f;
    float sunMoonGlowFactor = 0.f;
    float moonBrightness = 1.f;
    float starBrightness = 0.f;
    float starPhase = 0.f;
    float blendFactor = 0.f;
    float hdriExposure = 1.f;
    float hdriRotation = 0.f;
    float hdriSplitScreen = 0.f;
    bool sunUp = false;
    bool moonUp = false;
    std::vector<EnvironmentTextureBinding> textures;

    friend bool operator==(const SkyLayerState&,
                           const SkyLayerState&) = default;
};

struct WaterState
{
    std::array<float, 3> fogColor{};
    std::array<float, 3> normalScale{{1.f, 1.f, 1.f}};
    std::array<float, 2> waveDirection1{};
    std::array<float, 2> waveDirection2{};
    std::array<float, 3> lightDirection{{0.f, 0.f, -1.f}};
    std::array<float, 3> lightColor{};
    std::array<float, 3> clampedLightNormal{{0.f, 0.f, -1.f}};
    float waterHeight = 0.f;
    float cameraToWaterHeight = 0.f;
    float fogDensity = 0.f;
    float fresnelScale = 0.f;
    float fresnelOffset = 0.f;
    float blurMultiplier = 0.f;
    float scaleAbove = 0.f;
    float scaleBelow = 0.f;
    float phase = 0.f;
    float normalBlendFactor = 0.f;
    float exposure = 1.f;
    float tonemapMix = 1.f;
    std::uint32_t tonemapType = 0;
    bool normalMipFiltering = false;
    bool sunUp = false;
    std::vector<EnvironmentTextureBinding> textures;

    friend bool operator==(const WaterState&, const WaterState&) = default;
};

struct WaterSceneVertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{{0.f, 0.f, 1.f}};
    std::array<float, 2> texCoord{};

    friend bool operator==(const WaterSceneVertex&,
                           const WaterSceneVertex&) = default;
};
static_assert(sizeof(WaterSceneVertex) == 32);

struct WaterSceneDraw
{
    std::uint64_t semanticId = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::array<float, 16> modelTransform{};
    bool edgePatch = false;

    friend bool operator==(const WaterSceneDraw&,
                           const WaterSceneDraw&) = default;
};

// Immutable after encoding. Attachment dependencies are semantic inputs from
// the shared production graph, never OpenGL framebuffer names or Vulkan views.
struct EnvironmentScenePacket
{
    std::uint32_t version = ENVIRONMENT_SCENE_PACKET_VERSION;
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    EnvironmentViewKind viewKind = EnvironmentViewKind::Main;
    std::uint32_t passMask = 0;
    std::uint32_t dependencyMask = 0;
    std::array<float, 16> viewMatrix{};
    std::array<float, 16> projectionMatrix{};
    std::array<float, 3> cameraOrigin{};
    AtmosphereState atmosphere;
    SkyLayerState sky;
    WaterState water;
    std::vector<MaterialTextureResource> textures;
    std::vector<WaterSceneVertex> waterVertices;
    std::vector<std::uint32_t> waterIndices;
    std::vector<WaterSceneDraw> waterDraws;

    friend bool operator==(const EnvironmentScenePacket&,
                           const EnvironmentScenePacket&) = default;
};

Status validateEnvironmentScenePacket(const EnvironmentScenePacket& packet);
Status encodeEnvironmentScenePacket(const EnvironmentScenePacket& packet,
                                    std::vector<std::byte>& encoded);
Status decodeEnvironmentScenePacket(std::span<const std::byte> encoded,
                                    EnvironmentScenePacket& packet);
std::string environmentScenePacketSha256(const EnvironmentScenePacket& packet);

} // namespace LL::GHI

#endif // LL_LLGHIENVIRONMENTSCENEPACKET_H
