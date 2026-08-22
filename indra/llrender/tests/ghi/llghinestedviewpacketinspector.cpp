/**
 * @file llghinestedviewpacketinspector.cpp
 * @brief Decode and summarize one immutable P0e4 nested-view packet.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "ghi/include/llghinestedviewscenepacket.h"

#include <algorithm>
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
        std::cerr << "usage: llrender_nested_view_packet_inspector <packet>\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    if (!input && !input.eof())
    {
        std::cerr << "unable to read nested-view packet\n";
        return 2;
    }
    std::vector<std::byte> encoded(raw.size());
    std::transform(raw.begin(), raw.end(), encoded.begin(),
                   [](char value)
                   { return static_cast<std::byte>(
                         static_cast<unsigned char>(value)); });
    NestedViewScenePacket packet;
    const Status status = decodeNestedViewScenePacket(encoded, packet);
    if (!status)
    {
        std::cerr << status.message() << '\n';
        return 1;
    }
    std::array<std::size_t, RENDER_VIEW_CLASS_COUNT> views{};
    for (const NestedViewPass& nested : packet.passes)
        ++views[static_cast<std::size_t>(nested.pass.view)];
    std::cout
        << "{\n"
        << "  \"status\": \"PASS\",\n"
        << "  \"sha256\": \"" << nestedViewScenePacketSha256(packet) << "\",\n"
        << "  \"frame_id\": " << packet.frameId << ",\n"
        << "  \"scene_generation\": " << packet.sceneGeneration << ",\n"
        << "  \"resource_generation\": " << packet.resourceGeneration << ",\n"
        << "  \"passes\": " << packet.passes.size() << ",\n"
        << "  \"reflection_probe\": " << views[1] << ",\n"
        << "  \"hero_probe\": " << views[2] << ",\n"
        << "  \"mirror\": " << views[3] << ",\n"
        << "  \"cube_snapshot\": " << views[4] << ",\n"
        << "  \"impostor\": " << views[5] << ",\n"
        << "  \"dynamic_texture\": " << views[6] << ",\n"
        << "  \"preview\": " << views[7] << ",\n"
        << "  \"pre_water_alpha\": " << views[8] << ",\n"
        << "  \"media_surface\": " << views[9] << "\n"
        << "}\n";
    return 0;
}