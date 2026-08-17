/**
 * @file llghimaterialscenepacket.cpp
 * @brief Deterministic R5 material/skin observation packet encoding.
 */

#include "linden_common.h"

#include "ghi/include/llghimaterialscenepacket.h"
#include "llghihash.h"

#include <bit>
#include <limits>

namespace LL::GHI
{
namespace
{
constexpr std::array<std::byte, 8> MAGIC{{
    std::byte{'L'}, std::byte{'L'}, std::byte{'G'}, std::byte{'H'},
    std::byte{'I'}, std::byte{'M'}, std::byte{'5'}, std::byte{'B'}}};
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

bool validPacket(const MaterialScenePacket& packet, bool allowLegacy = false)
{
    if ((packet.version != MATERIAL_SCENE_PACKET_VERSION &&
         !(allowLegacy && packet.version == 1)) ||
        packet.textures.size() > MAX_RESOURCES ||
        packet.materials.size() > MAX_RESOURCES ||
        packet.skins.size() > MAX_RESOURCES || packet.draws.size() > MAX_DRAWS ||
        packet.vertices.size() > MAX_VERTICES || packet.indices.size() > MAX_INDICES)
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
    for (const auto& material : packet.materials)
    {
        if (zeroDigest(material.identity)) return false;
        for (const auto& binding : material.textures)
            if (binding.texture >= packet.textures.size()) return false;
    }
    for (const auto& skin : packet.skins)
        if (zeroDigest(skin.identity) ||
            skin.matrixPalette.size() != static_cast<std::size_t>(skin.jointCount) * 12)
            return false;
    for (const auto& draw : packet.draws)
    {
        if ((draw.material != NO_RESOURCE && draw.material >= packet.materials.size()) ||
            (draw.skin != NO_RESOURCE && draw.skin >= packet.skins.size()) ||
            draw.firstIndex > packet.indices.size() ||
            draw.indexCount > packet.indices.size() - draw.firstIndex ||
            (draw.indexCount && draw.material == NO_RESOURCE))
            return false;
    }
    for (std::uint32_t index : packet.indices)
        if (index >= packet.vertices.size()) return false;
    return true;
}

bool counts(Reader& reader, std::uint64_t& textures, std::uint64_t& materials,
            std::uint64_t& skins, std::uint64_t& draws)
{
    return reader.u64(textures) && reader.u64(materials) && reader.u64(skins) &&
           reader.u64(draws) && textures <= MAX_RESOURCES &&
           materials <= MAX_RESOURCES && skins <= MAX_RESOURCES && draws <= MAX_DRAWS;
}
} // namespace

Status encodeMaterialScenePacket(const MaterialScenePacket& packet,
                                 std::vector<std::byte>& encoded)
{
    if (!validPacket(packet)) return invalid("invalid material scene packet");
    encoded.clear();
    encoded.insert(encoded.end(), MAGIC.begin(), MAGIC.end());
    appendU32(encoded, packet.version);
    appendU32(encoded, 0);
    appendU64(encoded, packet.frameId);
    appendU64(encoded, packet.sceneEpoch);
    appendU64(encoded, packet.resourceEpoch);
    appendU64(encoded, packet.textures.size());
    appendU64(encoded, packet.materials.size());
    appendU64(encoded, packet.skins.size());
    appendU64(encoded, packet.draws.size());
    appendU32(encoded, packet.sourceWidth);
    appendU32(encoded, packet.sourceHeight);
    appendU64(encoded, packet.vertices.size());
    appendU64(encoded, packet.indices.size());
    for (const auto& texture : packet.textures)
    {
        appendDigest(encoded, texture.sourceIdentity);
        appendDigest(encoded, texture.contentIdentity);
        appendU32(encoded, static_cast<std::uint32_t>(texture.colorSpace));
        appendU32(encoded, static_cast<std::uint32_t>(texture.comparability));
        appendU32(encoded, texture.width); appendU32(encoded, texture.height);
        appendU32(encoded, texture.components); appendU32(encoded, texture.discardLevel);
        appendU64(encoded, texture.decodedPixels.size());
        encoded.insert(encoded.end(), texture.decodedPixels.begin(), texture.decodedPixels.end());
    }
    for (const auto& material : packet.materials)
    {
        appendDigest(encoded, material.identity);
        appendU32(encoded, static_cast<std::uint32_t>(material.model));
        appendU32(encoded, static_cast<std::uint32_t>(material.alphaMode));
        appendU32(encoded, static_cast<std::uint32_t>(material.comparability));
        appendU32(encoded, material.doubleSided ? 1u : 0u);
        appendU32(encoded, material.fullbright ? 1u : 0u);
        appendU32(encoded, 0);
        for (float value : material.baseColor) appendFloat(encoded, value);
        for (float value : material.emissive) appendFloat(encoded, value);
        for (float value : material.legacySpecular) appendFloat(encoded, value);
        appendFloat(encoded, material.metallic); appendFloat(encoded, material.roughness);
        appendFloat(encoded, material.alphaCutoff);
        appendFloat(encoded, material.environmentIntensity);
        appendU64(encoded, material.textures.size());
        for (const auto& binding : material.textures)
        {
            appendU32(encoded, static_cast<std::uint32_t>(binding.semantic));
            appendU32(encoded, binding.texture); appendU32(encoded, binding.texcoord);
            appendU32(encoded, 0);
            for (float value : binding.transform) appendFloat(encoded, value);
        }
    }
    for (const auto& skin : packet.skins)
    {
        appendDigest(encoded, skin.identity);
        appendU32(encoded, static_cast<std::uint32_t>(skin.comparability));
        appendU32(encoded, skin.jointCount);
        for (float value : skin.matrixPalette) appendFloat(encoded, value);
    }
    for (const auto& vertex : packet.vertices)
    {
        for (float value : vertex.position) appendFloat(encoded, value);
        for (float value : vertex.normal) appendFloat(encoded, value);
        for (float value : vertex.tangent) appendFloat(encoded, value);
        for (float value : vertex.texCoord) appendFloat(encoded, value);
        for (std::uint8_t value : vertex.color)
            encoded.push_back(static_cast<std::byte>(value));
        for (std::uint16_t value : vertex.joints)
        {
            encoded.push_back(static_cast<std::byte>(value & 0xffu));
            encoded.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
        }
        for (float value : vertex.weights) appendFloat(encoded, value);
    }
    for (std::uint32_t index : packet.indices) appendU32(encoded, index);
    for (const auto& draw : packet.draws)
    {
        appendU64(encoded, draw.semanticId); appendU32(encoded, draw.material);
        appendU32(encoded, draw.skin);
        appendU32(encoded, static_cast<std::uint32_t>(draw.comparability));
        appendU32(encoded, 0);
        appendU32(encoded, draw.firstIndex);
        appendU32(encoded, draw.indexCount);
        for (float value : draw.transform) appendFloat(encoded, value);
        for (float value : draw.modelTransform) appendFloat(encoded, value);
    }
    return Status::success();
}

Status decodeMaterialScenePacket(std::span<const std::byte> encoded,
                                 MaterialScenePacket& packet)
{
    Reader reader(encoded);
    std::array<std::byte, 8> magic{};
    std::uint32_t reserved = 0;
    std::uint64_t textureCount = 0, materialCount = 0, skinCount = 0, drawCount = 0;
    std::uint64_t vertexCount = 0, indexCount = 0;
    MaterialScenePacket out;
    if (!reader.bytes(magic) || magic != MAGIC || !reader.u32(out.version) ||
        !reader.u32(reserved) || !reader.u64(out.frameId) ||
        !reader.u64(out.sceneEpoch) || !reader.u64(out.resourceEpoch) ||
        !counts(reader, textureCount, materialCount, skinCount, drawCount))
        return invalid("truncated or unrecognized material scene packet header");
    if (out.version == MATERIAL_SCENE_PACKET_VERSION)
    {
        if (!reader.u32(out.sourceWidth) || !reader.u32(out.sourceHeight) ||
            !reader.u64(vertexCount) || !reader.u64(indexCount) ||
            vertexCount > MAX_VERTICES || indexCount > MAX_INDICES)
            return invalid("truncated or invalid material geometry header");
    }
    else if (out.version != 1)
    {
        return invalid("unsupported material scene packet version");
    }
    out.textures.resize(static_cast<std::size_t>(textureCount));
    out.materials.resize(static_cast<std::size_t>(materialCount));
    out.skins.resize(static_cast<std::size_t>(skinCount));
    out.vertices.resize(static_cast<std::size_t>(vertexCount));
    out.indices.resize(static_cast<std::size_t>(indexCount));
    out.draws.resize(static_cast<std::size_t>(drawCount));
    std::uint64_t totalPayload = 0;
    for (auto& texture : out.textures)
    {
        std::uint32_t color = 0, comparable = 0;
        std::uint64_t size = 0;
        if (!reader.bytes(texture.sourceIdentity) || !reader.bytes(texture.contentIdentity) ||
            !reader.u32(color) || !reader.u32(comparable) ||
            !reader.u32(texture.width) || !reader.u32(texture.height) ||
            !reader.u32(texture.components) || !reader.u32(texture.discardLevel) ||
            !reader.u64(size) || size > MAX_PAYLOAD - totalPayload)
            return invalid("truncated or invalid material texture resource");
        totalPayload += size;
        texture.colorSpace = static_cast<TextureColorSpace>(color);
        texture.comparability = static_cast<ResourceComparability>(comparable);
        texture.decodedPixels.resize(static_cast<std::size_t>(size));
        if (!reader.bytes(texture.decodedPixels))
            return invalid("truncated material texture pixels");
    }
    for (auto& material : out.materials)
    {
        std::uint32_t model = 0, alpha = 0, comparable = 0, doubleSided = 0,
                      fullbright = 0, ignored = 0;
        std::uint64_t bindingCount = 0;
        if (!reader.bytes(material.identity) || !reader.u32(model) ||
            !reader.u32(alpha) || !reader.u32(comparable) ||
            !reader.u32(doubleSided) || !reader.u32(fullbright) ||
            !reader.u32(ignored)) return invalid("truncated material resource");
        material.model = static_cast<MaterialModel>(model);
        material.alphaMode = static_cast<MaterialAlphaMode>(alpha);
        material.comparability = static_cast<ResourceComparability>(comparable);
        material.doubleSided = doubleSided != 0; material.fullbright = fullbright != 0;
        for (float& value : material.baseColor) if (!reader.floating(value)) return invalid("truncated material factors");
        for (float& value : material.emissive) if (!reader.floating(value)) return invalid("truncated material factors");
        for (float& value : material.legacySpecular) if (!reader.floating(value)) return invalid("truncated material factors");
        if (!reader.floating(material.metallic) || !reader.floating(material.roughness) ||
            !reader.floating(material.alphaCutoff) ||
            !reader.floating(material.environmentIntensity) ||
            !reader.u64(bindingCount) || bindingCount > MAX_RESOURCES)
            return invalid("truncated material factors or bindings");
        material.textures.resize(static_cast<std::size_t>(bindingCount));
        for (auto& binding : material.textures)
        {
            std::uint32_t semantic = 0;
            if (!reader.u32(semantic) || !reader.u32(binding.texture) ||
                !reader.u32(binding.texcoord) || !reader.u32(ignored))
                return invalid("truncated material texture binding");
            binding.semantic = static_cast<TextureSemantic>(semantic);
            for (float& value : binding.transform)
                if (!reader.floating(value)) return invalid("truncated texture transform");
        }
    }
    for (auto& skin : out.skins)
    {
        std::uint32_t comparable = 0;
        if (!reader.bytes(skin.identity) || !reader.u32(comparable) ||
            !reader.u32(skin.jointCount) || skin.jointCount > MAX_RESOURCES)
            return invalid("truncated or invalid skin resource");
        skin.comparability = static_cast<ResourceComparability>(comparable);
        skin.matrixPalette.resize(static_cast<std::size_t>(skin.jointCount) * 12);
        for (float& value : skin.matrixPalette)
            if (!reader.floating(value)) return invalid("truncated skin palette");
    }
    for (auto& vertex : out.vertices)
    {
        for (float& value : vertex.position)
            if (!reader.floating(value)) return invalid("truncated material vertex");
        for (float& value : vertex.normal)
            if (!reader.floating(value)) return invalid("truncated material vertex");
        for (float& value : vertex.tangent)
            if (!reader.floating(value)) return invalid("truncated material vertex");
        for (float& value : vertex.texCoord)
            if (!reader.floating(value)) return invalid("truncated material vertex");
        std::array<std::byte, 4> color{};
        if (!reader.bytes(color)) return invalid("truncated material vertex color");
        for (std::size_t i = 0; i < color.size(); ++i)
            vertex.color[i] = std::to_integer<std::uint8_t>(color[i]);
        std::array<std::byte, 8> joints{};
        if (!reader.bytes(joints)) return invalid("truncated material vertex joints");
        for (std::size_t i = 0; i < vertex.joints.size(); ++i)
            vertex.joints[i] = std::to_integer<std::uint16_t>(joints[i * 2]) |
                (std::to_integer<std::uint16_t>(joints[i * 2 + 1]) << 8);
        for (float& value : vertex.weights)
            if (!reader.floating(value)) return invalid("truncated material vertex");
    }
    for (std::uint32_t& index : out.indices)
        if (!reader.u32(index)) return invalid("truncated material index data");
    for (auto& draw : out.draws)
    {
        std::uint32_t comparable = 0, ignored = 0;
        if (!reader.u64(draw.semanticId) || !reader.u32(draw.material) ||
            !reader.u32(draw.skin) || !reader.u32(comparable) || !reader.u32(ignored))
            return invalid("truncated material draw record");
        draw.comparability = static_cast<ResourceComparability>(comparable);
        if (out.version == MATERIAL_SCENE_PACKET_VERSION)
        {
            if (!reader.u32(draw.firstIndex) || !reader.u32(draw.indexCount))
                return invalid("truncated material draw geometry");
            for (float& value : draw.transform)
                if (!reader.floating(value))
                    return invalid("truncated material draw transform");
            for (float& value : draw.modelTransform)
                if (!reader.floating(value))
                    return invalid("truncated material draw model transform");
        }
    }
    if (!reader.finished() || !validPacket(out, true))
        return invalid("material scene packet has trailing or invalid data");
    packet = std::move(out);
    return Status::success();
}

std::string materialScenePacketSha256(const MaterialScenePacket& packet)
{
    std::vector<std::byte> encoded;
    if (!encodeMaterialScenePacket(packet, encoded)) return {};
    return sha256(encoded);
}

} // namespace LL::GHI
