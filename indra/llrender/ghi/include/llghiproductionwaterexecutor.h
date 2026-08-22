/**
 * @file llghiproductionwaterexecutor.h
 * @brief Private production water composition from explicit GHI dependencies.
 */
#ifndef LL_LLGHIPRODUCTIONWATEREXECUTOR_H
#define LL_LLGHIPRODUCTIONWATEREXECUTOR_H

#include "llghienvironmentscenepacket.h"
#include "llghidescriptors.h"
#include "llghiproductionframetargets.h"
#include "llghiproductionwaterresources.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace LL::GHI
{
class Device;

struct ProductionWaterLimits
{
    std::uint32_t maxDraws = 1024;
    std::uint32_t maxVertices = 4 * 1024 * 1024;
    std::uint32_t maxIndices = 12 * 1024 * 1024;
    std::uint64_t maxUploadBytes = 192ull * 1024ull * 1024ull;
    std::uint64_t maxTextureBytes = 64ull * 1024ull * 1024ull;
};

struct ProductionWaterResult
{
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint64_t targetGeneration = 0;
    std::uint32_t draws = 0;
    std::uint64_t uploadBytes = 0;
    std::uint64_t nonClearPixels = 0;
    std::uint64_t modifiedPixels = 0;
    bool underwater = false;
    std::string packetSha256;
    std::string colorSha256;
    std::vector<std::byte> inputPixels;
    std::vector<std::byte> colorPixels;
};

class ProductionWaterExecutor
{
public:
    ProductionWaterExecutor(Device& device, ShaderPackageDesc shader);
    ~ProductionWaterExecutor();
    ProductionWaterExecutor(const ProductionWaterExecutor&) = delete;
    ProductionWaterExecutor& operator=(const ProductionWaterExecutor&) = delete;

    Status submit(const EnvironmentScenePacket& packet,
                  const ProductionFrameTargetSet& targets,
                  const ProductionWaterDependencies& dependencies,
                  const ProductionWaterLimits& limits);
    Status poll(ProductionWaterResult& result);
    bool pending() const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONWATEREXECUTOR_H
