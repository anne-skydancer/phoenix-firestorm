/**
 * @file llghiopaquepacketconsumer.cpp
 * @brief Bounded, transfer-only consumption of a live opaque scene packet.
 */

#include "linden_common.h"

#include "ghi/include/llghiopaquepacketconsumer.h"

#include "ghi/core/llghihash.h"
#include "ghi/include/llghidevice.h"

#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace LL::GHI
{
namespace
{
constexpr std::uint64_t align16(std::uint64_t value)
{
    return (value + 15ull) & ~15ull;
}

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}
} // namespace

Status consumeOpaquePacketTransfer(
    Device& device,
    const OpaqueScenePacket& packet,
    const OpaquePacketTransferLimits& limits,
    OpaquePacketTransferResult& result)
{
    result = {};
    if (packet.vertices.empty() || packet.indices.empty() || packet.draws.empty())
        return invalid("live opaque packet contains no transferable draws");
    if (!limits.maxVertices || !limits.maxIndices || !limits.maxDraws ||
        !limits.maxUploadBytes)
        return invalid("live opaque packet transfer limits must be nonzero");
    if (packet.vertices.size() > limits.maxVertices ||
        packet.indices.size() > limits.maxIndices ||
        packet.draws.size() > limits.maxDraws)
        return invalid("live opaque packet exceeds transfer element limits");

    std::vector<std::byte> encoded;
    Status status = encodeOpaqueScenePacket(packet, encoded);
    if (!status) return status;

    const std::uint64_t vertexBytes =
        packet.vertices.size() * sizeof(OpaqueSceneVertex);
    const std::uint64_t indexBytes =
        packet.indices.size() * sizeof(std::uint32_t);
    const std::uint64_t transformBytes =
        packet.draws.size() * sizeof(packet.draws.front().transform);
    const std::uint64_t vertexOffset = 0;
    const std::uint64_t indexOffset = align16(vertexBytes);
    const std::uint64_t transformOffset = align16(indexOffset + indexBytes);
    if (transformOffset > std::numeric_limits<std::uint64_t>::max() - transformBytes)
        return invalid("live opaque packet upload size overflow");
    const std::uint64_t uploadBytes = transformOffset + transformBytes;
    if (uploadBytes > limits.maxUploadBytes)
        return invalid("live opaque packet exceeds transfer byte limit");
    const RendererCapabilities& capabilities = device.capabilities();
    if (uploadBytes > capabilities.maxBufferSize ||
        vertexBytes > capabilities.maxBufferSize ||
        indexBytes > capabilities.maxBufferSize ||
        transformBytes > capabilities.maxBufferSize)
        return Status::failure(StatusCode::Unsupported,
                               "live opaque packet exceeds device buffer limits");
    if (transformBytes > capabilities.maxUniformBufferSize)
        return Status::failure(StatusCode::Unsupported,
                               "live opaque transforms exceed the uniform-buffer limit");

    std::vector<std::byte> uploadData(static_cast<std::size_t>(uploadBytes));
    std::memcpy(uploadData.data() + vertexOffset, packet.vertices.data(),
                static_cast<std::size_t>(vertexBytes));
    std::memcpy(uploadData.data() + indexOffset, packet.indices.data(),
                static_cast<std::size_t>(indexBytes));
    for (std::size_t draw = 0; draw < packet.draws.size(); ++draw)
    {
        std::memcpy(uploadData.data() + transformOffset +
                        draw * sizeof(packet.draws.front().transform),
                    packet.draws[draw].transform.data(),
                    sizeof(packet.draws.front().transform));
    }

    std::array<BufferHandle, 4> buffers{};
    auto destroyBuffers = [&]()
    {
        Status first = Status::success();
        for (BufferHandle& handle : buffers)
        {
            if (!handle) continue;
            const Status destroyed = device.destroy(handle);
            if (!destroyed && first) first = destroyed;
            handle = {};
        }
        return first;
    };

    buffers[0] = device.createBuffer(
        {uploadBytes, ResourceUsage::TransferSource, MemoryClass::Upload}, status);
    if (status)
        buffers[1] = device.createBuffer(
            {vertexBytes, ResourceUsage::Vertex | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
    if (status)
        buffers[2] = device.createBuffer(
            {indexBytes, ResourceUsage::Index | ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
    if (status)
        buffers[3] = device.createBuffer(
            {transformBytes, ResourceUsage::Uniform |
                                 ResourceUsage::TransferDestination,
             MemoryClass::DeviceLocal}, status);
    if (!status)
    {
        destroyBuffers();
        return status;
    }
    status = device.writeBuffer(buffers[0], 0, uploadData);
    if (!status)
    {
        destroyBuffers();
        return status;
    }

    CommandContext& commands = device.commandContext();
    status = commands.beginFrame();
    if (!status)
    {
        destroyBuffers();
        return status;
    }
    const std::array<BufferCopyRegion, 1> vertexCopy{{
        {vertexOffset, 0, vertexBytes}}};
    const std::array<BufferCopyRegion, 1> indexCopy{{
        {indexOffset, 0, indexBytes}}};
    const std::array<BufferCopyRegion, 1> transformCopy{{
        {transformOffset, 0, transformBytes}}};
    status = commands.copyBuffer(buffers[0], buffers[1], vertexCopy);
    if (status) status = commands.copyBuffer(buffers[0], buffers[2], indexCopy);
    if (status) status = commands.copyBuffer(buffers[0], buffers[3], transformCopy);
    const Status endStatus = commands.endFrame();
    if (!endStatus) return endStatus;

    const Status destroyStatus = destroyBuffers();
    if (!status) return status;
    if (!destroyStatus) return destroyStatus;

    result.frameId = packet.frameId;
    result.uploadBytes = uploadBytes;
    result.encodedBytes = encoded.size();
    result.vertices = static_cast<std::uint32_t>(packet.vertices.size());
    result.indices = static_cast<std::uint32_t>(packet.indices.size());
    result.draws = static_cast<std::uint32_t>(packet.draws.size());
    result.packetSha256 = sha256(encoded);
    return Status::success();
}

} // namespace LL::GHI
