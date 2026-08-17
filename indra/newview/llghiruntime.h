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
}

namespace LLGHIRuntime
{

void initialize();
void shutdown();
bool active();
bool shouldCaptureLiveOpaquePacket(std::uint64_t frame_id);
void consumeLiveOpaquePacket(const LL::GHI::OpaqueScenePacket& packet,
                             bool budget_limited);

} // namespace LLGHIRuntime

#endif // LL_LLGHIRUNTIME_H
