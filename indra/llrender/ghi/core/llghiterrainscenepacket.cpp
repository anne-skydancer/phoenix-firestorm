/**
 * @file llghiterrainscenepacket.cpp
 * @brief Deterministic I6 production-terrain packet encoding.
 */

#include "linden_common.h"

#include "ghi/include/llghiterrainscenepacket.h"
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
    std::byte{'I'}, std::byte{'T'}, std::byte{'6'}, std::byte{'B'}}};
constexpr std::uint64_t MAX_RESOURCES = 1024ull * 1024ull;
constexpr std::uint64_t MAX_DRAWS = 4ull * 1024ull * 1024ull;
constexpr std::uint64_t MAX_VERTICES = 16ull * 1024ull * 1024ull;
constexpr std::uint64_t MAX_INDICES = 48ull * 1024ull * 1024ull;
constexpr std::uint64_t MAX_PAYLOAD = 2ull * 1024ull * 1024ull * 1024ull;

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

void appendFloat(std::vector<std::byte>& out, float value)
{
    appendU32(out, std::bit_cast<std::uint32_t>(value));
}

void appendDigest(std::vector<std::byte>& out, const ResourceDigest& value)
{
    out.insert(out.end(), value.begin(), value.end());
}

class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) : mBytes(bytes) {}

    bool bytes(std::span<std::byte> out)
    {
        if (out.size() > mBytes.size() - mOffset) return false;
        std::copy_n(mBytes.begin() + static_cast<std::ptrdiff_t>(mOffset),
                    out.size(), out.begin());
        mOffset += out.size();
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

bool zeroDigest(const ResourceDigest& digest)
{
    return std::all_of(digest.begin(), digest.end(),
                       [](std::byte value) { return value == std::byte{0}; });
}

template<std::size_t N>
bool finite(const std::array<float, N>& values)
{
    return std::all_of(values.begin(), values.end(),
                       [](float value) { return std::isfinite(value); });
}

bool validTextureIndex(std::uint32_t index, std::size_t count, bool required)
{
    return (!required && index == NO_RESOURCE) || index < count;
}

bool validPacket(const TerrainScenePacket& packet)
{
    if (packet.version != TERRAIN_SCENE_PACKET_VERSION ||
        packet.textures.size() > MAX_RESOURCES ||
        packet.regions.size() > MAX_RESOURCES ||
        packet.draws.size() > MAX_DRAWS ||
        packet.vertices.size() > MAX_VERTICES ||
        packet.indices.size() > MAX_INDICES)
        return false;

    std::uint64_t payload = 0;
    for (const auto& texture : packet.textures)
    {
        payload += texture.decodedPixels.size();
        if (payload > MAX_PAYLOAD || zeroDigest(texture.sourceIdentity) ||
            (!texture.decodedPixels.empty() && zeroDigest(texture.contentIdentity)) ||
            (texture.decodedPixels.empty() && !zeroDigest(texture.contentIdentity)) ||
            (!texture.decodedPixels.empty() &&
             texture.decodedPixels.size() != static_cast<std::uint64_t>(texture.width) *
                 texture.height * texture.components))
            return false;
    }

    for (const auto& region : packet.regions)
    {
        if (zeroDigest(region.identity) || !std::isfinite(region.regionScale) ||
            region.regionScale <= 0.f || !std::isfinite(region.detailScale) ||
            region.detailScale <= 0.f ||
            !validTextureIndex(region.compositionTexture, packet.textures.size(), true) ||
            (region.projection != TerrainProjection::Planar &&
             region.projection != TerrainProjection::Triplanar) ||
            static_cast<std::uint32_t>(region.model) >
                static_cast<std::uint32_t>(MaterialModel::MetallicRoughness) ||
            static_cast<std::uint32_t>(region.paintMode) >
                static_cast<std::uint32_t>(TerrainPaintMode::PBRPaintMap) ||
            static_cast<std::uint32_t>(region.detailMode) >
                static_cast<std::uint32_t>(TerrainDetailMode::Emissive) ||
            (region.model == MaterialModel::Legacy &&
             region.paintMode != TerrainPaintMode::HeightmapWithNoise))
            return false;

        for (const auto& layer : region.layers)
        {
            if (zeroDigest(layer.identity) || layer.model != region.model ||
                !finite(layer.baseColor) || !finite(layer.emissive) ||
                !finite(layer.transform) || !std::isfinite(layer.metallic) ||
                !std::isfinite(layer.roughness) || !std::isfinite(layer.alphaCutoff) ||
                !validTextureIndex(layer.baseColorTexture, packet.textures.size(), true) ||
                !validTextureIndex(layer.normalTexture, packet.textures.size(), false) ||
                !validTextureIndex(layer.metallicRoughnessTexture,
                                   packet.textures.size(), false) ||
                !validTextureIndex(layer.emissiveTexture, packet.textures.size(), false) ||
                (region.model == MaterialModel::Legacy &&
                 (layer.normalTexture != NO_RESOURCE ||
                  layer.metallicRoughnessTexture != NO_RESOURCE ||
                  layer.emissiveTexture != NO_RESOURCE)))
                return false;
        }
    }

    for (const auto& vertex : packet.vertices)
        if (!finite(vertex.position) || !finite(vertex.normal) ||
            !finite(vertex.tangent) || !finite(vertex.compositionCoord))
            return false;

    for (std::uint32_t index : packet.indices)
        if (index >= packet.vertices.size()) return false;

    for (const auto& draw : packet.draws)
        if (draw.region >= packet.regions.size() ||
            draw.firstIndex > packet.indices.size() ||
            draw.indexCount > packet.indices.size() - draw.firstIndex ||
            !finite(draw.viewProjection) || !finite(draw.modelTransform))
            return false;

    return true;
}

void encodeTexture(std::vector<std::byte>& out,
                   const MaterialTextureResource& texture)
{
    appendDigest(out, texture.sourceIdentity);
    appendDigest(out, texture.contentIdentity);
    appendU32(out, static_cast<std::uint32_t>(texture.colorSpace));
    appendU32(out, static_cast<std::uint32_t>(texture.comparability));
    appendU32(out, texture.width);
    appendU32(out, texture.height);
    appendU32(out, texture.components);
    appendU32(out, texture.discardLevel);
    appendU64(out, texture.decodedPixels.size());
    out.insert(out.end(), texture.decodedPixels.begin(), texture.decodedPixels.end());
}

bool decodeTexture(Reader& reader, MaterialTextureResource& texture,
                   std::uint64_t& totalPayload)
{
    std::uint32_t color = 0, comparable = 0;
    std::uint64_t size = 0;
    if (!reader.bytes(texture.sourceIdentity) ||
        !reader.bytes(texture.contentIdentity) || !reader.u32(color) ||
        !reader.u32(comparable) || !reader.u32(texture.width) ||
        !reader.u32(texture.height) || !reader.u32(texture.components) ||
        !reader.u32(texture.discardLevel) || !reader.u64(size) ||
        size > MAX_PAYLOAD - totalPayload)
        return false;
    totalPayload += size;
    texture.colorSpace = static_cast<TextureColorSpace>(color);
    texture.comparability = static_cast<ResourceComparability>(comparable);
    texture.decodedPixels.resize(static_cast<std::size_t>(size));
    return reader.bytes(texture.decodedPixels);
}
} // namespace

Status encodeTerrainScenePacket(const TerrainScenePacket& packet,
                                std::vector<std::byte>& encoded)
{
    if (!validPacket(packet)) return invalid("invalid terrain scene packet");
    encoded.clear();
    encoded.insert(encoded.end(), MAGIC.begin(), MAGIC.end());
    appendU32(encoded, packet.version);
    appendU32(encoded, 0);
    appendU64(encoded, packet.frameId);
    appendU64(encoded, packet.sceneEpoch);
    appendU64(encoded, packet.resourceEpoch);
    appendU32(encoded, packet.sourceWidth);
    appendU32(encoded, packet.sourceHeight);
    appendU64(encoded, packet.textures.size());
    appendU64(encoded, packet.regions.size());
    appendU64(encoded, packet.vertices.size());
    appendU64(encoded, packet.indices.size());
    appendU64(encoded, packet.draws.size());

    for (const auto& texture : packet.textures) encodeTexture(encoded, texture);
    for (const auto& region : packet.regions)
    {
        appendDigest(encoded, region.identity);
        appendU32(encoded, static_cast<std::uint32_t>(region.model));
        appendU32(encoded, static_cast<std::uint32_t>(region.paintMode));
        appendU32(encoded, static_cast<std::uint32_t>(region.projection));
        appendU32(encoded, static_cast<std::uint32_t>(region.detailMode));
        appendU32(encoded, static_cast<std::uint32_t>(region.comparability));
        appendU32(encoded, region.compositionTexture);
        appendFloat(encoded, region.regionScale);
        appendFloat(encoded, region.detailScale);
        for (const auto& layer : region.layers)
        {
            appendDigest(encoded, layer.identity);
            appendU32(encoded, static_cast<std::uint32_t>(layer.model));
            appendU32(encoded, static_cast<std::uint32_t>(layer.comparability));
            appendU32(encoded, layer.baseColorTexture);
            appendU32(encoded, layer.normalTexture);
            appendU32(encoded, layer.metallicRoughnessTexture);
            appendU32(encoded, layer.emissiveTexture);
            for (float value : layer.baseColor) appendFloat(encoded, value);
            for (float value : layer.emissive) appendFloat(encoded, value);
            appendFloat(encoded, layer.metallic);
            appendFloat(encoded, layer.roughness);
            appendFloat(encoded, layer.alphaCutoff);
            for (float value : layer.transform) appendFloat(encoded, value);
        }
    }
    for (const auto& vertex : packet.vertices)
    {
        for (float value : vertex.position) appendFloat(encoded, value);
        for (float value : vertex.normal) appendFloat(encoded, value);
        for (float value : vertex.tangent) appendFloat(encoded, value);
        for (float value : vertex.compositionCoord) appendFloat(encoded, value);
    }
    for (std::uint32_t index : packet.indices) appendU32(encoded, index);
    for (const auto& draw : packet.draws)
    {
        appendU64(encoded, draw.semanticId);
        appendU32(encoded, draw.region);
        appendU32(encoded, static_cast<std::uint32_t>(draw.comparability));
        appendU32(encoded, draw.firstIndex);
        appendU32(encoded, draw.indexCount);
        for (float value : draw.viewProjection) appendFloat(encoded, value);
        for (float value : draw.modelTransform) appendFloat(encoded, value);
    }
    return Status::success();
}

Status decodeTerrainScenePacket(std::span<const std::byte> encoded,
                                TerrainScenePacket& packet)
{
    Reader reader(encoded);
    std::array<std::byte, 8> magic{};
    std::uint32_t reserved = 0;
    std::uint64_t textureCount = 0, regionCount = 0, vertexCount = 0;
    std::uint64_t indexCount = 0, drawCount = 0;
    TerrainScenePacket out;
    if (!reader.bytes(magic) || magic != MAGIC || !reader.u32(out.version) ||
        !reader.u32(reserved) || !reader.u64(out.frameId) ||
        !reader.u64(out.sceneEpoch) || !reader.u64(out.resourceEpoch) ||
        !reader.u32(out.sourceWidth) || !reader.u32(out.sourceHeight) ||
        !reader.u64(textureCount) || !reader.u64(regionCount) ||
        !reader.u64(vertexCount) || !reader.u64(indexCount) ||
        !reader.u64(drawCount) || out.version != TERRAIN_SCENE_PACKET_VERSION ||
        textureCount > MAX_RESOURCES || regionCount > MAX_RESOURCES ||
        vertexCount > MAX_VERTICES || indexCount > MAX_INDICES ||
        drawCount > MAX_DRAWS)
        return invalid("truncated or unrecognized terrain scene packet header");

    out.textures.resize(static_cast<std::size_t>(textureCount));
    out.regions.resize(static_cast<std::size_t>(regionCount));
    out.vertices.resize(static_cast<std::size_t>(vertexCount));
    out.indices.resize(static_cast<std::size_t>(indexCount));
    out.draws.resize(static_cast<std::size_t>(drawCount));
    std::uint64_t totalPayload = 0;
    for (auto& texture : out.textures)
        if (!decodeTexture(reader, texture, totalPayload))
            return invalid("truncated or invalid terrain texture resource");

    for (auto& region : out.regions)
    {
        std::uint32_t model = 0, paint = 0, projection = 0;
        std::uint32_t detail = 0, comparable = 0;
        if (!reader.bytes(region.identity) || !reader.u32(model) ||
            !reader.u32(paint) || !reader.u32(projection) ||
            !reader.u32(detail) || !reader.u32(comparable) ||
            !reader.u32(region.compositionTexture) ||
            !reader.floating(region.regionScale) ||
            !reader.floating(region.detailScale))
            return invalid("truncated terrain region resource");
        region.model = static_cast<MaterialModel>(model);
        region.paintMode = static_cast<TerrainPaintMode>(paint);
        region.projection = static_cast<TerrainProjection>(projection);
        region.detailMode = static_cast<TerrainDetailMode>(detail);
        region.comparability = static_cast<ResourceComparability>(comparable);
        for (auto& layer : region.layers)
        {
            std::uint32_t layerModel = 0, layerComparable = 0;
            if (!reader.bytes(layer.identity) || !reader.u32(layerModel) ||
                !reader.u32(layerComparable) ||
                !reader.u32(layer.baseColorTexture) ||
                !reader.u32(layer.normalTexture) ||
                !reader.u32(layer.metallicRoughnessTexture) ||
                !reader.u32(layer.emissiveTexture))
                return invalid("truncated terrain layer resource");
            layer.model = static_cast<MaterialModel>(layerModel);
            layer.comparability =
                static_cast<ResourceComparability>(layerComparable);
            for (float& value : layer.baseColor)
                if (!reader.floating(value)) return invalid("truncated terrain layer");
            for (float& value : layer.emissive)
                if (!reader.floating(value)) return invalid("truncated terrain layer");
            if (!reader.floating(layer.metallic) ||
                !reader.floating(layer.roughness) ||
                !reader.floating(layer.alphaCutoff))
                return invalid("truncated terrain layer factors");
            for (float& value : layer.transform)
                if (!reader.floating(value)) return invalid("truncated terrain transform");
        }
    }
    for (auto& vertex : out.vertices)
    {
        for (float& value : vertex.position)
            if (!reader.floating(value)) return invalid("truncated terrain vertex");
        for (float& value : vertex.normal)
            if (!reader.floating(value)) return invalid("truncated terrain vertex");
        for (float& value : vertex.tangent)
            if (!reader.floating(value)) return invalid("truncated terrain vertex");
        for (float& value : vertex.compositionCoord)
            if (!reader.floating(value)) return invalid("truncated terrain vertex");
    }
    for (auto& index : out.indices)
        if (!reader.u32(index)) return invalid("truncated terrain index data");
    for (auto& draw : out.draws)
    {
        std::uint32_t comparable = 0;
        if (!reader.u64(draw.semanticId) || !reader.u32(draw.region) ||
            !reader.u32(comparable) || !reader.u32(draw.firstIndex) ||
            !reader.u32(draw.indexCount))
            return invalid("truncated terrain draw record");
        draw.comparability = static_cast<ResourceComparability>(comparable);
        for (float& value : draw.viewProjection)
            if (!reader.floating(value)) return invalid("truncated terrain view transform");
        for (float& value : draw.modelTransform)
            if (!reader.floating(value)) return invalid("truncated terrain model transform");
    }
    if (!reader.finished() || !validPacket(out))
        return invalid("terrain scene packet has trailing or invalid data");
    packet = std::move(out);
    return Status::success();
}

std::string terrainScenePacketSha256(const TerrainScenePacket& packet)
{
    std::vector<std::byte> encoded;
    if (!encodeTerrainScenePacket(packet, encoded)) return {};
    return sha256(encoded);
}

} // namespace LL::GHI
