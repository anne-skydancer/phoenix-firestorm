/**
 * @file llghiproductiontextureresidency.h
 * @brief Retained decoded-texture residency for coherent production frames.
 */

#ifndef LL_LLGHIPRODUCTIONTEXTURERESIDENCY_H
#define LL_LLGHIPRODUCTIONTEXTURERESIDENCY_H

#include "llghiproductionframepacket.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace LL::GHI
{

class Device;

enum class ProductionTextureDomain : std::uint8_t
{
    Material,
    Terrain,
    Projector,
};

struct ProductionTextureSourceIdentity
{
    ProductionTextureDomain domain = ProductionTextureDomain::Material;
    ResourceDigest source{};

    friend bool operator==(const ProductionTextureSourceIdentity&,
                           const ProductionTextureSourceIdentity&) = default;
    friend auto operator<=>(const ProductionTextureSourceIdentity&,
                            const ProductionTextureSourceIdentity&) = default;
};

struct ProductionTextureBinding
{
    ImageHandle image;
    ImageViewHandle view;
    Format format = Format::Undefined;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t mipLevels = 0;
    std::uint64_t generation = 0;
    ResourceDigest contentIdentity{};
};

struct ProductionTextureResidencyLimits
{
    std::size_t maxEntries = 1024;
    std::size_t maxSourceRecords = 4096;
    std::uint64_t maxResidentBytes = 512ull * 1024ull * 1024ull;
    std::uint64_t maxUploadBytesPerFrame = 32ull * 1024ull * 1024ull;
    std::uint64_t staleAfterAssemblyEpochs = 120;
};

struct ProductionTextureResidencyResult
{
    std::uint64_t frameId = 0;
    std::uint64_t assemblyEpoch = 0;
    std::uint32_t requestedSources = 0;
    std::uint32_t uniqueContents = 0;
    std::uint32_t cacheHits = 0;
    std::uint32_t uploads = 0;
    std::uint32_t generationChanges = 0;
    std::uint32_t evictions = 0;
    std::uint32_t deferredSources = 0;
    std::uint32_t residentEntries = 0;
    std::uint64_t uploadBytes = 0;
    std::uint64_t residentBytes = 0;
};

class ProductionTextureResidency
{
public:
    explicit ProductionTextureResidency(Device& device);
    ~ProductionTextureResidency();

    ProductionTextureResidency(const ProductionTextureResidency&) = delete;
    ProductionTextureResidency& operator=(const ProductionTextureResidency&) = delete;

    Status update(const ProductionFramePacket& frame,
                  const ProductionTextureResidencyLimits& limits,
                  ProductionTextureResidencyResult& result);
    std::optional<ProductionTextureBinding> find(
        const ProductionTextureSourceIdentity& source) const;
    Status shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONTEXTURERESIDENCY_H
