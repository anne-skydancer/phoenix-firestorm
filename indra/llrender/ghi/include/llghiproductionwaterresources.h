/**
 * @file llghiproductionwaterresources.h
 * @brief GHI-owned production water dependency resources.
 */
#ifndef LL_LLGHIPRODUCTIONWATERRESOURCES_H
#define LL_LLGHIPRODUCTIONWATERRESOURCES_H

#include "llghienvironmentscenepacket.h"
#include "llghidescriptors.h"

#include <cstdint>
#include <memory>
#include <string>

namespace LL::GHI
{
class Device;

struct ProductionWaterDependencies
{
    std::uint64_t generation = 0;
    ImageViewHandle reflectionColorView;
    ImageViewHandle exclusionMaskView;
};

struct ProductionWaterResourceLimits
{
    std::uint64_t maxPixels = 32768ull * 32768ull;
    std::uint64_t maxUploadBytes = 256ull * 1024ull * 1024ull;
};

struct ProductionWaterResourceResult
{
    std::uint64_t frameId = 0;
    std::uint64_t resourceEpoch = 0;
    std::uint64_t generation = 0;
    std::uint32_t reflectionWidth = 0;
    std::uint32_t reflectionHeight = 0;
    std::uint32_t exclusionWidth = 0;
    std::uint32_t exclusionHeight = 0;
    std::uint64_t uploadBytes = 0;
    std::string reflectionSha256;
    std::string exclusionSha256;
};

class ProductionWaterResources
{
public:
    explicit ProductionWaterResources(Device& device);
    ~ProductionWaterResources();

    ProductionWaterResources(const ProductionWaterResources&) = delete;
    ProductionWaterResources& operator=(const ProductionWaterResources&) = delete;

    Status update(const EnvironmentScenePacket& packet,
                  std::uint64_t generation,
                  const ProductionWaterResourceLimits& limits,
                  ProductionWaterResourceResult& result);
    const ProductionWaterDependencies& dependencies() const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONWATERRESOURCES_H
