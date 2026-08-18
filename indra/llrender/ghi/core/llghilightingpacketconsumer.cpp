/**
 * @file llghilightingpacketconsumer.cpp
 * @brief Bounded I7a transfer of live lighting state to a native backend.
 */

#include "linden_common.h"

#include "ghi/include/llghilightingpacketconsumer.h"

#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <array>
#include <algorithm>

namespace LL::GHI
{

Status consumeLightingPacketTransfer(
    Device& device,
    const LightingScenePacket& packet,
    const LightingPacketTransferLimits& limits,
    LightingPacketTransferResult& result)
{
    result = {};
    if (!limits.maxLocalLights || !limits.maxUploadBytes)
        return Status::failure(StatusCode::InvalidArgument,
                               "lighting transfer limits must be nonzero");
    if (packet.localLights.size() > limits.maxLocalLights)
        return Status::failure(StatusCode::InvalidArgument,
                               "lighting packet exceeds the local-light limit");

    std::vector<std::byte> encoded;
    Status status = encodeLightingScenePacket(packet, encoded);
    if (!status) return status;
    if (encoded.empty() || encoded.size() > limits.maxUploadBytes ||
        encoded.size() > device.capabilities().maxBufferSize)
        return Status::failure(StatusCode::Unsupported,
                               "lighting packet exceeds the upload byte limit");

    BufferHandle upload = device.createBuffer(
        {encoded.size(), ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    BufferHandle storage;
    if (status)
        storage = device.createBuffer(
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
        device.destroy(storage);
        device.destroy(upload);
        return status;
    }

    CommandContext& commands = device.commandContext();
    if ((status = commands.beginFrame()))
    {
        const std::array<BufferCopyRegion, 1> copy{{
            {0, 0, static_cast<std::uint64_t>(encoded.size())}}};
        status = commands.copyBuffer(upload, storage, copy);
        const Status endStatus = commands.endFrame();
        if (status && !endStatus) status = endStatus;
    }
    const Status storageStatus = device.destroy(storage);
    const Status uploadStatus = device.destroy(upload);
    if (!status) return status;
    if (!storageStatus) return storageStatus;
    if (!uploadStatus) return uploadStatus;

    result.frameId = packet.frameId;
    result.sceneEpoch = packet.sceneEpoch;
    result.resourceEpoch = packet.resourceEpoch;
    result.uploadBytes = encoded.size();
    result.localLights = static_cast<std::uint32_t>(packet.localLights.size());
    result.projectorLights = static_cast<std::uint32_t>(std::count_if(
        packet.localLights.begin(), packet.localLights.end(),
        [](const LocalLightRecord& light)
        { return light.kind == LocalLightKind::Projector; }));
    result.shadowCascades = packet.shadows.directionalCascadeCount;
    result.packetSha256 = sha256(encoded);
    return Status::success();
}

} // namespace LL::GHI
