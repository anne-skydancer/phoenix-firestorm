/**
 * @file llghiproductionalphaexecutor.h
 * @brief Private legacy alpha execution on shared production GHI targets.
 */

#ifndef LL_LLGHIPRODUCTIONALPHAEXECUTOR_H
#define LL_LLGHIPRODUCTIONALPHAEXECUTOR_H

#include "llghialphascenepacket.h"
#include "llghidescriptors.h"
#include "llghiproductionframetargets.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace LL::GHI
{
class Device;

struct ProductionAlphaLighting
{
    std::uint64_t generation = 0;
    std::array<float, 3> direction{{0.f, 0.f, 1.f}};
    std::array<float, 3> ambient{{0.2f, 0.2f, 0.2f}};
    std::array<float, 3> directional{{0.8f, 0.8f, 0.8f}};
};

struct ProductionAlphaLimits
{
    std::uint32_t maxDraws = 4096;
    std::uint32_t maxVertices = 1024 * 1024;
    std::uint32_t maxIndices = 3 * 1024 * 1024;
    std::uint64_t maxUploadBytes = 256ull * 1024ull * 1024ull;
    std::uint64_t maxTextureBytes = 64ull * 1024ull * 1024ull;
};

struct ProductionAlphaResult
{
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint64_t targetGeneration = 0;
    std::uint32_t maskDraws = 0;
    std::uint32_t sortedDraws = 0;
    std::uint32_t residualDraws = 0;
    std::uint32_t emissiveReplays = 0;
    std::uint32_t deferredDraws = 0;
    std::uint32_t deferredRouteOrMaterialDraws = 0;
    std::uint32_t deferredSkinDraws = 0;
    std::uint32_t deferredTextureDraws = 0;
    std::uint64_t uploadBytes = 0;
    std::uint64_t modifiedPixels = 0;
    std::string packetSha256;
    std::string colorSha256;
};

class ProductionAlphaExecutor
{
public:
    ProductionAlphaExecutor(Device& device, ShaderPackageDesc shader);
    ~ProductionAlphaExecutor();
    ProductionAlphaExecutor(const ProductionAlphaExecutor&) = delete;
    ProductionAlphaExecutor& operator=(const ProductionAlphaExecutor&) = delete;

    Status submit(const AlphaScenePacket& packet,
                  const ProductionFrameTargetSet& targets,
                  const ProductionAlphaLighting& lighting,
                  const ProductionAlphaLimits& limits);
    Status poll(ProductionAlphaResult& result);
    bool pending() const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONALPHAEXECUTOR_H