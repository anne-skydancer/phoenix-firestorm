/**
 * @file llghiproductionenvironmentexecutor.h
 * @brief Private production sky execution into shared G-buffer targets.
 */
#ifndef LL_LLGHIPRODUCTIONENVIRONMENTEXECUTOR_H
#define LL_LLGHIPRODUCTIONENVIRONMENTEXECUTOR_H

#include "llghienvironmentscenepacket.h"
#include "llghidescriptors.h"
#include "llghiproductionframetargets.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace LL::GHI
{
class Device;

struct ProductionEnvironmentLimits
{
    std::uint32_t maxSkyDraws = 1024;
    std::uint32_t maxSkyVertices = 1024 * 1024;
    std::uint32_t maxSkyIndices = 3 * 1024 * 1024;
    std::uint64_t maxUploadBytes = 96ull * 1024ull * 1024ull;
    std::uint64_t maxTextureBytes = 64ull * 1024ull * 1024ull;
};

struct ProductionEnvironmentResult
{
    std::uint64_t frameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint64_t targetGeneration = 0;
    std::uint32_t atmosphereDraws = 0;
    std::uint32_t sunDraws = 0;
    std::uint32_t moonDraws = 0;
    std::uint32_t starDraws = 0;
    std::uint32_t cloudDraws = 0;
    std::uint64_t uploadBytes = 0;
    std::string packetSha256;
    std::array<std::string, PRODUCTION_GBUFFER_TARGETS> colorSha256;
    std::array<std::uint64_t, PRODUCTION_GBUFFER_TARGETS> nonClearPixels{};
};

class ProductionEnvironmentExecutor
{
public:
    ProductionEnvironmentExecutor(Device& device, ShaderPackageDesc shader);
    ~ProductionEnvironmentExecutor();
    ProductionEnvironmentExecutor(const ProductionEnvironmentExecutor&) = delete;
    ProductionEnvironmentExecutor& operator=(const ProductionEnvironmentExecutor&) = delete;

    Status submit(const EnvironmentScenePacket& packet,
                  const ProductionFrameTargetSet& targets,
                  const ProductionEnvironmentLimits& limits);
    Status poll(ProductionEnvironmentResult& result);
    bool pending() const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONENVIRONMENTEXECUTOR_H
