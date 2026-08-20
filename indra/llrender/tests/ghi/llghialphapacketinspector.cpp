/**
 * @file llghialphapacketinspector.cpp
 * @brief Independently decode and summarize one P0e3 alpha packet.
 */

#include "linden_common.h"

#include "ghi/include/llghialphascenepacket.h"

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv)
{
    using namespace LL::GHI;
    if (argc != 2)
    {
        std::cerr << "usage: llrender_alpha_packet_inspector <packet>\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    if (!input && !input.eof())
    {
        std::cerr << "unable to read alpha packet\n";
        return 2;
    }
    std::vector<std::byte> encoded(raw.size());
    std::transform(raw.begin(), raw.end(), encoded.begin(),
                   [](char value)
                   { return static_cast<std::byte>(
                         static_cast<unsigned char>(value)); });
    AlphaScenePacket packet;
    const Status status = decodeAlphaScenePacket(encoded, packet);
    if (!status)
    {
        std::cerr << status.message() << '\n';
        return 1;
    }

    std::array<std::uint64_t, 4> classes{};
    std::array<std::uint64_t, 5> routes{};
    std::uint64_t rigged = 0;
    std::uint64_t fullbright = 0;
    std::uint64_t emissive = 0;
    std::uint64_t legacy = 0;
    std::uint64_t pbr = 0;
    std::uint64_t productionBlend = 0;
    const AlphaRoutingState routing{packet.requestedMethod, true, true,
                                    packet.transientLoad};
    for (const auto& draw : packet.draws)
    {
        ++classes[static_cast<std::size_t>(draw.classification)];
        const AlphaSubmission submission{packet.phase, draw.classification,
                                         draw.rigged, draw.fullbright,
                                         draw.emissive};
        ++routes[static_cast<std::size_t>(
            routeAlphaSubmission(submission, routing).route)];
        rigged += draw.rigged;
        fullbright += draw.fullbright;
        emissive += draw.emissive;
        const MaterialResource& material = packet.materials.materials[
            packet.materials.draws[&draw - packet.draws.data()].material];
        legacy += material.model == MaterialModel::Legacy;
        pbr += material.model == MaterialModel::MetallicRoughness;
        productionBlend +=
            draw.blend.sourceColor == AlphaBlendFactor::SourceAlpha &&
            draw.blend.destinationColor ==
                AlphaBlendFactor::OneMinusSourceAlpha &&
            draw.blend.sourceAlpha == AlphaBlendFactor::Zero &&
            draw.blend.destinationAlpha ==
                AlphaBlendFactor::OneMinusSourceAlpha;
    }
    std::uint64_t textureBytes = 0;
    std::uint64_t comparableTextures = 0;
    for (const auto& texture : packet.materials.textures)
    {
        textureBytes += texture.decodedPixels.size();
        comparableTextures += texture.comparability ==
            ResourceComparability::Comparable;
    }

    std::cout
        << "{\n"
        << "  \"status\": \"PASS\",\n"
        << "  \"sha256\": \"" << alphaScenePacketSha256(packet) << "\",\n"
        << "  \"frame_id\": " << packet.frameId << ",\n"
        << "  \"scene_epoch\": " << packet.sceneEpoch << ",\n"
        << "  \"resource_epoch\": " << packet.resourceEpoch << ",\n"
        << "  \"extent\": [" << packet.sourceWidth << ", "
        << packet.sourceHeight << "],\n"
        << "  \"phase\": " << static_cast<std::uint32_t>(packet.phase) << ",\n"
        << "  \"requested_method\": "
        << static_cast<std::uint32_t>(packet.requestedMethod) << ",\n"
        << "  \"transient_load\": " << packet.transientLoad << ",\n"
        << "  \"draws\": " << packet.draws.size() << ",\n"
        << "  \"material_draws\": " << packet.materials.draws.size() << ",\n"
        << "  \"classes\": [" << classes[0] << ", " << classes[1]
        << ", " << classes[2] << ", " << classes[3] << "],\n"
        << "  \"routes\": [" << routes[0] << ", " << routes[1]
        << ", " << routes[2] << ", " << routes[3] << ", " << routes[4]
        << "],\n"
        << "  \"rigged\": " << rigged << ",\n"
        << "  \"fullbright\": " << fullbright << ",\n"
        << "  \"emissive\": " << emissive << ",\n"
        << "  \"legacy_draws\": " << legacy << ",\n"
        << "  \"pbr_draws\": " << pbr << ",\n"
        << "  \"production_blend_state\": " << productionBlend << ",\n"
        << "  \"textures\": " << packet.materials.textures.size() << ",\n"
        << "  \"comparable_textures\": " << comparableTextures << ",\n"
        << "  \"texture_bytes\": " << textureBytes << ",\n"
        << "  \"vertices\": " << packet.materials.vertices.size() << ",\n"
        << "  \"indices\": " << packet.materials.indices.size() << "\n"
        << "}\n";
    return 0;
}