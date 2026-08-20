#include "ghi/core/llghishaderpackage.h"
#include "ghi/core/llghihash.h"
#include "ghi/include/llghi.h"
#include "tests/ghi/llghiproductionalphaharness.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifndef LL_GHI_P0_ALPHA_SHADER_PACKAGE
#error LL_GHI_P0_ALPHA_SHADER_PACKAGE must name the production alpha package
#endif
#ifndef LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE
#error LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE must name the legacy alpha package
#endif

int main(int argc, char** argv)
{
    std::string packetPath;
    bool validation = false;
    bool forcePPLL = false;
    bool stressPPLL = false;
    bool tailPPLL = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--packet" && index + 1 < argc)
            packetPath = argv[++index];
        else if (argument == "--validation") validation = true;
        else if (argument == "--ppll") forcePPLL = true;
        else if (argument == "--ppll-stress")
        { forcePPLL = true; stressPPLL = true; }
        else if (argument == "--ppll-tail")
        { forcePPLL = true; tailPPLL = true; }
    }
    if (packetPath.empty())
    {
        std::cerr << "usage: llrender_vulkan_production_alpha_harness --packet <file> [--validation]\n";
        return 2;
    }
    std::ifstream input(packetPath, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
    std::vector<std::byte> encoded(raw.size());
    std::transform(raw.begin(), raw.end(), encoded.begin(), [](char value)
    { return static_cast<std::byte>(static_cast<unsigned char>(value)); });
    LL::GHI::AlphaScenePacket packet;
    LL::GHI::Status status = LL::GHI::decodeAlphaScenePacket(encoded, packet);
    if ((!input && !input.eof()) || !status)
    {
        std::cerr << (status ? "could not read alpha packet" : status.message()) << '\n';
        return 3;
    }
    const std::string sourceHash = LL::GHI::sha256(encoded);
    if (forcePPLL) packet.requestedMethod = LL::GHI::AlphaMethod::PPLL;
    if (stressPPLL || tailPPLL)
        LL::GHI::Test::stressProductionAlphaPacket(packet, stressPPLL);
    LL::GHI::ShaderPackageDesc package;
    status = LL::GHI::loadShaderPackage(forcePPLL
        ? LL_GHI_P0_ALPHA_SHADER_PACKAGE
        : LL_GHI_P0_ALPHA_LEGACY_SHADER_PACKAGE, package);
    if (!status) { std::cerr << status.message() << '\n'; return 4; }
    LL::GHI::DeviceCreationResult creation = LL::GHI::createDevice(
        {LL::GHI::Backend::Vulkan, 0, 2, validation});
    if (!creation.status) { std::cerr << creation.status.message() << '\n'; return 5; }
    auto result = LL::GHI::Test::runProductionAlphaHarness(
        *creation.device, std::move(package), packet);
    if (!result.passed) { std::cerr << result.message << '\n'; return 6; }
    const auto& value = result.execution;
    std::cout << result.message << " backend=Vulkan validation="
              << (validation ? "on" : "off")
              << " frame=" << value.frameId
              << " source-sha256=" << sourceHash
              << " packet-sha256=" << value.packetSha256
              << " routes=" << value.maskDraws << ',' << value.sortedDraws
              << ',' << value.residualDraws << ',' << value.emissiveReplays
              << ',' << value.deferredDraws
              << " deferred-reasons="
              << value.deferredRouteOrMaterialDraws << ','
              << value.deferredSkinDraws << ','
              << value.deferredTextureDraws
              << " ppll=" << value.ppllAvailable << ',' << value.ppllDraws
              << ',' << value.ppllNodeCapacity << ',' << value.ppllExactLayers
              << ',' << value.ppllAllocatedNodes << ','
              << value.ppllOverflowFragments
              << " modified=" << value.modifiedPixels
              << " color-sha256=" << value.colorSha256 << '\n';
    creation.device.reset();
    return 0;
}