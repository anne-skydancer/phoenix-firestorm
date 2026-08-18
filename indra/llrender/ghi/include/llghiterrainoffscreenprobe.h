/**
 * @file llghiterrainoffscreenprobe.h
 * @brief Asynchronous non-presenting replay of production terrain packets.
 */

#ifndef LL_LLGHITERRAINOFFSCREENPROBE_H
#define LL_LLGHITERRAINOFFSCREENPROBE_H

#include "llghiterrainscenepacket.h"
#include "llghidescriptors.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace LL::GHI
{
class Device;

struct TerrainOffscreenProbeLimits
{
    std::uint32_t maxDraws = 16;
    std::uint32_t maxVertices = 131072;
    std::uint32_t maxIndices = 393216;
    std::uint32_t maxTextures = 80;
    std::uint64_t maxUploadBytes = 16ull * 1024ull * 1024ull;
    std::uint64_t maxTextureBytes = 4ull * 1024ull * 1024ull;
};

struct TerrainOffscreenProbeResult
{
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint32_t vertices = 0;
    std::uint32_t indices = 0;
    std::uint32_t draws = 0;
    std::uint32_t regions = 0;
    std::uint32_t pbrDraws = 0;
    std::uint32_t triplanarDraws = 0;
    std::string packetSha256;
    std::array<std::string, 4> colorSha256;
    std::array<std::uint64_t, 4> nonClearPixels{};
};

class TerrainOffscreenProbe
{
public:
    TerrainOffscreenProbe(Device& device, ShaderPackageDesc package);
    ~TerrainOffscreenProbe();
    TerrainOffscreenProbe(const TerrainOffscreenProbe&) = delete;
    TerrainOffscreenProbe& operator=(const TerrainOffscreenProbe&) = delete;

    Status submit(const TerrainScenePacket& packet,
                  const TerrainOffscreenProbeLimits& limits);
    Status poll(TerrainOffscreenProbeResult& result);
    bool pending() const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace LL::GHI

#endif // LL_LLGHITERRAINOFFSCREENPROBE_H
