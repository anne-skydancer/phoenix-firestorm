/**
 * @file llghialphascenepacket.cpp
 * @brief Deterministic P0e3 alpha-scene packet encoding.
 */
#include "linden_common.h"

#include "ghi/include/llghialphascenepacket.h"
#include "llghihash.h"

#include <bit>
#include <cmath>
#include <limits>

namespace LL::GHI
{
namespace
{
constexpr std::array<std::byte, 8> MAGIC{{
    std::byte{'L'}, std::byte{'L'}, std::byte{'G'}, std::byte{'H'},
    std::byte{'I'}, std::byte{'A'}, std::byte{'3'}, std::byte{'P'}}};
constexpr std::uint64_t MAX_DRAWS = 4ull * 1024ull * 1024ull;
constexpr std::uint64_t MAX_MATERIAL_BYTES = 2ull * 1024ull * 1024ull * 1024ull;

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

void appendU32(std::vector<std::byte>& output, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendU64(std::vector<std::byte>& output, std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendFloat(std::vector<std::byte>& output, float value)
{
    appendU32(output, std::bit_cast<std::uint32_t>(value));
}

class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) : mBytes(bytes) {}

    bool u32(std::uint32_t& value)
    {
        if (mBytes.size() - mOffset < 4) return false;
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= std::to_integer<std::uint32_t>(mBytes[mOffset++]) << shift;
        return true;
    }

    bool u64(std::uint64_t& value)
    {
        if (mBytes.size() - mOffset < 8) return false;
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
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

    bool bytes(std::size_t count, std::span<const std::byte>& value)
    {
        if (count > mBytes.size() - mOffset) return false;
        value = mBytes.subspan(mOffset, count);
        mOffset += count;
        return true;
    }

    bool finished() const { return mOffset == mBytes.size(); }

private:
    std::span<const std::byte> mBytes;
    std::size_t mOffset = 0;
};

bool comparable(ResourceComparability value, ResourceComparability flag)
{
    return (static_cast<std::uint32_t>(value) &
            static_cast<std::uint32_t>(flag)) != 0;
}

bool validFactor(AlphaBlendFactor value)
{
    return value <= AlphaBlendFactor::OneMinusDestinationAlpha;
}

bool validOperation(AlphaBlendOperation value)
{
    return value <= AlphaBlendOperation::Maximum;
}
} // namespace

Status validateAlphaScenePacket(const AlphaScenePacket& packet)
{
    if (packet.version != ALPHA_SCENE_PACKET_VERSION || !packet.frameId ||
        !packet.sceneEpoch || !packet.resourceEpoch || !packet.sourceWidth ||
        !packet.sourceHeight ||
        packet.phase > AlphaViewPhase::MediaSurface ||
        packet.requestedMethod > AlphaMethod::DepthPeeling)
        return invalid("alpha scene identity or enumeration is invalid");
    if (packet.draws.empty() || packet.draws.size() > MAX_DRAWS ||
        packet.draws.size() != packet.materials.draws.size())
        return invalid("alpha draw policy does not match material draws");
    if (packet.ppllPolicy.nodesPerPixel < 1 ||
        packet.ppllPolicy.nodesPerPixel > 32 ||
        packet.ppllPolicy.memoryMiB < 32 ||
        packet.ppllPolicy.memoryMiB > 2048 ||
        packet.ppllPolicy.exactLayersPerPixel < 4 ||
        packet.ppllPolicy.exactLayersPerPixel > 32 ||
        clampAlphaDepthPeelPolicy(packet.depthPeelPolicy) !=
            packet.depthPeelPolicy)
        return invalid("alpha OIT policy is outside its bounded range");
    if (packet.materials.frameId != packet.frameId ||
        packet.materials.sceneEpoch != packet.sceneEpoch ||
        packet.materials.resourceEpoch != packet.resourceEpoch ||
        packet.materials.sourceWidth != packet.sourceWidth ||
        packet.materials.sourceHeight != packet.sourceHeight)
        return invalid("alpha and material packet identities differ");

    std::vector<std::byte> materialBytes;
    if (!encodeMaterialScenePacket(packet.materials, materialBytes))
        return invalid("alpha material payload is invalid");
    if (materialBytes.size() > MAX_MATERIAL_BYTES)
        return invalid("alpha material payload exceeds its bound");

    for (std::size_t index = 0; index < packet.draws.size(); ++index)
    {
        const AlphaSceneDraw& draw = packet.draws[index];
        if (draw.classification > AlphaSubmissionClass::Particle ||
            !validFactor(draw.blend.sourceColor) ||
            !validFactor(draw.blend.destinationColor) ||
            !validFactor(draw.blend.sourceAlpha) ||
            !validFactor(draw.blend.destinationAlpha) ||
            !validOperation(draw.blend.colorOperation) ||
            !validOperation(draw.blend.alphaOperation) ||
            !std::isfinite(draw.minimumAlpha) || draw.minimumAlpha < 0.f ||
            draw.minimumAlpha > 1.f)
            return invalid("alpha draw state is invalid");
        const MaterialSceneDraw& materialDraw = packet.materials.draws[index];
        if (!materialDraw.indexCount || materialDraw.material == NO_RESOURCE ||
            draw.rigged != (materialDraw.skin != NO_RESOURCE))
            return invalid("alpha draw geometry or skin ownership is invalid");
        const MaterialResource& material =
            packet.materials.materials[materialDraw.material];
        const bool mask = draw.classification == AlphaSubmissionClass::Mask;
        if ((mask && material.alphaMode != MaterialAlphaMode::Mask) ||
            (!mask && material.alphaMode != MaterialAlphaMode::Blend) ||
            (!mask && !comparable(material.comparability,
                                   ResourceComparability::AlphaDeferred)))
            return invalid("alpha draw classification disagrees with its material");
    }
    return Status::success();
}

Status encodeAlphaScenePacket(const AlphaScenePacket& packet,
                              std::vector<std::byte>& encoded)
{
    Status status = validateAlphaScenePacket(packet);
    if (!status) return status;
    std::vector<std::byte> materialBytes;
    status = encodeMaterialScenePacket(packet.materials, materialBytes);
    if (!status) return status;

    encoded.clear();
    encoded.insert(encoded.end(), MAGIC.begin(), MAGIC.end());
    appendU32(encoded, packet.version);
    appendU32(encoded, 0);
    appendU64(encoded, packet.frameId);
    appendU64(encoded, packet.sceneEpoch);
    appendU64(encoded, packet.resourceEpoch);
    appendU32(encoded, packet.sourceWidth);
    appendU32(encoded, packet.sourceHeight);
    appendU32(encoded, static_cast<std::uint32_t>(packet.phase));
    appendU32(encoded, static_cast<std::uint32_t>(packet.requestedMethod));
    appendU32(encoded, packet.transientLoad ? 1u : 0u);
    appendU32(encoded, packet.ppllPolicy.nodesPerPixel);
    appendU32(encoded, packet.ppllPolicy.memoryMiB);
    appendU32(encoded, packet.ppllPolicy.exactLayersPerPixel);
    appendU32(encoded, packet.depthPeelPolicy.maximumLayers);
    appendU32(encoded, packet.depthPeelPolicy.submissionBudgetMilliseconds);
    appendU64(encoded, packet.draws.size());
    appendU64(encoded, materialBytes.size());
    for (const AlphaSceneDraw& draw : packet.draws)
    {
        appendU32(encoded, static_cast<std::uint32_t>(draw.classification));
        appendU32(encoded, draw.rigged ? 1u : 0u);
        appendU32(encoded, draw.fullbright ? 1u : 0u);
        appendU32(encoded, draw.emissive ? 1u : 0u);
        appendU32(encoded, static_cast<std::uint32_t>(draw.blend.sourceColor));
        appendU32(encoded, static_cast<std::uint32_t>(draw.blend.destinationColor));
        appendU32(encoded, static_cast<std::uint32_t>(draw.blend.sourceAlpha));
        appendU32(encoded, static_cast<std::uint32_t>(draw.blend.destinationAlpha));
        appendU32(encoded, static_cast<std::uint32_t>(draw.blend.colorOperation));
        appendU32(encoded, static_cast<std::uint32_t>(draw.blend.alphaOperation));
        appendFloat(encoded, draw.minimumAlpha);
    }
    encoded.insert(encoded.end(), materialBytes.begin(), materialBytes.end());
    return Status::success();
}

Status decodeAlphaScenePacket(std::span<const std::byte> encoded,
                              AlphaScenePacket& packet)
{
    packet = {};
    if (encoded.size() < MAGIC.size() ||
        !std::equal(MAGIC.begin(), MAGIC.end(), encoded.begin()))
        return invalid("alpha scene packet has an invalid magic");
    Reader reader(encoded.subspan(MAGIC.size()));
    AlphaScenePacket output;
    std::uint32_t reserved = 0, phase = 0, method = 0, transient = 0;
    std::uint64_t drawCount = 0, materialSize = 0;
    if (!reader.u32(output.version) || !reader.u32(reserved) || reserved ||
        !reader.u64(output.frameId) || !reader.u64(output.sceneEpoch) ||
        !reader.u64(output.resourceEpoch) || !reader.u32(output.sourceWidth) ||
        !reader.u32(output.sourceHeight) || !reader.u32(phase) ||
        !reader.u32(method) || !reader.u32(transient) || transient > 1 ||
        !reader.u32(output.ppllPolicy.nodesPerPixel) ||
        !reader.u32(output.ppllPolicy.memoryMiB) ||
        !reader.u32(output.ppllPolicy.exactLayersPerPixel) ||
        !reader.u32(output.depthPeelPolicy.maximumLayers) ||
        !reader.u32(output.depthPeelPolicy.submissionBudgetMilliseconds) ||
        !reader.u64(drawCount) || !reader.u64(materialSize) ||
        drawCount > MAX_DRAWS || materialSize > MAX_MATERIAL_BYTES ||
        drawCount > std::numeric_limits<std::size_t>::max() ||
        materialSize > std::numeric_limits<std::size_t>::max())
        return invalid("alpha scene packet header is invalid or truncated");
    output.phase = static_cast<AlphaViewPhase>(phase);
    output.requestedMethod = static_cast<AlphaMethod>(method);
    output.transientLoad = transient != 0;
    output.draws.resize(static_cast<std::size_t>(drawCount));
    for (AlphaSceneDraw& draw : output.draws)
    {
        std::uint32_t classification = 0, rigged = 0, fullbright = 0,
            emissive = 0, sourceColor = 0, destinationColor = 0,
            sourceAlpha = 0, destinationAlpha = 0, colorOperation = 0,
            alphaOperation = 0;
        if (!reader.u32(classification) || !reader.u32(rigged) ||
            !reader.u32(fullbright) || !reader.u32(emissive) || rigged > 1 ||
            fullbright > 1 || emissive > 1 || !reader.u32(sourceColor) ||
            !reader.u32(destinationColor) || !reader.u32(sourceAlpha) ||
            !reader.u32(destinationAlpha) || !reader.u32(colorOperation) ||
            !reader.u32(alphaOperation) || !reader.floating(draw.minimumAlpha))
            return invalid("alpha scene draw is invalid or truncated");
        draw.classification = static_cast<AlphaSubmissionClass>(classification);
        draw.rigged = rigged != 0;
        draw.fullbright = fullbright != 0;
        draw.emissive = emissive != 0;
        draw.blend.sourceColor = static_cast<AlphaBlendFactor>(sourceColor);
        draw.blend.destinationColor =
            static_cast<AlphaBlendFactor>(destinationColor);
        draw.blend.sourceAlpha = static_cast<AlphaBlendFactor>(sourceAlpha);
        draw.blend.destinationAlpha =
            static_cast<AlphaBlendFactor>(destinationAlpha);
        draw.blend.colorOperation =
            static_cast<AlphaBlendOperation>(colorOperation);
        draw.blend.alphaOperation =
            static_cast<AlphaBlendOperation>(alphaOperation);
    }
    std::span<const std::byte> materialBytes;
    if (!reader.bytes(static_cast<std::size_t>(materialSize), materialBytes) ||
        !reader.finished())
        return invalid("alpha material payload is truncated or has trailing data");
    Status status = decodeMaterialScenePacket(materialBytes, output.materials);
    if (!status) return invalid("alpha material payload cannot be decoded");
    status = validateAlphaScenePacket(output);
    if (!status) return status;
    packet = std::move(output);
    return Status::success();
}

std::string alphaScenePacketSha256(const AlphaScenePacket& packet)
{
    std::vector<std::byte> encoded;
    if (!encodeAlphaScenePacket(packet, encoded)) return {};
    return sha256(encoded);
}
} // namespace LL::GHI
