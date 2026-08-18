/**
 * @file llghimaterialoffscreenprobe.h
 * @brief Asynchronous, non-presenting replay of rigid and rigged opaque PBR draws.
 */

#ifndef LL_LLGHIMATERIALOFFSCREENPROBE_H
#define LL_LLGHIMATERIALOFFSCREENPROBE_H

#include "llghimaterialscenepacket.h"
#include "llghilightingscenepacket.h"
#include "llghidescriptors.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace LL::GHI
{

class Device;

struct MaterialOffscreenProbeLimits
{
    std::uint32_t maxDraws = 32;
    std::uint32_t maxVertices = 65536;
    std::uint32_t maxIndices = 196608;
    std::uint32_t maxTextures = 64;
    std::uint64_t maxUploadBytes = 16ull * 1024ull * 1024ull;
    std::uint64_t maxTextureBytes = 4ull * 1024ull * 1024ull;
    std::uint32_t maxPointLights = 64;
    std::uint32_t maxProjectorLights = 8;
    std::uint64_t maxProjectorTextureBytes = 512ull * 1024ull;
    std::uint32_t maxShadowDraws = 32;
};

struct MaterialOffscreenProbeResult
{
    std::uint64_t frameId = 0;
    std::uint32_t vertices = 0;
    std::uint32_t indices = 0;
    std::uint32_t draws = 0;
    std::uint32_t riggedDraws = 0;
    std::uint32_t maxJointCount = 0;
    std::uint32_t textureTransformedDraws = 0;
    std::uint32_t textures = 0;
    std::string packetSha256;
    std::array<std::string, 4> colorSha256;
    std::array<std::uint64_t, 4> nonClearPixels{};
    bool lightingExecuted = false;
    std::uint32_t directionalLights = 0;
    std::uint32_t pointLights = 0;
    std::uint32_t projectorLights = 0;
    std::uint32_t projectorTextures = 0;
    std::uint32_t projectorVolumeLights = 0;
    std::uint32_t projectorFullscreenLights = 0;
    bool shadowsExecuted = false;
    std::uint32_t shadowMaps = 0;
    std::uint32_t directionalShadowMaps = 0;
    std::uint32_t projectorShadowMaps = 0;
    std::uint32_t shadowCasterDraws = 0;
    std::uint32_t shadowRiggedDraws = 0;
    std::uint32_t shadowMaskedDraws = 0;
    std::array<std::string, 6> shadowDepthSha256;
    std::array<std::uint64_t, 6> shadowNonClearPixels{};
    std::string lightingPacketSha256;
    std::string litColorSha256;
    std::uint64_t litNonClearPixels = 0;
};

// submit() records at most one sample and never waits. poll() returns NotReady
// until the backend completes its readbacks. No surface, swapchain, or
// presentation object is created by this class.
class MaterialOffscreenProbe
{
public:
    MaterialOffscreenProbe(Device& device, ShaderPackageDesc shader_package);
    MaterialOffscreenProbe(Device& device,
                           ShaderPackageDesc material_shader_package,
                           ShaderPackageDesc lighting_shader_package);
    MaterialOffscreenProbe(Device& device,
                           ShaderPackageDesc material_shader_package,
                           ShaderPackageDesc lighting_shader_package,
                           ShaderPackageDesc projector_shader_package);
    MaterialOffscreenProbe(Device& device,
                           ShaderPackageDesc material_shader_package,
                           ShaderPackageDesc lighting_shader_package,
                           ShaderPackageDesc projector_shader_package,
                           ShaderPackageDesc shadow_shader_package);
    ~MaterialOffscreenProbe();

    MaterialOffscreenProbe(const MaterialOffscreenProbe&) = delete;
    MaterialOffscreenProbe& operator=(const MaterialOffscreenProbe&) = delete;

    Status submit(const MaterialScenePacket& packet,
                  const MaterialOffscreenProbeLimits& limits);
    Status submit(const MaterialScenePacket& material_packet,
                  const LightingScenePacket& lighting_packet,
                  const MaterialOffscreenProbeLimits& limits);
    Status poll(MaterialOffscreenProbeResult& result);
    bool pending() const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace LL::GHI

#endif // LL_LLGHIMATERIALOFFSCREENPROBE_H
