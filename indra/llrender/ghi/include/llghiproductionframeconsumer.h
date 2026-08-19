/**
 * @file llghiproductionframeconsumer.h
 * @brief Bounded transfer-only consumption of an assembled production frame.
 */

#ifndef LL_LLGHIPRODUCTIONFRAMECONSUMER_H
#define LL_LLGHIPRODUCTIONFRAMECONSUMER_H

#include "llghiproductionframepacket.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace LL::GHI
{

class Device;

struct ProductionFrameTransferLimits
{
    std::size_t maxOpaqueDraws = 256;
    std::size_t maxMaterialDraws = 256;
    std::size_t maxTerrainDraws = 128;
    std::size_t maxVertices = 262144;
    std::size_t maxIndices = 786432;
    std::size_t maxUniqueResources = 4096;
    std::uint64_t maxDecodedTextureBytes = 32ull * 1024ull * 1024ull;
    std::uint64_t maxEncodedBytes = 64ull * 1024ull * 1024ull;
};

struct ProductionFrameTransferResult
{
    std::uint64_t frameId = 0;
    std::uint64_t assemblyEpoch = 0;
    std::uint64_t uploadBytes = 0;
    ProductionFramePassMask passes = 0;
    ProductionFrameResourceSummary resources;
    std::string packetSha256;
};

// Serializes one coherent frame and copies it into backend-local storage in a
// single GHI frame. It records no rendering, creates no image/surface/swapchain,
// and cannot present. Later I8 gates retain individual resources and execute
// the same frame graph.
Status consumeProductionFrameTransfer(
    Device& device,
    const ProductionFramePacket& packet,
    const ProductionFrameTransferLimits& limits,
    ProductionFrameTransferResult& result);

} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONFRAMECONSUMER_H
