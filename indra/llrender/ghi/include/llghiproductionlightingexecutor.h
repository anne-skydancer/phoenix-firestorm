/**
 * @file llghiproductionlightingexecutor.h
 * @brief Shadow and deferred-light execution on shared production targets.
 */

#ifndef LL_LLGHIPRODUCTIONLIGHTINGEXECUTOR_H
#define LL_LLGHIPRODUCTIONLIGHTINGEXECUTOR_H

#include "llghidescriptors.h"
#include "llghiproductionframetargets.h"
#include "llghiproductiontextureresidency.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace LL::GHI
{

class Device;

struct ProductionLightingLimits
{
    std::uint32_t maxPointLights = 64;
    std::uint32_t maxProjectorLights = 8;
    std::uint32_t maxShadowDraws = 64;
    std::uint64_t maxUploadBytes = 64ull * 1024ull * 1024ull;
};

struct ProductionLightingResult
{
    std::uint64_t frameId = 0;
    std::uint64_t assemblyEpoch = 0;
    std::uint64_t targetGeneration = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t directionalLights = 0;
    std::uint32_t pointLights = 0;
    std::uint32_t projectorLights = 0;
    std::uint32_t projectorTextures = 0;
    std::uint32_t projectorVolumeLights = 0;
    std::uint32_t projectorFullscreenLights = 0;
    std::uint32_t shadowMaps = 0;
    std::uint32_t directionalShadowMaps = 0;
    std::uint32_t projectorShadowMaps = 0;
    std::uint32_t shadowCasterDraws = 0;
    std::uint32_t shadowRiggedDraws = 0;
    std::uint32_t shadowMaskedDraws = 0;
    std::uint32_t deferredShadowDraws = 0;
    std::uint64_t uploadBytes = 0;
    std::string frameSha256;
    std::string lightingSha256;
    std::uint64_t litNonClearPixels = 0;
    std::array<std::string, PRODUCTION_SHADOW_TARGETS> shadowDepthSha256;
    std::array<std::uint64_t, PRODUCTION_SHADOW_TARGETS>
        shadowNonClearPixels{};
    std::array<bool, PRODUCTION_SHADOW_TARGETS> shadowActive{};
};

// submit() appends private shadow and lighting work after the I8c2 G-buffer
// submission. Queue order preserves the shared frame identity; no surface,
// swapchain, or presentation object is created. poll() resolves verification
// readbacks after backend completion.
class ProductionLightingExecutor
{
public:
    ProductionLightingExecutor(Device& device,
                               ShaderPackageDesc lightingShader,
                               ShaderPackageDesc projectorShader,
                               ShaderPackageDesc shadowShader);
    ~ProductionLightingExecutor();

    ProductionLightingExecutor(const ProductionLightingExecutor&) = delete;
    ProductionLightingExecutor& operator=(
        const ProductionLightingExecutor&) = delete;

    Status submit(const ProductionFramePacket& frame,
                  const ProductionFrameTargetSet& targets,
                  const ProductionTextureResidency& residency,
                  const ProductionLightingLimits& limits);
    Status poll(ProductionLightingResult& result);
    bool pending() const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONLIGHTINGEXECUTOR_H
