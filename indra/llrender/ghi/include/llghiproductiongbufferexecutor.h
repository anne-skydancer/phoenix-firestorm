/**
 * @file llghiproductiongbufferexecutor.h
 * @brief Material and terrain execution into shared private frame targets.
 */

#ifndef LL_LLGHIPRODUCTIONGBUFFEREXECUTOR_H
#define LL_LLGHIPRODUCTIONGBUFFEREXECUTOR_H

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

struct ProductionGBufferLimits
{
    std::uint32_t maxMaterialDraws = 256;
    std::uint32_t maxTerrainDraws = 128;
    std::uint64_t maxUploadBytes = 64ull * 1024ull * 1024ull;
};

struct ProductionGBufferResult
{
    std::uint64_t frameId = 0;
    std::uint64_t assemblyEpoch = 0;
    std::uint64_t targetGeneration = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t materialDraws = 0;
    std::uint32_t riggedMaterialDraws = 0;
    std::uint32_t terrainDraws = 0;
    std::uint32_t pbrTerrainDraws = 0;
    std::uint32_t deferredMaterialDraws = 0;
    std::uint32_t deferredTerrainDraws = 0;
    std::uint64_t uploadBytes = 0;
    std::string frameSha256;
    std::array<std::string, PRODUCTION_GBUFFER_TARGETS> colorSha256;
    std::array<std::uint64_t, PRODUCTION_GBUFFER_TARGETS> nonClearPixels{};
};

// submit() records one non-presenting G-buffer execution and never waits.
// poll() resolves verification readbacks after backend completion.
class ProductionGBufferExecutor
{
public:
    ProductionGBufferExecutor(Device& device,
                              ShaderPackageDesc materialShader,
                              ShaderPackageDesc terrainShader);
    ~ProductionGBufferExecutor();

    ProductionGBufferExecutor(const ProductionGBufferExecutor&) = delete;
    ProductionGBufferExecutor& operator=(const ProductionGBufferExecutor&) = delete;

    Status submit(const ProductionFramePacket& frame,
                  const ProductionFrameTargetSet& targets,
                  const ProductionTextureResidency& residency,
                  const ProductionGBufferLimits& limits);
    Status poll(ProductionGBufferResult& result);
    bool pending() const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONGBUFFEREXECUTOR_H
