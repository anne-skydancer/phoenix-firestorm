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

namespace LLGHIRuntime
{

void initialize();
void shutdown();
bool active();

} // namespace LLGHIRuntime

#endif // LL_LLGHIRUNTIME_H
