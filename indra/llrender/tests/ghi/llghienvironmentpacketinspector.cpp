/**
 * @file llghienvironmentpacketinspector.cpp
 * @brief Decode and summarize one immutable P0e2 environment packet.
 */

#include "linden_common.h"

#include "ghi/include/llghienvironmentscenepacket.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <vector>

namespace
{
std::array<float, 4> transform(const std::array<float, 16>& matrix,
                               const std::array<float, 4>& value)
{
    std::array<float, 4> result{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result[row] += matrix[column * 4 + row] * value[column];
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    using namespace LL::GHI;
    if (argc != 2)
    {
        std::cerr << "usage: llrender_environment_packet_inspector <packet>\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    if (!input && !input.eof())
    {
        std::cerr << "unable to read environment packet\n";
        return 2;
    }
    std::vector<std::byte> encoded(raw.size());
    std::transform(raw.begin(), raw.end(), encoded.begin(),
                   [](char value)
                   { return static_cast<std::byte>(
                         static_cast<unsigned char>(value)); });
    EnvironmentScenePacket packet;
    const Status status = decodeEnvironmentScenePacket(encoded, packet);
    if (!status)
    {
        std::cerr << status.message() << '\n';
        return 1;
    }
    std::uint64_t textureBytes = 0;
    std::uint32_t comparableTextures = 0;
    for (const auto& texture : packet.textures)
    {
        textureBytes += texture.decodedPixels.size();
        if (texture.comparability == ResourceComparability::Comparable)
            ++comparableTextures;
    }
    const auto dependencyStats = [&packet](EnvironmentTextureSemantic semantic)
    {
        struct Stats
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint64_t nonzero = 0;
            std::uint32_t minimum = 0;
            std::uint32_t maximum = 0;
        } result;
        const auto binding = std::find_if(
            packet.water.textures.begin(), packet.water.textures.end(),
            [semantic](const auto& value) { return value.semantic == semantic; });
        if (binding == packet.water.textures.end() ||
            binding->texture >= packet.textures.size())
            return result;
        const auto& texture = packet.textures[binding->texture];
        result.width = texture.width;
        result.height = texture.height;
        if (texture.decodedPixels.empty()) return result;
        const auto [minimum, maximum] = std::minmax_element(
            texture.decodedPixels.begin(), texture.decodedPixels.end());
        result.minimum = std::to_integer<std::uint32_t>(*minimum);
        result.maximum = std::to_integer<std::uint32_t>(*maximum);
        result.nonzero = static_cast<std::uint64_t>(std::count_if(
            texture.decodedPixels.begin(), texture.decodedPixels.end(),
            [](std::byte value) { return value != std::byte{}; }));
        return result;
    };
    const auto reflection = dependencyStats(
        EnvironmentTextureSemantic::ReflectionColor);
    const auto exclusion = dependencyStats(
        EnvironmentTextureSemantic::WaterExclusionMask);
    std::uint64_t waterClipSamples = 0;
    std::uint64_t waterViewportSamples = 0;
    std::uint64_t waterIdentityViewportSamples = 0;
    std::array<float, 2> waterNdcX{{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest()}};
    std::array<float, 2> waterNdcY = waterNdcX;
    for (const auto& draw : packet.waterDraws)
    {
        for (std::uint32_t item = 0; item < draw.indexCount; ++item)
        {
            const std::uint32_t index =
                packet.waterIndices[draw.firstIndex + item];
            const auto& position = packet.waterVertices[index].position;
            std::array<float, 4> value{{
                position[0], position[1], position[2], 1.f}};
            value = transform(draw.modelTransform, value);
            value = transform(packet.viewMatrix, value);
            value = transform(packet.projectionMatrix, value);
            ++waterClipSamples;
            if (value[3] <= 0.f) continue;
            const float x = value[0] / value[3];
            const float y = value[1] / value[3];
            waterNdcX[0] = std::min(waterNdcX[0], x);
            waterNdcX[1] = std::max(waterNdcX[1], x);
            waterNdcY[0] = std::min(waterNdcY[0], y);
            waterNdcY[1] = std::max(waterNdcY[1], y);
            if (x >= -1.f && x <= 1.f && y >= -1.f && y <= 1.f &&
                value[2] >= -value[3] && value[2] <= value[3])
                ++waterViewportSamples;

            value = {{position[0], position[1], position[2], 1.f}};
            value = transform(packet.viewMatrix, value);
            value = transform(packet.projectionMatrix, value);
            if (value[3] > 0.f && value[0] >= -value[3] &&
                value[0] <= value[3] && value[1] >= -value[3] &&
                value[1] <= value[3] && value[2] >= -value[3] &&
                value[2] <= value[3])
                ++waterIdentityViewportSamples;
        }
    }
    std::cout
        << "{\n"
        << "  \"status\": \"PASS\",\n"
        << "  \"sha256\": \"" << environmentScenePacketSha256(packet) << "\",\n"
        << "  \"frame_id\": " << packet.frameId << ",\n"
        << "  \"scene_epoch\": " << packet.sceneEpoch << ",\n"
        << "  \"resource_epoch\": " << packet.resourceEpoch << ",\n"
        << "  \"extent\": [" << packet.sourceWidth << ", "
        << packet.sourceHeight << "],\n"
        << "  \"view_kind\": " << static_cast<std::uint32_t>(packet.viewKind) << ",\n"
        << "  \"pass_mask\": " << packet.passMask << ",\n"
        << "  \"dependency_mask\": " << packet.dependencyMask << ",\n"
        << "  \"textures\": " << packet.textures.size() << ",\n"
        << "  \"comparable_textures\": " << comparableTextures << ",\n"
        << "  \"texture_bytes\": " << textureBytes << ",\n"
        << "  \"reflection_extent\": [" << reflection.width << ", "
        << reflection.height << "],\n"
        << "  \"reflection_nonzero_samples\": " << reflection.nonzero << ",\n"
        << "  \"reflection_range\": [" << reflection.minimum << ", "
        << reflection.maximum << "],\n"
        << "  \"exclusion_extent\": [" << exclusion.width << ", "
        << exclusion.height << "],\n"
        << "  \"exclusion_nonzero_samples\": " << exclusion.nonzero << ",\n"
        << "  \"exclusion_range\": [" << exclusion.minimum << ", "
        << exclusion.maximum << "],\n"
        << "  \"sky_vertices\": " << packet.skyVertices.size() << ",\n"
        << "  \"sky_indices\": " << packet.skyIndices.size() << ",\n"
        << "  \"sky_draws\": " << packet.skyDraws.size() << ",\n"
        << "  \"water_vertices\": " << packet.waterVertices.size() << ",\n"
        << "  \"water_indices\": " << packet.waterIndices.size() << ",\n"
        << "  \"water_draws\": " << packet.waterDraws.size() << ",\n"
        << "  \"water_clip_samples\": " << waterClipSamples << ",\n"
        << "  \"water_viewport_samples\": " << waterViewportSamples << ",\n"
        << "  \"water_identity_viewport_samples\": "
        << waterIdentityViewportSamples << ",\n"
        << "  \"water_ndc_x\": [" << waterNdcX[0] << ", "
        << waterNdcX[1] << "],\n"
        << "  \"water_ndc_y\": [" << waterNdcY[0] << ", "
        << waterNdcY[1] << "]\n"
        << "}\n";
    return 0;
}
