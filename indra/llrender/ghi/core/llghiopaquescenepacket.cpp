/**
 * @file llghiopaquescenepacket.cpp
 * @brief Deterministic encoding for post-cull opaque scene packets.
 */

#include "linden_common.h"

#include "ghi/include/llghiopaquescenepacket.h"
#include "llghihash.h"

#include <bit>
#include <limits>

namespace LL::GHI
{
namespace
{
constexpr std::array<std::byte, 8> MAGIC{{
    std::byte{'L'}, std::byte{'L'}, std::byte{'G'}, std::byte{'H'},
    std::byte{'I'}, std::byte{'O'}, std::byte{'P'}, std::byte{'4'}}};
constexpr std::uint64_t MAX_VERTICES = 16ull * 1024ull * 1024ull;
constexpr std::uint64_t MAX_INDICES = 64ull * 1024ull * 1024ull;
constexpr std::uint64_t MAX_DRAWS = 4ull * 1024ull * 1024ull;

void appendU32(std::vector<std::byte>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendU64(std::vector<std::byte>& bytes, std::uint64_t value)
{
    for (unsigned shift = 0; shift != 64; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendFloat(std::vector<std::byte>& bytes, float value)
{
    appendU32(bytes, std::bit_cast<std::uint32_t>(value));
}

class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) : mBytes(bytes) {}

    bool bytes(std::span<std::byte> destination)
    {
        if (destination.size() > mBytes.size() - mOffset) return false;
        std::copy_n(mBytes.begin() + static_cast<std::ptrdiff_t>(mOffset),
                    destination.size(), destination.begin());
        mOffset += destination.size();
        return true;
    }

    bool u32(std::uint32_t& value)
    {
        if (4 > mBytes.size() - mOffset) return false;
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
            value |= std::to_integer<std::uint32_t>(mBytes[mOffset++]) << shift;
        return true;
    }

    bool u64(std::uint64_t& value)
    {
        if (8 > mBytes.size() - mOffset) return false;
        value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8)
            value |= std::to_integer<std::uint64_t>(mBytes[mOffset++]) << shift;
        return true;
    }

    bool floating(float& value)
    {
        std::uint32_t bits = 0;
        if (!u32(bits)) return false;
        value = std::bit_cast<float>(bits);
        return true;
    }

    bool finished() const { return mOffset == mBytes.size(); }

private:
    std::span<const std::byte> mBytes;
    std::size_t mOffset = 0;
};

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

bool validPacket(const OpaqueScenePacket& packet)
{
    if (packet.version != OPAQUE_SCENE_PACKET_VERSION ||
        packet.vertices.size() > MAX_VERTICES || packet.indices.size() > MAX_INDICES ||
        packet.draws.size() > MAX_DRAWS)
        return false;
    for (std::uint32_t index : packet.indices)
        if (index >= packet.vertices.size()) return false;
    for (const auto& draw : packet.draws)
        if (draw.firstIndex > packet.indices.size() ||
            draw.indexCount > packet.indices.size() - draw.firstIndex ||
            draw.indexCount % 3 != 0)
            return false;
    return true;
}
} // namespace

Status encodeOpaqueScenePacket(const OpaqueScenePacket& packet,
                               std::vector<std::byte>& encoded)
{
    if (!validPacket(packet)) return invalid("invalid opaque scene packet");
    encoded.clear();
    encoded.reserve(128 + packet.vertices.size() * 16 + packet.indices.size() * 4 +
                    packet.draws.size() * 84);
    encoded.insert(encoded.end(), MAGIC.begin(), MAGIC.end());
    appendU32(encoded, packet.version);
    appendU32(encoded, packet.sourceWidth);
    appendU32(encoded, packet.sourceHeight);
    appendU64(encoded, packet.frameId);
    appendU64(encoded, packet.sceneEpoch);
    appendU32(encoded, packet.productionOcclusionEnabled ? 1u : 0u);
    appendU32(encoded, 0); // reserved
    for (std::uint64_t value : {
        packet.statistics.submittedDraws, packet.statistics.submittedTriangles,
        packet.statistics.capturedDraws, packet.statistics.capturedTriangles,
        packet.statistics.skippedRiggedDraws, packet.statistics.skippedMaterialDraws,
        packet.statistics.invalidDraws})
        appendU64(encoded, value);
    appendU64(encoded, packet.vertices.size());
    appendU64(encoded, packet.indices.size());
    appendU64(encoded, packet.draws.size());
    for (const auto& vertex : packet.vertices)
    {
        for (float component : vertex.position) appendFloat(encoded, component);
        for (std::uint8_t component : vertex.color)
            encoded.push_back(static_cast<std::byte>(component));
    }
    for (std::uint32_t index : packet.indices) appendU32(encoded, index);
    for (const auto& draw : packet.draws)
    {
        appendU32(encoded, draw.firstIndex);
        appendU32(encoded, draw.indexCount);
        for (float component : draw.transform) appendFloat(encoded, component);
        appendU64(encoded, draw.semanticId);
        appendU32(encoded, static_cast<std::uint32_t>(draw.flags));
    }
    return Status::success();
}

Status decodeOpaqueScenePacket(std::span<const std::byte> encoded,
                               OpaqueScenePacket& packet)
{
    Reader reader(encoded);
    std::array<std::byte, 8> magic{};
    std::uint32_t occlusion = 0;
    std::uint32_t reserved = 0;
    std::uint64_t vertexCount = 0, indexCount = 0, drawCount = 0;
    OpaqueScenePacket decoded;
    if (!reader.bytes(magic) || magic != MAGIC || !reader.u32(decoded.version) ||
        !reader.u32(decoded.sourceWidth) || !reader.u32(decoded.sourceHeight) ||
        !reader.u64(decoded.frameId) || !reader.u64(decoded.sceneEpoch) ||
        !reader.u32(occlusion) || !reader.u32(reserved))
        return invalid("truncated or unrecognized opaque scene packet header");
    decoded.productionOcclusionEnabled = occlusion != 0;
    for (std::uint64_t* value : {
        &decoded.statistics.submittedDraws, &decoded.statistics.submittedTriangles,
        &decoded.statistics.capturedDraws, &decoded.statistics.capturedTriangles,
        &decoded.statistics.skippedRiggedDraws, &decoded.statistics.skippedMaterialDraws,
        &decoded.statistics.invalidDraws})
        if (!reader.u64(*value)) return invalid("truncated opaque scene statistics");
    if (!reader.u64(vertexCount) || !reader.u64(indexCount) || !reader.u64(drawCount) ||
        vertexCount > MAX_VERTICES || indexCount > MAX_INDICES || drawCount > MAX_DRAWS)
        return invalid("invalid opaque scene packet counts");
    decoded.vertices.resize(static_cast<std::size_t>(vertexCount));
    decoded.indices.resize(static_cast<std::size_t>(indexCount));
    decoded.draws.resize(static_cast<std::size_t>(drawCount));
    for (auto& vertex : decoded.vertices)
    {
        for (float& component : vertex.position)
            if (!reader.floating(component)) return invalid("truncated opaque vertices");
        std::array<std::byte, 4> color{};
        if (!reader.bytes(color)) return invalid("truncated opaque vertex colors");
        for (std::size_t i = 0; i < color.size(); ++i)
            vertex.color[i] = std::to_integer<std::uint8_t>(color[i]);
    }
    for (auto& index : decoded.indices)
        if (!reader.u32(index)) return invalid("truncated opaque indices");
    for (auto& draw : decoded.draws)
    {
        std::uint32_t flags = 0;
        if (!reader.u32(draw.firstIndex) || !reader.u32(draw.indexCount))
            return invalid("truncated opaque draw records");
        for (float& component : draw.transform)
            if (!reader.floating(component)) return invalid("truncated opaque transforms");
        if (!reader.u64(draw.semanticId) || !reader.u32(flags))
            return invalid("truncated opaque draw metadata");
        draw.flags = static_cast<OpaqueSceneDrawFlags>(flags);
    }
    if (!reader.finished() || !validPacket(decoded))
        return invalid("opaque scene packet has trailing or invalid data");
    packet = std::move(decoded);
    return Status::success();
}

std::string opaqueScenePacketSha256(const OpaqueScenePacket& packet)
{
    std::vector<std::byte> encoded;
    if (!encodeOpaqueScenePacket(packet, encoded)) return {};
    return sha256(encoded);
}

} // namespace LL::GHI
