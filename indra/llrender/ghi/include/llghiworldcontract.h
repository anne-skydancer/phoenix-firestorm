/**
 * @file llghiworldcontract.h
 * @brief Backend-neutral R5 terrain, lighting, and environment contracts.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIWORLDCONTRACT_H
#define LL_LLGHIWORLDCONTRACT_H

#include <array>
#include <cstdint>

namespace LL::GHI
{

enum class TerrainModel : std::uint8_t
{
    LegacyDetail,
    MetallicRoughness
};

enum class WorldPass : std::uint8_t
{
    TerrainDepth,
    TerrainMaterial,
    DirectionalShadow,
    ProjectorShadow,
    DeferredLighting,
    Atmosphere,
    WaterReflection,
    WaterSurface,
    Underwater
};

// This is a dependency order, not an API command list. A backend may fuse
// adjacent passes only when the externally visible inputs and outputs remain
// identical.
inline constexpr std::array<WorldPass, 9> WORLD_PASS_ORDER{{
    WorldPass::TerrainDepth,
    WorldPass::TerrainMaterial,
    WorldPass::DirectionalShadow,
    WorldPass::ProjectorShadow,
    WorldPass::DeferredLighting,
    WorldPass::Atmosphere,
    WorldPass::WaterReflection,
    WorldPass::WaterSurface,
    WorldPass::Underwater}};

struct TerrainLayerState
{
    std::array<float, 4> uvTransform{{1.f, 1.f, 0.f, 0.f}};
    std::uint32_t baseColorTexture = 0;
    std::uint32_t normalTexture = 0;
    std::uint32_t ormTexture = 0;
    std::uint32_t emissiveTexture = 0;
};

struct TerrainState
{
    TerrainModel model = TerrainModel::LegacyDetail;
    std::array<TerrainLayerState, 4> layers;
    std::uint32_t weightTexture = 0;
    float detailScale = 1.f;
    float heightBlend = 0.f;
    float normalScale = 1.f;
};

struct DirectionalLightState
{
    std::array<float, 3> direction{{0.f, 0.f, -1.f}};
    float intensity = 1.f;
    std::array<float, 3> color{{1.f, 1.f, 1.f}};
    std::uint32_t shadowTexture = 0;
    float constantBias = 0.f;
    float slopeBias = 0.f;
};

struct LocalLightState
{
    std::array<float, 3> position{};
    float radius = 1.f;
    std::array<float, 3> color{{1.f, 1.f, 1.f}};
    float falloff = 1.f;
    std::array<float, 3> projectorDirection{{0.f, 0.f, -1.f}};
    float projectorCutoff = 0.f;
    std::uint32_t projectorTexture = 0;
    std::uint32_t shadowTexture = 0;
    float shadowBias = 0.f;
};

struct EnvironmentState
{
    std::array<float, 3> skyZenith{};
    float haze = 0.f;
    std::array<float, 3> skyHorizon{};
    float waterHeight = 0.f;
    std::array<float, 3> waterColor{};
    float fresnelBase = 0.f;
    std::array<float, 4> waveDirections{};
    float time = 0.f;
    bool underwater = false;
};

static_assert(WORLD_PASS_ORDER[0] == WorldPass::TerrainDepth);
static_assert(WORLD_PASS_ORDER[4] == WorldPass::DeferredLighting);
static_assert(WORLD_PASS_ORDER[7] == WorldPass::WaterSurface);
static_assert(WORLD_PASS_ORDER[8] == WorldPass::Underwater);

} // namespace LL::GHI

#endif // LL_LLGHIWORLDCONTRACT_H
