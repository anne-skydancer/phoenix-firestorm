/**
 * @file llghioffscreencontract.h
 * @brief Backend-neutral R7 offscreen and recursive-view policy.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIOFFSCREENCONTRACT_H
#define LL_LLGHIOFFSCREENCONTRACT_H

#include "llghialphacontract.h"

#include <cstdint>

namespace LL::GHI
{

enum class RenderViewClass : std::uint8_t
{
    Main,
    ReflectionProbe,
    HeroProbe,
    Mirror,
    CubeSnapshot,
    Impostor,
    DynamicTexture,
    Preview,
    PreWaterAlpha,
    MediaSurface,
};

inline constexpr std::size_t RENDER_VIEW_CLASS_COUNT =
    static_cast<std::size_t>(RenderViewClass::MediaSurface) + 1;

// Layer order is shared by OpenGL cube arrays, Vulkan cube-compatible arrays,
// production probe packets, and the R7 native-peer fixture.
enum class CubeFace : std::uint8_t
{
    PositiveX,
    NegativeX,
    PositiveY,
    NegativeY,
    PositiveZ,
    NegativeZ,
    None = 0xff,
};

enum class ProbePhase : std::uint8_t
{
    None,
    DirectLighting,
    Irradiance,
    Radiance,
};

struct OffscreenPassDesc
{
    RenderViewClass view = RenderViewClass::Main;
    std::uint8_t recursionDepth = 0;
    CubeFace face = CubeFace::None;
    ProbePhase probePhase = ProbePhase::None;
    std::uint16_t arrayLayer = 0;
    std::uint16_t mipLevel = 0;
    std::uint64_t updateEpoch = 0;

    friend bool operator==(const OffscreenPassDesc&,
                           const OffscreenPassDesc&) = default;
};

constexpr bool isCubeView(RenderViewClass view)
{
    return view == RenderViewClass::ReflectionProbe ||
           view == RenderViewClass::HeroProbe ||
           view == RenderViewClass::Mirror ||
           view == RenderViewClass::CubeSnapshot;
}

constexpr bool isValidCubeFace(CubeFace face)
{
    return face >= CubeFace::PositiveX && face <= CubeFace::NegativeZ;
}

constexpr std::uint16_t cubeArrayLayer(std::uint16_t cubeIndex, CubeFace face)
{
    return static_cast<std::uint16_t>(cubeIndex * 6u +
        static_cast<std::uint8_t>(face));
}

constexpr bool validOffscreenPass(const OffscreenPassDesc& pass)
{
    if (static_cast<std::size_t>(pass.view) >= RENDER_VIEW_CLASS_COUNT)
        return false;
    if (pass.view == RenderViewClass::Main)
        return pass.recursionDepth == 0 && pass.face == CubeFace::None &&
               pass.probePhase == ProbePhase::None && pass.arrayLayer == 0 &&
               pass.mipLevel == 0;
    if (pass.recursionDepth != 1)
        return false;
    if (isCubeView(pass.view))
        return isValidCubeFace(pass.face) &&
               pass.arrayLayer % 6u == static_cast<std::uint8_t>(pass.face);
    return pass.face == CubeFace::None && pass.probePhase == ProbePhase::None;
}

constexpr bool maySpawnPass(RenderViewClass parent, RenderViewClass child)
{
    // Recursive/offscreen views never recursively schedule another offscreen
    // world render. This is the primary R7 stall and feedback-loop invariant.
    return parent == RenderViewClass::Main && child != RenderViewClass::Main;
}

constexpr AlphaViewPhase alphaPhaseForView(RenderViewClass view)
{
    switch (view)
    {
    case RenderViewClass::Main: return AlphaViewPhase::MainPostWater;
    case RenderViewClass::ReflectionProbe:
    case RenderViewClass::HeroProbe:
    case RenderViewClass::Mirror: return AlphaViewPhase::Reflection;
    case RenderViewClass::CubeSnapshot: return AlphaViewPhase::CubeSnapshot;
    case RenderViewClass::Impostor: return AlphaViewPhase::Impostor;
    case RenderViewClass::DynamicTexture: return AlphaViewPhase::DynamicTexture;
    case RenderViewClass::Preview: return AlphaViewPhase::DynamicTexture;
    case RenderViewClass::PreWaterAlpha: return AlphaViewPhase::PreWater;
    case RenderViewClass::MediaSurface: return AlphaViewPhase::MediaSurface;
    }
    return AlphaViewPhase::MainPostWater;
}

constexpr std::uint64_t offscreenSemanticId(const OffscreenPassDesc& pass)
{
    // FNV-1a over stable semantic fields. Native handles, timestamps, and
    // frame numbers are intentionally absent; updateEpoch is scene-owned.
    std::uint64_t hash = 1469598103934665603ull;
    const auto append = [&hash](std::uint64_t value)
    {
        for (unsigned byte = 0; byte < 8; ++byte)
        {
            hash ^= (value >> (byte * 8u)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    append(static_cast<std::uint8_t>(pass.view));
    append(pass.recursionDepth);
    append(static_cast<std::uint8_t>(pass.face));
    append(static_cast<std::uint8_t>(pass.probePhase));
    append(pass.arrayLayer);
    append(pass.mipLevel);
    append(pass.updateEpoch);
    return hash;
}

} // namespace LL::GHI

#endif // LL_LLGHIOFFSCREENCONTRACT_H
