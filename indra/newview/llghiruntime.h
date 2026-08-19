/**
 * @file llghiruntime.h
 * @brief Developer-gated native Vulkan coexistence lifetime.
 *
 * The production viewer remains OpenGL. This owner exists so the real viewer
 * process can validate a native Vulkan GHI device before production rendering
 * is routed to it.
 */

#ifndef LL_LLGHIRUNTIME_H
#define LL_LLGHIRUNTIME_H

#include <cstdint>

namespace LL::GHI
{
struct OpaqueScenePacket;
struct MaterialScenePacket;
struct TerrainScenePacket;
struct LightingScenePacket;
}

namespace LLGHIRuntime
{

void initialize();
void shutdown();
bool active();
bool productionFrameCaptureRequested();
bool shouldCaptureLiveOpaquePacket(std::uint64_t frame_id);
void consumeLiveOpaquePacket(const LL::GHI::OpaqueScenePacket& packet,
                             bool budget_limited);
bool materialCaptureRequested();
bool shadowOffscreenRequested();
bool shouldCaptureLiveMaterialPacket(std::uint64_t frame_id);
void consumeLiveMaterialPacket(const LL::GHI::MaterialScenePacket& packet,
                               bool budget_limited);
bool terrainCaptureRequested();
bool shouldCaptureLiveTerrainPacket(std::uint64_t frame_id);
void consumeLiveTerrainPacket(const LL::GHI::TerrainScenePacket& packet,
                              bool budget_limited);
bool lightingCaptureRequested();
bool shouldCaptureLiveLightingPacket(std::uint64_t frame_id);
void consumeLiveLightingPacket(const LL::GHI::LightingScenePacket& packet,
                               bool budget_limited);

} // namespace LLGHIRuntime

#endif // LL_LLGHIRUNTIME_H
