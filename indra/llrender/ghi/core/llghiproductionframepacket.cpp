/**
 * @file llghiproductionframepacket.cpp
 * @brief Deterministic I8a production-frame packet encoding and validation.
 */

#include "linden_common.h"

#include "ghi/include/llghiproductionframepacket.h"
#include "llghihash.h"

#include <array>
#include <limits>
#include <set>
#include <utility>

namespace LL::GHI
{
namespace
{
constexpr std::array<std::byte, 8> MAGIC{{
    std::byte{'L'}, std::byte{'L'}, std::byte{'G'}, std::byte{'H'},
    std::byte{'I'}, std::byte{'F'}, std::byte{'8'}, std::byte{'A'}}};
constexpr std::uint64_t MAX_CHILD_BYTES = 2ull * 1024ull * 1024ull * 1024ull;
constexpr ProductionFramePassMask KNOWN_PASSES =
    productionFramePassBit(ProductionFramePass::OpaqueGBuffer) |
    productionFramePassBit(ProductionFramePass::MaterialGBuffer) |
    productionFramePassBit(ProductionFramePass::TerrainGBuffer) |
    productionFramePassBit(ProductionFramePass::DirectionalShadow) |
    productionFramePassBit(ProductionFramePass::ProjectorShadow) |
    productionFramePassBit(ProductionFramePass::DeferredLighting) |
    productionFramePassBit(ProductionFramePass::ProjectorLighting);
constexpr ProductionFramePassMask REQUIRED_PASSES =
    productionFramePassBit(ProductionFramePass::OpaqueGBuffer) |
    productionFramePassBit(ProductionFramePass::MaterialGBuffer) |
    productionFramePassBit(ProductionFramePass::TerrainGBuffer) |
    productionFramePassBit(ProductionFramePass::DeferredLighting);

void appendU32(std::vector<std::byte>& out, std::uint32_t value)
{
    for (unsigned shift = 0; shift != 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void appendU64(std::vector<std::byte>& out, std::uint64_t value)
{
    for (unsigned shift = 0; shift != 64; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) : mBytes(bytes) {}

    bool bytes(std::span<std::byte> output)
    {
        if (output.size() > mBytes.size() - mOffset) return false;
        std::copy_n(mBytes.begin() + static_cast<std::ptrdiff_t>(mOffset),
                    output.size(), output.begin());
        mOffset += output.size();
        return true;
    }

    bool view(std::uint64_t size, std::span<const std::byte>& output)
    {
        if (size > mBytes.size() - mOffset) return false;
        output = mBytes.subspan(mOffset, static_cast<std::size_t>(size));
        mOffset += static_cast<std::size_t>(size);
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

    bool finished() const { return mOffset == mBytes.size(); }

private:
    std::span<const std::byte> mBytes;
    std::size_t mOffset = 0;
};

Status invalid(const char* message)
{
    return Status::failure(StatusCode::InvalidArgument, message);
}

bool zeroDigest(const ResourceDigest& digest)
{
    return std::all_of(digest.begin(), digest.end(),
                       [](std::byte value) { return value == std::byte{0}; });
}

void addTextureResource(
    const MaterialTextureResource& texture, std::uint32_t domain,
    std::set<std::pair<std::uint32_t, ResourceDigest>>& identities,
    ProductionFrameResourceSummary& summary)
{
    const ResourceDigest& identity = zeroDigest(texture.contentIdentity)
        ? texture.sourceIdentity : texture.contentIdentity;
    identities.emplace(domain, identity);
    summary.decodedTextureBytes += texture.decodedPixels.size();
}

Status buildSummary(const ProductionFramePacket& packet,
                    ProductionFrameResourceSummary& summary)
{
    summary = {};
    std::set<std::pair<std::uint32_t, ResourceDigest>> identities;
    summary.materialTextures =
        static_cast<std::uint32_t>(packet.materials.textures.size());
    summary.terrainTextures =
        static_cast<std::uint32_t>(packet.terrain.textures.size());
    summary.projectorTextures =
        static_cast<std::uint32_t>(packet.lighting.projectorTextures.size());
    summary.materials =
        static_cast<std::uint32_t>(packet.materials.materials.size());
    summary.skins = static_cast<std::uint32_t>(packet.materials.skins.size());
    summary.terrainRegions =
        static_cast<std::uint32_t>(packet.terrain.regions.size());
    summary.opaqueDraws =
        static_cast<std::uint32_t>(packet.opaque.draws.size());
    summary.materialDraws =
        static_cast<std::uint32_t>(packet.materials.draws.size());
    summary.terrainDraws =
        static_cast<std::uint32_t>(packet.terrain.draws.size());

    const std::uint64_t vertices = packet.opaque.vertices.size() +
                                   packet.materials.vertices.size() +
                                   packet.terrain.vertices.size();
    const std::uint64_t indices = packet.opaque.indices.size() +
                                  packet.materials.indices.size() +
                                  packet.terrain.indices.size();
    if (vertices > std::numeric_limits<std::uint32_t>::max() ||
        indices > std::numeric_limits<std::uint32_t>::max())
        return invalid("production frame geometry count overflow");
    summary.vertices = static_cast<std::uint32_t>(vertices);
    summary.indices = static_cast<std::uint32_t>(indices);

    for (const auto& texture : packet.materials.textures)
        addTextureResource(texture, 1, identities, summary);
    for (const auto& material : packet.materials.materials)
        identities.emplace(2, material.identity);
    for (const auto& skin : packet.materials.skins)
        identities.emplace(3, skin.identity);
    for (const auto& texture : packet.terrain.textures)
        addTextureResource(texture, 4, identities, summary);
    for (const auto& region : packet.terrain.regions)
    {
        identities.emplace(5, region.identity);
        for (const auto& layer : region.layers)
            identities.emplace(6, layer.identity);
    }
    for (const auto& texture : packet.lighting.projectorTextures)
    {
        identities.emplace(7, texture.contentIdentity);
        summary.decodedTextureBytes += texture.decodedPixels.size();
    }
    if (identities.size() > std::numeric_limits<std::uint32_t>::max())
        return invalid("production frame resource count overflow");
    summary.uniqueResources = static_cast<std::uint32_t>(identities.size());
    return Status::success();
}
} // namespace

Status validateProductionFramePacket(
    const ProductionFramePacket& packet,
    ProductionFrameResourceSummary* summary)
{
    if (packet.version != PRODUCTION_FRAME_PACKET_VERSION || !packet.frameId ||
        !packet.assemblyEpoch || !packet.sourceWidth || !packet.sourceHeight)
        return invalid("invalid production frame packet header");
    if ((packet.passes & ~KNOWN_PASSES) != 0 ||
        (packet.passes & REQUIRED_PASSES) != REQUIRED_PASSES)
        return invalid("production frame pass mask is incomplete or unknown");
    if (packet.opaque.frameId != packet.frameId ||
        packet.materials.frameId != packet.frameId ||
        packet.terrain.frameId != packet.frameId ||
        packet.lighting.frameId != packet.frameId)
        return invalid("production frame child packets do not share a frame");
    if (packet.opaque.sourceWidth != packet.sourceWidth ||
        packet.opaque.sourceHeight != packet.sourceHeight ||
        packet.materials.sourceWidth != packet.sourceWidth ||
        packet.materials.sourceHeight != packet.sourceHeight ||
        packet.terrain.sourceWidth != packet.sourceWidth ||
        packet.terrain.sourceHeight != packet.sourceHeight ||
        packet.lighting.sourceWidth != packet.sourceWidth ||
        packet.lighting.sourceHeight != packet.sourceHeight)
        return invalid("production frame child extents do not match");
    if (packet.opaque.draws.empty() || packet.materials.draws.empty() ||
        packet.terrain.draws.empty())
        return invalid("production frame requires opaque, material, and terrain draws");

    if (productionFrameHasPass(packet.passes,
                               ProductionFramePass::DirectionalShadow) &&
        (!packet.lighting.shadows.enabled ||
         !packet.lighting.shadows.directionalCascadeCount))
        return invalid("directional shadow pass lacks directional shadow state");
    if (productionFrameHasPass(packet.passes,
                               ProductionFramePass::ProjectorLighting))
    {
        const bool projector = std::any_of(
            packet.lighting.localLights.begin(),
            packet.lighting.localLights.end(),
            [](const LocalLightRecord& light)
            { return light.kind == LocalLightKind::Projector; });
        if (!projector || packet.lighting.projectorTextures.empty())
            return invalid("projector lighting pass lacks projector resources");
    }
    if (productionFrameHasPass(packet.passes,
                               ProductionFramePass::ProjectorShadow) &&
        (!productionFrameHasPass(packet.passes,
                                 ProductionFramePass::ProjectorLighting) ||
         !packet.lighting.shadows.enabled ||
         !packet.lighting.shadows.projectorShadowCount))
        return invalid("projector shadow pass lacks projector shadow state");

    // Child encoders remain the single validation authority for each adopted
    // production stream. Encoding here also rejects malformed nested resource
    // references before an assembled packet can cross the GHI boundary.
    std::vector<std::byte> child;
    Status status = encodeOpaqueScenePacket(packet.opaque, child);
    if (!status) return status;
    status = encodeMaterialScenePacket(packet.materials, child);
    if (!status) return status;
    status = encodeTerrainScenePacket(packet.terrain, child);
    if (!status) return status;
    status = encodeLightingScenePacket(packet.lighting, child);
    if (!status) return status;

    ProductionFrameResourceSummary built;
    status = buildSummary(packet, built);
    if (!status) return status;
    if (summary) *summary = built;
    return Status::success();
}

Status encodeProductionFramePacket(const ProductionFramePacket& packet,
                                   std::vector<std::byte>& encoded)
{
    Status status = validateProductionFramePacket(packet);
    if (!status) return status;

    std::vector<std::byte> opaqueBytes, materialBytes, terrainBytes, lightingBytes;
    if (!(status = encodeOpaqueScenePacket(packet.opaque, opaqueBytes)) ||
        !(status = encodeMaterialScenePacket(packet.materials, materialBytes)) ||
        !(status = encodeTerrainScenePacket(packet.terrain, terrainBytes)) ||
        !(status = encodeLightingScenePacket(packet.lighting, lightingBytes)))
        return status;
    if (opaqueBytes.size() > MAX_CHILD_BYTES ||
        materialBytes.size() > MAX_CHILD_BYTES ||
        terrainBytes.size() > MAX_CHILD_BYTES ||
        lightingBytes.size() > MAX_CHILD_BYTES)
        return invalid("production frame child packet exceeds format limit");

    encoded.clear();
    encoded.insert(encoded.end(), MAGIC.begin(), MAGIC.end());
    appendU32(encoded, packet.version);
    appendU32(encoded, packet.passes);
    appendU64(encoded, packet.frameId);
    appendU64(encoded, packet.assemblyEpoch);
    appendU32(encoded, packet.sourceWidth);
    appendU32(encoded, packet.sourceHeight);
    appendU64(encoded, opaqueBytes.size());
    appendU64(encoded, materialBytes.size());
    appendU64(encoded, terrainBytes.size());
    appendU64(encoded, lightingBytes.size());
    encoded.insert(encoded.end(), opaqueBytes.begin(), opaqueBytes.end());
    encoded.insert(encoded.end(), materialBytes.begin(), materialBytes.end());
    encoded.insert(encoded.end(), terrainBytes.begin(), terrainBytes.end());
    encoded.insert(encoded.end(), lightingBytes.begin(), lightingBytes.end());
    return Status::success();
}

Status decodeProductionFramePacket(std::span<const std::byte> encoded,
                                   ProductionFramePacket& packet)
{
    Reader reader(encoded);
    ProductionFramePacket output;
    std::array<std::byte, 8> magic{};
    std::uint64_t opaqueSize = 0, materialSize = 0, terrainSize = 0,
                  lightingSize = 0;
    if (!reader.bytes(magic) || magic != MAGIC ||
        !reader.u32(output.version) || !reader.u32(output.passes) ||
        !reader.u64(output.frameId) || !reader.u64(output.assemblyEpoch) ||
        !reader.u32(output.sourceWidth) || !reader.u32(output.sourceHeight) ||
        !reader.u64(opaqueSize) ||
        !reader.u64(materialSize) || !reader.u64(terrainSize) ||
        !reader.u64(lightingSize) || opaqueSize > MAX_CHILD_BYTES ||
        materialSize > MAX_CHILD_BYTES ||
        terrainSize > MAX_CHILD_BYTES || lightingSize > MAX_CHILD_BYTES)
        return invalid("truncated or unrecognized production frame header");

    std::span<const std::byte> opaqueBytes, materialBytes, terrainBytes,
                               lightingBytes;
    if (!reader.view(opaqueSize, opaqueBytes) ||
        !reader.view(materialSize, materialBytes) ||
        !reader.view(terrainSize, terrainBytes) ||
        !reader.view(lightingSize, lightingBytes) || !reader.finished())
        return invalid("truncated production frame child packets");
    Status status = decodeOpaqueScenePacket(opaqueBytes, output.opaque);
    if (!status) return status;
    status = decodeMaterialScenePacket(materialBytes, output.materials);
    if (!status) return status;
    status = decodeTerrainScenePacket(terrainBytes, output.terrain);
    if (!status) return status;
    status = decodeLightingScenePacket(lightingBytes, output.lighting);
    if (!status) return status;
    status = validateProductionFramePacket(output);
    if (!status) return status;
    packet = std::move(output);
    return Status::success();
}

std::string productionFramePacketSha256(const ProductionFramePacket& packet)
{
    std::vector<std::byte> encoded;
    const Status status = encodeProductionFramePacket(packet, encoded);
    return status ? sha256(encoded) : std::string{};
}

} // namespace LL::GHI
