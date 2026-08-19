/**
 * @file llghiproductionframeconsumer.cpp
 * @brief Bounded I8a transfer of one assembled production frame.
 */

#include "linden_common.h"

#include "ghi/include/llghiproductionframeconsumer.h"

#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <array>

namespace LL::GHI
{

Status consumeProductionFrameTransfer(
    Device& device,
    const ProductionFramePacket& packet,
    const ProductionFrameTransferLimits& limits,
    ProductionFrameTransferResult& result)
{
    result = {};
    if (!limits.maxOpaqueDraws || !limits.maxMaterialDraws ||
        !limits.maxTerrainDraws ||
        !limits.maxVertices || !limits.maxIndices ||
        !limits.maxUniqueResources || !limits.maxDecodedTextureBytes ||
        !limits.maxEncodedBytes)
        return Status::failure(StatusCode::InvalidArgument,
                               "production frame transfer limits must be nonzero");

    ProductionFrameResourceSummary resources;
    Status status = validateProductionFramePacket(packet, &resources);
    if (!status) return status;
    if (resources.opaqueDraws > limits.maxOpaqueDraws ||
        resources.materialDraws > limits.maxMaterialDraws ||
        resources.terrainDraws > limits.maxTerrainDraws ||
        resources.vertices > limits.maxVertices ||
        resources.indices > limits.maxIndices ||
        resources.uniqueResources > limits.maxUniqueResources)
        return Status::failure(StatusCode::InvalidArgument,
                               "production frame exceeds transfer element limits");
    if (resources.decodedTextureBytes > limits.maxDecodedTextureBytes)
        return Status::failure(StatusCode::Unsupported,
                               "production frame exceeds decoded texture byte limit");

    std::vector<std::byte> encoded;
    status = encodeProductionFramePacket(packet, encoded);
    if (!status) return status;
    if (encoded.empty() || encoded.size() > limits.maxEncodedBytes ||
        encoded.size() > device.capabilities().maxBufferSize)
        return Status::failure(StatusCode::Unsupported,
                               "production frame exceeds encoded transfer limit");

    BufferHandle upload = device.createBuffer(
        {encoded.size(), ResourceUsage::TransferSource, MemoryClass::Upload},
        status);
    BufferHandle resident;
    if (status)
        resident = device.createBuffer(
            {encoded.size(), ResourceUsage::Storage |
                                 ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
    if (!status)
    {
        if (upload) device.destroy(upload);
        return status;
    }
    if (!(status = device.writeBuffer(upload, 0, encoded)))
    {
        device.destroy(resident);
        device.destroy(upload);
        return status;
    }

    CommandContext& commands = device.commandContext();
    if ((status = commands.beginFrame()))
    {
        const std::array<BufferCopyRegion, 1> copy{{
            {0, 0, static_cast<std::uint64_t>(encoded.size())}}};
        status = commands.copyBuffer(upload, resident, copy);
        const Status endStatus = commands.endFrame();
        if (status && !endStatus) status = endStatus;
    }
    const Status residentStatus = device.destroy(resident);
    const Status uploadStatus = device.destroy(upload);
    if (!status) return status;
    if (!residentStatus) return residentStatus;
    if (!uploadStatus) return uploadStatus;

    result.frameId = packet.frameId;
    result.assemblyEpoch = packet.assemblyEpoch;
    result.uploadBytes = encoded.size();
    result.passes = packet.passes;
    result.resources = resources;
    result.packetSha256 = sha256(encoded);
    return Status::success();
}

} // namespace LL::GHI
