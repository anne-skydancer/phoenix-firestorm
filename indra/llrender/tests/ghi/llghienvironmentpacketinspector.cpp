/**
 * @file llghienvironmentpacketinspector.cpp
 * @brief Decode and summarize one immutable P0e2 environment packet.
 */

#include "linden_common.h"

#include "ghi/include/llghienvironmentscenepacket.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

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
        << "  \"water_vertices\": " << packet.waterVertices.size() << ",\n"
        << "  \"water_indices\": " << packet.waterIndices.size() << ",\n"
        << "  \"water_draws\": " << packet.waterDraws.size() << "\n"
        << "}\n";
    return 0;
}
